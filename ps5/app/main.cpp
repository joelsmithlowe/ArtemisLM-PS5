// PS5LM native app: llama-server on the PS5's GPU through RADV (ggml-vulkan),
// with a dashboard on the TV drawn by ps5-homebrew-ui through ps5-opengl.
//
// A title gets the console's direct memory (12 GiB) where a payload gets none,
// and RADV turns it into Vulkan device memory, so the whole model sits on the
// GPU. llama-server runs on its own thread on port 8081 (chat page and OpenAI
// API) once a model is loaded. With nothing loaded, that port serves the page
// on its own, so a model can be downloaded or a folder added. The main thread
// draws the dashboard, reads the controller and plays the interface sounds.
// Models come from /data/ArtemisLM/models and USB drives; the
// planner (model_plan.cpp) picks each one's context and cache for the memory
// free. The home folder is /data/ArtemisLM/models. PS5LM and PS5LM Plus are
// read from /data/PS5LM/models when that switch is on. The phone page's
// library listens on port 8082. /data/ArtemisLM/app-args.txt, one argument
// per line, overrides the planner.
// The app logs to /data/ArtemisLM/app.log and llama.log.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <GL/glcorearb.h>

#include "dashboard.hpp"
#include "fetch.hpp"
#include "library.hpp"
#include "market.hpp"
#include "memory.hpp"
#include "projects.hpp"
#include "panel.hpp"
#include "ports.hpp"
#include "display.hpp"
#include "ggml-backend.h"
#include "model_plan.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "suggested.hpp"
#include "update.hpp"
#include "version.hpp"

#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>

#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

int  llama_server(int argc, char ** argv);
void llama_server_terminate();

// ---- Vulkan without a loader ----------------------------------------------------
// RADV is linked in whole, and ggml-vulkan's dispatcher starts from
// vkGetInstanceProcAddr, which the ICD entry point answers for every command.
// The few commands ggml calls by name go to Mesa's common implementations,
// which dispatch through the object's own table.
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char * name);
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}
extern "C" PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice device, const char * name);
extern "C" void vk_common_GetPhysicalDeviceFeatures2(VkPhysicalDevice dev, VkPhysicalDeviceFeatures2 * features);
extern "C" void vk_common_CmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t n,
                                        const VkBufferCopy * regions);
extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char * name) {
    return vk_common_GetDeviceProcAddr(device, name);
}
extern "C" void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice dev, VkPhysicalDeviceFeatures2 * features) {
    vk_common_GetPhysicalDeviceFeatures2(dev, features);
}
extern "C" void vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t n,
                                const VkBufferCopy * regions) {
    vk_common_CmdCopyBuffer(cmd, src, dst, n, regions);
}

extern "C" {
int sceSystemServiceHideSplashScreen(void);
int sceKernelSendNotificationRequest(uint32_t device, void * request, size_t size, int blocking);
int sceKernelUsleep(uint32_t microseconds);
int sceSystemServiceLoadExec(const char * path, const char * const * args);
}

namespace {

using namespace ps5lm;

constexpr const char * kLog         = "/data/ArtemisLM/app.log";
constexpr const char * kArgs        = "/data/ArtemisLM/app-args.txt";
constexpr const char * kChoice      = "/data/ArtemisLM/model.txt";
constexpr const char * kEnv         = "/data/ArtemisLM/env.txt";  // driver and ggml switches, for measuring
constexpr const char * kTextOnly    = "/data/ArtemisLM/text-only";  // the last load's projector failed: load without it once
constexpr const char * kQuit        = "/data/ArtemisLM/quit";
constexpr const char * kSettings    = "/data/ArtemisLM/settings.json";
constexpr const char * kScratch     = "/data/ArtemisLM/scratch";
// File tools only. A title cannot start a process, so the shell tool is
// never offered. Web search and the clock are separate: they run in this
// process (libSceHttp2 and the C clock), wired in by patches/0008.
constexpr const char * kFileTools = "read_file,write_file,edit_file,file_glob_search,grep_search,get_info";

std::string tool_list(const Settings & s) {
    std::string tools;
    const auto add = [&](const char * name) {
        if (!tools.empty()) {
            tools += ',';
        }
        tools += name;
    };
    if (s.tools) {
        tools = kFileTools;
    }
    if (s.web_search) {
        add("web_search");
        add("web_fetch");
    }
    if (s.datetime) {
        add("get_datetime");
    }
    return tools;
}

RunOptions run_options(const Settings & s) {
    RunOptions r;
    r.flash_attn = s.flash_attn;
    r.batch = s.batch;
    r.slots = s.slots;
    r.sampling = s.sampling;
    r.max_tokens = s.max_tokens;
    r.reasoning = s.reasoning;
    r.system_prompt = s.system_prompt;
    r.memory = s.memory;
    r.repeat_penalty = s.repeat_penalty;
    r.min_p = s.min_p;
    r.seed = s.seed;
    return r;
}
constexpr const char * kLlamaLog    = "/data/ArtemisLM/llama.log";
constexpr const char * kModels      = "/data/ArtemisLM/models";
constexpr const char * kAssets      = "/app0/assets";
constexpr int          kPort        = kChatPort;

// The last lines of a log, up to about 64 KiB of it, oldest first.
std::vector<std::string> tail_lines(const char * path) {
    std::vector<std::string> lines;
    FILE * f = std::fopen(path, "rb");
    if (!f) {
        return lines;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long from = std::max(0L, size - 65536L);
    std::fseek(f, from, SEEK_SET);
    std::string text((size_t) (size - from), '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f));
    std::fclose(f);
    size_t start = from > 0 ? text.find('\n') + 1 : 0;  // the first line is cut
    for (size_t end; start < text.size() && (end = text.find('\n', start)) != std::string::npos; start = end + 1) {
        lines.emplace_back(text, start, end - start);
    }
    return lines;
}

// Probe: download a URL over HTTPS to /data/ArtemisLM/fetch.out, logging the
// status, size and speed (scripts/artemislm-ps5.sh fetch <url>).
void probe_fetch(std::string url) {
    FILE * out = std::fopen("/data/ArtemisLM/fetch.out", "wb");
    const FetchResult r = https_get(url, "", [&](const char * p, size_t n) { return out && std::fwrite(p, 1, n, out) == n; });
    if (out) {
        std::fclose(out);
    }
    std::printf("artemislm-ps5: fetch %s: status %d, length %lld, %llu bytes in %.1f s (%.2f MB/s)%s%s\n", url.c_str(),
                r.status, (long long) r.length, (unsigned long long) r.bytes, r.seconds,
                r.seconds > 0 ? r.bytes / 1e6 / r.seconds : 0.0, r.error.empty() ? "" : ", ", r.error.c_str());
}

// Probe: how fast the app can write, by method and place, fsync included
// (scripts/artemislm-ps5.sh bench-write). 128 MiB each.
void probe_write() {
    const size_t total = 128u << 20;
    std::vector<char> buf(8u << 20, 'x');
    const char * dirs[] = { "/data/ArtemisLM", "/mnt/usb0" };
    for (const char * dir : dirs) {
        struct statvfs v = {};
        if (statvfs(dir, &v) != 0) {
            continue;
        }
        const std::string path = std::string(dir) + "/write-bench.tmp";
        struct Method {
            const char * name;
            size_t       chunk;
            bool         stdio;
            int          flags;
        } methods[] = {
            { "stdio 8M", 8u << 20, true, 0 },
            { "write 1M", 1u << 20, false, 0 },
            { "write 8M", 8u << 20, false, 0 },
            { "write 8M direct", 8u << 20, false, O_DIRECT },
        };
        for (const auto & m : methods) {
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = true;
            if (m.stdio) {
                FILE * f = std::fopen(path.c_str(), "wb");
                for (size_t done = 0; f && ok && done < total; done += m.chunk) {
                    ok = std::fwrite(buf.data(), 1, m.chunk, f) == m.chunk;
                }
                ok = f && ok && std::fflush(f) == 0 && fsync(fileno(f)) == 0;
                if (f) {
                    std::fclose(f);
                }
            } else {
                const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | m.flags, 0666);
                for (size_t done = 0; fd >= 0 && ok && done < total; done += m.chunk) {
                    ok = write(fd, buf.data(), m.chunk) == (ssize_t) m.chunk;
                }
                ok = fd >= 0 && ok && fsync(fd) == 0;
                if (fd >= 0) {
                    close(fd);
                }
            }
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("artemislm-ps5: write-bench %s %s: %s, %.1f MB/s\n", dir, m.name, ok ? "ok" : strerror(errno),
                        ok ? total / 1e6 / s : 0.0);
            unlink(path.c_str());
        }
    }
    std::printf("artemislm-ps5: write-bench done\n");
}

// Bytes under a folder, or 0.
uint64_t folder_bytes(const char * path) {
    std::error_code ec;
    uint64_t total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(path, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return total;
}

// A system notification on the TV.
void notify(const char * text) {
    struct {
        uint8_t reserved[45];
        char    message[3075];
    } request = {};
    std::snprintf(request.message, sizeof(request.message), "%s", text);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// A quit request from the PC (scripts/artemislm-ps5.sh close): the app ends
// itself the way the system expects; killing a title that is rendering has
// been followed by lost consoles (ps5-homebrew-ui's AGENTS.md).
void quit_if_asked() {
    if (access(kQuit, F_OK) != 0) {
        return;
    }
    unlink(kQuit);
    std::printf("artemislm-ps5: quit requested\n");
    std::fflush(nullptr);
    sceSystemServiceLoadExec("exit", nullptr);
}

// The screen as a 24-bit BMP (glReadPixels of the TV's framebuffer), for
// checking the dashboard from the PC.
bool save_screen(const char * path, int width, int height) {
    std::vector<unsigned char> pixels((size_t) width * height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const uint32_t row = ((uint32_t) width * 3 + 3) & ~3u;
    const uint32_t size = 54 + row * (uint32_t) height;
    unsigned char header[54] = { 'B', 'M' };
    auto put = [&](int at, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            header[at + i] = (unsigned char) (v >> (8 * i));
        }
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, (uint32_t) width);
    put(22, (uint32_t) height);
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    FILE * f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y) {
        const unsigned char * in = pixels.data() + (size_t) y * width * 4;
        for (int x = 0; x < width; ++x) {
            line[(size_t) x * 3 + 0] = in[x * 4 + 2];
            line[(size_t) x * 3 + 1] = in[x * 4 + 1];
            line[(size_t) x * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, row, f) == row;
    }
    std::fclose(f);
    return ok;
}

// A request file from the PC: its first line, and the file is consumed.
std::string take_request_file(const char * path) {
    if (access(path, F_OK) != 0) {
        return {};
    }
    std::string line;
    std::ifstream(path) >> line;
    unlink(path);
    return line.empty() ? std::string("-") : line;
}

// Where the memory is: the pool's free part, the CPU heap, and what ggml
// says about each device (its GPU figure wraps once the unified heap is
// over-committed; the raw numbers still show a change).
void log_memory(const char * when) {
    int64_t at = 0;
    size_t  avail = 0;
    const int64_t pool = sceKernelGetDirectMemorySize();
    sceKernelAvailableDirectMemorySize(0, pool, 2 * 1024 * 1024, &at, &avail);
    struct ps5_heap_stats heap = {};
    ps5_heap_stats(&heap);
    std::printf("artemislm-ps5: memory %s: pool free %.2f GiB, cpu heap %.2f GiB (peak %.2f)", when, avail / 1073741824.0,
                heap.mapped_bytes / 1073741824.0, heap.peak_bytes / 1073741824.0);
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        size_t free = 0, total = 0;
        ggml_backend_dev_memory(ggml_backend_dev_get(i), &free, &total);
        std::printf(", %s free %zu total %zu", ggml_backend_dev_name(ggml_backend_dev_get(i)), free, total);
    }
    std::printf("\n");
}

std::vector<std::string> read_lines(const char * path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            out.push_back(line);
        }
    }
    return out;
}

// Free GiB on the drive whose models folder is `dir`, or -1 when unknown.
double free_gib(const std::string & dir) {
    for (const auto & d : mounted_drives()) {
        if (d.models == dir && d.total > 0) {
            return d.free;
        }
    }
    return -1;
}

std::string base_name(const std::string & path) {
    std::string n = path.substr(path.find_last_of('/') + 1);
    if (n.size() > 5 && n.compare(n.size() - 5, 5, ".gguf") == 0) {
        n.resize(n.size() - 5);
    }
    return n;
}

// ---- the server thread ------------------------------------------------------------

std::atomic<int> g_server_rc{ -1 };   // llama-server's exit code once it returns; -1 while it runs
std::vector<std::string> g_server_args;

void * run_server(void *) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("llama-server"));
    for (auto & a : g_server_args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    int rc = 1;
    try {
        rc = llama_server((int) argv.size() - 1, argv.data());
    } catch (const std::exception & e) {
        std::printf("artemislm-ps5: exception: %s\n", e.what());
    } catch (...) {
        std::printf("artemislm-ps5: unknown exception\n");
    }
    std::printf("artemislm-ps5: llama_server returned %d\n", rc);
    if (rc != 0) {
        const std::vector<std::string> lines = tail_lines(kLlamaLog);
        const size_t begin = lines.size() > 12 ? lines.size() - 12 : 0;
        for (size_t i = begin; i < lines.size(); ++i) {
            std::printf("artemislm-ps5: llama.log: %s\n", lines[i].c_str());
        }
        if (lines.empty()) {
            std::printf("artemislm-ps5: llama.log is empty\n");
        }
    }
    std::fflush(nullptr);
    g_server_rc = rc == 0 ? 0 : 1;
    return nullptr;
}

// ---- models -------------------------------------------------------------------------

struct Models {
    double                 budget_gib = 0;   // GPU memory free before any model
    std::vector<ModelRow>  rows;
    std::string            running;          // path of the model llama-server has
    pthread_t              server{};
    bool                   server_started = false;
    Settings               settings;

    const char * kv_force() const { return settings.kv_type == "auto" ? nullptr : settings.kv_type.c_str(); }

    void scan(StatsCollector & stats) {
        rows = plan_rows(settings, budget_gib, running);
        publish(stats);
    }

    // scan() on its own thread, for a settings change: the dashboard shows it
    // working instead of freezing while every header is read.
    std::mutex             scan_mutex;
    std::vector<ModelRow>  scanned;
    std::atomic<int>       scan_state{ 0 };  // 0 idle, 1 running, 2 done
    bool                   scan_again = false;
    bool                   scan_shown = true;  // the dashboard says it is working

    void rescan(StatsCollector & stats, bool shown = true) {
        scan_shown = shown;
        if (scan_state != 0) {
            scan_again = true;  // the running one read the old settings
            return;
        }
        scan_state = 1;
        std::thread([this, s = settings, b = budget_gib, r = running] {
            std::vector<ModelRow> out = plan_rows(s, b, r);
            std::lock_guard<std::mutex> lock(scan_mutex);
            scanned = std::move(out);
            scan_state = 2;
        }).detach();
        (void) stats;
    }

    // Takes a finished rescan; true while one is still running.
    bool poll_rescan(StatsCollector & stats) {
        if (scan_state == 2) {
            {
                std::lock_guard<std::mutex> lock(scan_mutex);
                rows = std::move(scanned);
            }
            scan_state = 0;
            publish(stats);
            if (scan_again) {
                scan_again = false;
                rescan(stats);
            }
        }
        return scan_state != 0;
    }

    static std::vector<ModelRow> plan_rows(const Settings & settings, double budget_gib, const std::string & running) {
        std::vector<ModelRow> rows;
        const char * kv = settings.kv_type == "auto" ? nullptr : settings.kv_type.c_str();
        if (settings.migrate == "on") {
            const std::string note = library_move(settings, "", running);
            std::printf("artemislm-ps5: %s\n", note.c_str());
        }
        for (const auto & path : find_models(library_dirs(settings))) {
            const ModelInfo m = read_model_info(path);
            ModelRow row;
            row.file = path;
            row.label = base_name(path);
            row.home = model_in_home(path);
            row.folder = model_folder(path);
            row.drive = drive_name(path);
            if (!m.ok) {
                row.plan = m.error;
                rows.push_back(row);
                continue;
            }
            const Plan p = plan_model(m, budget_gib, settings.ctx_cap, kv, run_options(settings));
            const Preset * pre = find_preset(m);
            row.arch = m.arch;
            row.plan = p.why;
            row.fits = p.fits;
            row.size_gib = m.file_bytes / 1073741824.0;
            row.preset = pre ? pre->label : "";
            row.decision = m.decision;
            row.current = path == running;
            rows.push_back(row);
            std::printf("artemislm-ps5: model %s (%s): %s\n", m.file_name.c_str(), m.arch.c_str(), p.why.c_str());
        }
        return rows;
    }

    void publish(StatsCollector & stats) {
        std::vector<ModelRow> out = rows;
        for (auto & r : out) {
            r.current = r.file == running;
        }
        stats.set_models(out);
    }

    // The arguments for a model: the planner's, or app-args.txt's verbatim.
    std::vector<std::string> args_for(const std::string & path, Live & model_part) {
        const ModelInfo m = read_model_info(path);
        const Plan p = plan_model(m, budget_gib, settings.ctx_cap, kv_force(), run_options(settings));
        const Preset * pre = find_preset(m);
        model_part.model_label = base_name(path);
        model_part.model_file  = path;
        model_part.plan        = p.why;
        model_part.args        = p.args;
        model_part.model_arch  = m.arch;
        model_part.preset      = pre ? pre->label : "";
        model_part.decision    = m.decision;
        model_part.model_gib   = m.file_bytes / 1073741824.0;
        model_part.ctx         = p.ctx;
        model_part.kv_type     = p.kv_type;
        model_part.kv_gib      = p.kv_gib;
        return p.fits ? p.args : std::vector<std::string>{};
    }

    bool start(const std::string & path, std::vector<std::string> args, StatsCollector & stats, const Live & model_part) {
        // llama.cpp's own log, written as it goes (it survives a crash), and
        // the metrics the dashboard reads.
        args.insert(args.begin(), { "--log-file", "/data/ArtemisLM/llama.log" });
        if (std::find(args.begin(), args.end(), "--metrics") == args.end()) {
            args.push_back("--metrics");
        }
        // File tools stay inside the scratch folder (patches/0003). The
        // working directory is moved there only then: a title cannot read
        // the directory it starts in, and web search and the clock do not
        // use it.
        const std::string tools = tool_list(settings);
        if (!tools.empty() && std::find(args.begin(), args.end(), "--tools") == args.end()) {
            if (settings.tools) {
                std::error_code ec;
                std::filesystem::create_directories(kScratch, ec);
                setenv("LLAMA_TOOLS_ROOT", kScratch, 1);
                chdir(kScratch);
            }
            args.insert(args.end(), { "--tools", tools });
        }
        // The stock llama-ui only forwards a server tool whose name it
        // already knows. web_search is not one of those names, so the model
        // never sees it. This page calls the clock and then the search.
        if (std::find(args.begin(), args.end(), "--path") == args.end()) {
            args.insert(args.end(), { "--path", "/app0/assets/web" });
        }
        for (const auto & a : args) {
            std::printf("artemislm-ps5: arg %s\n", a.c_str());
        }
        g_server_args = args;
        g_server_rc = -1;
        running = path;
        panel_set_running(path);
        stats.set_model(model_part);
        stats.set_state(ServerState::loading);
        publish(stats);
        // Sockets opened before the console network stack is up do not accept
        // a connection from the LAN. llama-server also refuses the port once
        // and would exit, so the stack is brought up before that thread starts.
        net_up();
        // The model line is already on screen. A failed thread clears it and
        // the caller opens the idle page, so the port does not stay silent.
        auto give_up = [&](const char * why) {
            std::printf("artemislm-ps5: %s\n", why);
            running.clear();
            panel_set_running("");
            stats.set_model(Live{});
            notify("ArtemisLM PS5: the server thread did not start");
        };
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) {
            give_up("server thread setup failed");
            return false;
        }
        // The console's default stack is small. Keep the 16 MiB stack llama-server uses.
        if (pthread_attr_setstacksize(&attr, 16u << 20) != 0) {
            pthread_attr_destroy(&attr);
            give_up("server thread stack failed");
            return false;
        }
        const int rc = pthread_create(&server, &attr, run_server, nullptr);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            char why[80];
            std::snprintf(why, sizeof(why), "server thread failed to start (%d)", rc);
            give_up(why);
            return false;
        }
        server_started = true;
        notify("ArtemisLM PS5: loading the model onto the GPU");
        return true;
    }

    void stop() {
        if (!server_started) {
            return;
        }
        llama_server_terminate();
        // With a reply in flight llama-server can stay inside its HTTP thread
        // pool forever (unload during generation hung the app). We are about
        // to be replaced by LoadExec anyway, so wait 10 s at most.
        for (int i = 0; i < 100 && g_server_rc < 0; ++i) {
            usleep(100 * 1000);
        }
        if (g_server_rc >= 0) {
            pthread_join(server, nullptr);
        } else {
            std::printf("artemislm-ps5: llama_server did not return in 10 s; restarting anyway\n");
        }
        server_started = false;
        running.clear();
        panel_set_running("");
        log_memory("after unloading");
    }
};

// A thread with a 16 MiB stack, as llama-server's: the console's default is small.
// False leaves *out unset. The caller runs the work itself or reports the failure.
bool start_thread(std::function<void()> & fn, pthread_t * out) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        std::printf("artemislm-ps5: thread setup failed\n");
        return false;
    }
    if (pthread_attr_setstacksize(&attr, 16u << 20) != 0) {
        pthread_attr_destroy(&attr);
        std::printf("artemislm-ps5: thread stack failed\n");
        return false;
    }
    const int rc = pthread_create(out, &attr, [](void * p) -> void * {
        (*static_cast<std::function<void()> *>(p))();
        return nullptr;
    }, &fn);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::printf("artemislm-ps5: thread failed to start (%d)\n", rc);
        return false;
    }
    return true;
}

bool load_font(hui::gfx::Renderer & renderer, const char * name, hui::gfx::Font * font, hui::ui::FontRef * ref) {
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data)) {
        std::printf("artemislm-ps5: font %s failed\n", name);
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

}  // namespace

int main(int, char **) {
    // mkdir creates one level. /data is already there and /data/ArtemisLM is
    // not, so the models folder is made in two steps. One mkdir of the models
    // path leaves no folder to copy a .gguf into.
    mkdir("/data/ArtemisLM", 0777);
    const int models_rc = mkdir(kModels, 0777);
    const int models_err = errno;
    mkdir("/data/ArtemisLM/cache", 0777);
    // Same one-level mkdir as models. Coding tasks store projects here.
    const int projects_rc = mkdir(ps5lm::kProjects, 0777);
    const int projects_err = errno;
    // Same one-level mkdir. Notes live here and stay out of the model until a question matches.
    const int memory_rc = mkdir(ps5lm::kMemory, 0777);
    const int memory_err = errno;

    // stdout and stderr to a file the PC can fetch over FTP.
    if (std::freopen(kLog, "w", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen(kLog, "a", stderr) != nullptr) {
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    std::printf("artemislm-ps5: version " PS5LM_VERSION "\n");
    if (models_rc != 0 && models_err != EEXIST) {
        std::printf("artemislm-ps5: /data/ArtemisLM/models: %s\n", std::strerror(models_err));
    }
    if (projects_rc != 0 && projects_err != EEXIST) {
        std::printf("artemislm-ps5: /data/ArtemisLM/projects: %s\n", std::strerror(projects_err));
    }
    if (memory_rc != 0 && memory_err != EEXIST) {
        std::printf("artemislm-ps5: /data/ArtemisLM/memory: %s\n", std::strerror(memory_err));
    }
    // One device-local heap over the whole pool (RADV splits an APU's 2/3 to
    // 1/3), and a HOME for llama.cpp, which throws without one.
    setenv("radv_enable_unified_heap_on_apu", "true", 1);
    setenv("HOME", "/data/ArtemisLM", 0);
    setenv("LLAMA_CACHE", "/data/ArtemisLM/cache", 0);
    // Driver and backend switches to measure with, one NAME=value a line in
    // env.txt, set before the GPU opens; only the driver's, ggml's and llama's.
    for (const auto & line : read_lines(kEnv)) {
        const size_t eq = line.find('=');
        const std::string name = line.substr(0, eq == std::string::npos ? 0 : eq);
        bool known = false;
        for (const char * prefix : { "RADV_", "MESA_", "ACO_", "GGML_", "LLAMA_", "ARTEMISLM_", "PS5LM_" }) {
            known = known || name.rfind(prefix, 0) == 0;
        }
        if (known) {
            setenv(name.c_str(), line.c_str() + eq + 1, 1);
        }
        std::printf("artemislm-ps5: env %s%s\n", line.c_str(), known ? "" : " (ignored)");
    }
    std::printf("artemislm-ps5: start\n");

    // The screen first: it takes from the same memory as the model.
    Display display;
    if (!display.open(1920, 1080)) {
        std::printf("artemislm-ps5: no display\n");
    }
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, headline, mono;
    hui::ui::Fonts fonts;
    const bool ui_ok = renderer.init() && load_font(renderer, "manrope-500.huifont", &regular, &fonts.regular) &&
                       load_font(renderer, "manrope-600.huifont", &semibold, &fonts.semibold) &&
                       load_font(renderer, "manrope-200.huifont", &headline, &fonts.display) &&
                       load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono);
    fonts.pixel = fonts.mono;
    fonts.hand = fonts.regular;
    std::printf("artemislm-ps5: dashboard %s\n", ui_ok ? "ready" : "failed");

    Dashboard dash(fonts);
    DashboardFrame frame;
    const auto render = [&] {
        frame.backdrop = {};
        frame.scene.clear();
        frame.overlay.clear();
        frame.glass = false;
        frame.glass_texture = renderer.glass_texture();
        dash.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass) {
            renderer.glass();
        }
        renderer.draw(frame.overlay);
        display.present(renderer);
    };
    // The splash at once, so a restart for a model is never a black screen.
    if (ui_ok) {
        dash.set_busy("Starting", "Opening the GPU and reading the models");
        render();
        display.swap();
    }
    sceSystemServiceHideSplashScreen();

    StatsCollector stats;
    stats.start(kPort);
    set_activity_sink(&stats);  // llama-server's request hooks (patches/0007) report here
    Models models;
    models.settings = load_settings(kSettings);
    // The model about to load, by name, for Home while the GPU opens.
    {
        std::vector<std::string> next = read_lines(kArgs);
        const auto m = std::find(next.begin(), next.end(), "-m");
        next = m != next.end() && m + 1 != next.end() ? std::vector<std::string>{ *(m + 1) } : read_lines(kChoice);
        if (next.empty() && models.settings.auto_load) {
            next = { models.settings.default_model };
        }
        if (!next.empty() && !next[0].empty() && next[0] != "none") {
            Live pending;
            pending.model_label = base_name(next[0]);
            stats.set_model(pending);
            stats.set_state(ServerState::loading);
        }
        stats.set_phase("Opening the GPU and reading the models");
    }

    // The slow part on a thread of its own (the GPU opening, every model's
    // header, the model's start) while the screen keeps moving: the splash
    // for a moment, then Home, where the load shows.
    bool idle_page = false;
    std::function<void()> boot = [&] {
        for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            size_t free = 0, total = 0;
            ggml_backend_dev_memory(dev, &free, &total);
            std::printf("artemislm-ps5: device %zu %s (%s): %.2f / %.2f GiB free\n", i, ggml_backend_dev_name(dev),
                        ggml_backend_dev_description(dev), free / 1073741824.0, total / 1073741824.0);
            // The PS5 is an APU: ggml calls its GPU integrated.
            const auto type = ggml_backend_dev_type(dev);
            if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
                models.budget_gib = free / 1073741824.0;
            }
        }

        models.scan(stats);

        // Which model: app-args.txt verbatim, else model.txt, which the library
        // writes just before restarting the app for a switch and which is used
        // once, else the default model when Settings asks for it at launch. A
        // plain launch otherwise loads nothing and opens the library.
        Live model_part;
        std::vector<std::string> args = read_lines(kArgs);
        std::string path;
        if (!args.empty()) {
            const auto m = std::find(args.begin(), args.end(), "-m");
            path = m != args.end() && m + 1 != args.end() ? *(m + 1) : "";
            model_part.model_label = base_name(path);
            const ModelInfo info = read_model_info(path);
            model_part.model_arch = info.arch;
            model_part.model_gib = info.file_bytes / 1073741824.0;
            model_part.model_file = path;
            model_part.args = args;
            std::printf("artemislm-ps5: app-args.txt overrides the planner\n");
        } else {
            auto chosen = read_lines(kChoice);
            unlink(kChoice);
            if (chosen.empty() && models.settings.auto_load && !models.settings.default_model.empty()) {
                chosen = { models.settings.default_model };  // "none" after an unload keeps it from coming back
            }
            for (const auto & r : models.rows) {
                if (r.fits && !chosen.empty() && r.file == chosen[0]) {
                    path = r.file;
                }
            }
            if (!path.empty()) {
                args = models.args_for(path, model_part);
            }
        }
        // A projector this llama.cpp cannot read stops the server (#15); the
        // restart after it loads the same model text only, this once.
        if (access(kTextOnly, F_OK) == 0) {
            unlink(kTextOnly);
            const auto mm = std::find(args.begin(), args.end(), "--mmproj");
            if (mm != args.end()) {
                std::printf("artemislm-ps5: loading without %s\n", mm + 1 != args.end() ? (mm + 1)->c_str() : "the projector");
                args.erase(mm, std::min(mm + 2, args.end()));
                model_part.args = args;
            }
        }
        log_memory("before loading");
        if (args.empty() || !models.start(path, args, stats, model_part)) {
            if (!args.empty()) {
                std::printf("artemislm-ps5: model thread did not start; opening the page\n");
            }
            stats.set_state(ServerState::no_model);
            idle_page = true;
        }
        stats.set_phase("");
    };
    std::atomic<bool> booted{ false };
    std::function<void()> boot_then = [&] {
        boot();
        booted = true;
    };
    pthread_t booting{};
    const bool boot_thread = start_thread(boot_then, &booting);
    if (!boot_thread) {
        // The splash waits on this scan. With no thread, do it here so the
        // wait cannot sit up forever and then join a thread that was never created.
        std::printf("artemislm-ps5: boot thread failed to start; scanning on the main thread\n");
        boot_then();
    }
    {
        hui::ui::Feedback quiet;  // no sounds yet: the mixer starts after
        const int64_t start = hui::sys::monotonic_us();
        int64_t at = start;
        while (ui_ok && !booted) {
            const int64_t now = hui::sys::monotonic_us();
            if (now - start > 1200000) {
                dash.set_busy("");
            }
            dash.set_live(stats.snapshot());
            quiet.clear();
            dash.update(hui::InputFrame{}, std::min(0.05f, (float) (now - at) / 1e6f), quiet);
            at = now;
            render();
            display.swap();
        }
    }
    if (boot_thread) {
        pthread_join(booting, nullptr);
    }

    // Input and sound, as ps5-homebrew-ui's own app does.
    hui::ps5::Pad pad;
    pad.open();
    hui::InputTracker tracker;
    hui::audio::Mixer mixer;
    hui::ps5::AudioOut audio_out;
    audio_out.start(mixer);
    hui::audio::SoundBank sounds;
    sounds.load(std::string(kAssets) + "/audio/sfx");

    dash.set_busy("");
    Market    market;
    market.start(models.budget_gib);
    panel_set_budget(models.budget_gib);
    panel_start(&market, models.budget_gib);
    if (idle_page) {
        page_start([&stats](const std::string & note) { stats.set_phase(note); });
        notify("ArtemisLM PS5: nothing is loaded. The page can download a model or add a folder.");
    }
    update_start(false);
    int panel_seen = panel_generation();
    std::atomic<int> file_job{ 0 };  // 1 while a move or delete runs, 2 when its note is ready
    std::mutex file_mu;
    std::string file_note;
    int  saved_seen = 0;  // downloads finished, as last seen
    dash.set_settings(models.settings);
    bool      scratch_warned = false;
    // Downloads left queued or running carry on, on their own page.
    unlink("/data/ArtemisLM/download.txt");  // the queue file replaced this one (v0.7.0)
    market.load_queue();
    for (const auto & d : market.snapshot().downloads) {
        // Running already, maybe: the queue's thread starts it at once.
        if (d.state == DownloadState::queued || d.state == DownloadState::running) {
            dash.show_downloads();
            break;
        }
    }
    hui::ui::Feedback feedback;
    hui::PadSample samples[64];
    int64_t last = hui::sys::monotonic_us();
    bool was_serving = false;
    bool rescanning = false;
    bool update_shown = false;
    uint64_t restart_at = 0;  // the frame to stop the model and restart on, or 0
    for (uint64_t n = 0;; ++n) {
        const int64_t now = hui::sys::monotonic_us();
        const float dt = n == 0 ? 1.0f / 60.0f : std::min(0.05f, (float) (now - last) / 1e6f);
        last = now;

        const size_t count = pad.read(samples);
        const hui::InputFrame input = tracker.update(std::span<const hui::PadSample>(samples, count), (uint64_t) now);
        Live live = stats.snapshot();
        if (g_server_rc >= 0 && live.state != ServerState::failed && live.state != ServerState::no_model) {
            stats.set_state(ServerState::failed);
            const bool projector = std::find(g_server_args.begin(), g_server_args.end(), "--mmproj") != g_server_args.end();
            if (live.state == ServerState::loading && projector && restart_at == 0) {
                std::ofstream(kChoice) << models.running << "\n";
                std::ofstream(kTextOnly) << models.running << "\n";
                notify("ArtemisLM PS5: the image projector did not load; loading the model text only");
                dash.set_busy("Loading " + base_name(models.running) + " text only",
                              "Its image projector did not load, see /data/ArtemisLM/llama.log");
                restart_at = n + 3;
            } else {
                notify("ArtemisLM PS5: the server stopped, see /data/ArtemisLM/llama.log");
            }
        }
        const bool serving = live.state == ServerState::ready || live.state == ServerState::generating;
        if (serving && !was_serving) {
            notify(live.decision.empty() ? "ArtemisLM PS5: ready, chat in a browser on this network" : "ArtemisLM PS5: ready for decisions at /v1/systemone");
        }
        was_serving = serving;
        if (n % 30 == 0) {
            models.publish(stats);
        }
        dash.set_live(live);
        feedback.clear();
        // A button pressed from the PC (scripts/artemislm-ps5.sh press r1), for
        // testing the screens without a controller in hand.
        hui::InputFrame in = input;
        if (n % 20 == 10) {
            const std::string key = take_request_file("/data/ArtemisLM/press");
            const struct {
                const char *   name;
                hui::Action    action;
                hui::Direction nav;
            } keys[] = {
                { "up", hui::Action::up, hui::Direction::up },
                { "down", hui::Action::down, hui::Direction::down },
                { "left", hui::Action::left, hui::Direction::left },
                { "right", hui::Action::right, hui::Direction::right },
                { "cross", hui::Action::confirm, hui::Direction::none },
                { "circle", hui::Action::back, hui::Direction::none },
                { "triangle", hui::Action::north, hui::Direction::none },
                { "square", hui::Action::west, hui::Direction::none },
                { "l1", hui::Action::page_prev, hui::Direction::none },
                { "r1", hui::Action::page_next, hui::Direction::none },
                { "l2", hui::Action::jump_prev, hui::Direction::none },
                { "r2", hui::Action::jump_next, hui::Direction::none },
            };
            for (const auto & k : keys) {
                if (key == k.name) {
                    in.connected = true;
                    in.pressed |= hui::action_bit(k.action);
                    in.nav = k.nav;
                }
            }
        }
        dash.update(in, dt, feedback);
        for (const auto & cue : feedback.cues) {
            if (!models.settings.sounds) {
                break;
            }
            sounds.play(mixer, cue.set == hui::audio::SoundSet::count ? hui::audio::SoundSet::glass : cue.set, cue);
        }
        if (feedback.rumble_strength > 0.0f) {
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        }
        pad.tick(dt);

        // The scratch folder: its size for the dashboard, and one warning each
        // time it grows past the limit.
        if (n % 600 == 300) {
            const double gib = folder_bytes(kScratch) / 1073741824.0;
            stats.set_scratch(gib);
            const bool over = gib > models.settings.scratch_limit_gib;
            if (over && !scratch_warned) {
                char text[128];
                std::snprintf(text, sizeof(text), "ArtemisLM PS5: the chat's scratch folder holds %.1f GiB; clear it in Settings", gib);
                notify(text);
                std::printf("artemislm-ps5: %s\n", text + 15);
            }
            scratch_warned = over;
        }
        if (n % 10 == 0) {
            MarketView mv = market.snapshot();
            // A finished download joins the library.
            if (mv.saved != saved_seen) {
                saved_seen = mv.saved;
                models.rescan(stats, false);
                notify(("ArtemisLM PS5: " + mv.note).c_str());
            }
            dash.set_market(std::move(mv));
        }
        if (dash.wants_logs() && n % 60 == 0) {
            dash.set_logs(tail_lines(kLog), tail_lines(kLlamaLog));
        }

        // A settings rescan in the background: say so until it lands.
        if (models.poll_rescan(stats) && models.scan_shown) {
            char text[96];
            std::snprintf(text, sizeof(text), "Planning every model again for %uk tokens, %s cache, %s batch",
                          models.settings.ctx_cap / 1024, models.settings.kv_type.c_str(), models.settings.batch.c_str());
            dash.set_busy("Applying the new limits", text);
            rescanning = true;
        } else if (rescanning) {
            rescanning = false;
            dash.set_busy("");
        }
        // A quiet GitHub check does not cover the screen. The busy page is
        // only there while a release is being downloaded and installed.
        std::string update_note;
        if (update_take_notice(&update_note)) {
            notify(update_note.c_str());
            std::printf("artemislm-ps5: %s\n", update_note.c_str());
        }
        if (update_take_restart() && restart_at == 0) {
            dash.set_busy("Updating", update_status());
            restart_at = n + 3;
            update_shown = false;
        } else if (update_installing()) {
            dash.set_busy("Updating", update_status());
            update_shown = true;
        } else if (update_shown) {
            update_shown = false;
            dash.set_busy("");
        }
        if (file_job.load() == 2) {
            std::string note;
            {
                std::lock_guard<std::mutex> lock(file_mu);
                note = file_note;
            }
            file_job = 0;
            dash.set_busy("");
            notify(("ArtemisLM PS5: " + note).c_str());
            std::printf("artemislm-ps5: %s\n", note.c_str());
            models.rescan(stats, false);
        }
        if (panel_generation() != panel_seen) {
            panel_seen = panel_generation();
            models.settings = load_settings(kSettings);
            dash.set_settings(models.settings);
            models.rescan(stats, false);
        }
        // A restart asked for a few frames ago, once the busy screen is up.
        if (restart_at != 0 && n >= restart_at) {
            std::fflush(nullptr);
            // Drop both listeners first. The new process starts while this one
            // is still exiting, and a single refused bind used to stop the load.
            page_stop();
            panel_stop();
            models.stop();
            sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
        }

        Dashboard::Request req = dash.take_request();
        if (req.kind == Dashboard::Request::settings) {
            // The planner's limits change every row's plan; the loaded model
            // keeps its settings until it is loaded again.
            // Context, cache, flash attention, batch and slots change whether
            // a file fits. Rescanning reads every model's header, seconds
            // with a USB drive.
            const Settings was = models.settings;
            models.settings = dash.settings();
            save_settings(kSettings, models.settings);
            // Sampling, tools and the system text change the next load's
            // arguments and not whether a file fits, so they do not re-plan.
            if (was.ctx_cap != models.settings.ctx_cap || was.kv_type != models.settings.kv_type ||
                was.flash_attn != models.settings.flash_attn || was.batch != models.settings.batch ||
                was.slots != models.settings.slots || was.memory != models.settings.memory ||
                was.scan_ps5lm != models.settings.scan_ps5lm || was.scan_usb != models.settings.scan_usb ||
                was.extra_dir != models.settings.extra_dir || was.migrate != models.settings.migrate) {
                models.rescan(stats);
            }
            std::printf("artemislm-ps5: settings saved\n");
        }
        switch (req.kind) {
            case Dashboard::Request::ask:
                ask_self(kPort, req.path, !live.decision.empty(), models.settings.datetime, models.settings.web_search,
                         (int) models.settings.search_hits, models.settings.agent, (int) models.settings.plan_tokens);
                break;
            case Dashboard::Request::market_search: market.search(req.path); break;
            case Dashboard::Request::market_open: market.open_repo(req.repo); break;
            case Dashboard::Request::market_plan: market.plan_file(req.repo, req.file); break;
            case Dashboard::Request::market_download: {
                MarketJob job;
                if (market.job(req.repo, req.file, req.path.empty() ? models.settings.download_dir : req.path, &job)) {
                    market.enqueue(job, free_gib(job.dir));
                }
                break;
            }
            case Dashboard::Request::suggested_download: {
                const Suggestion & s = kSuggestions[std::clamp(req.repo, 0, kSuggestionCount - 1)];
                const std::string dir = req.path.empty() ? models.settings.download_dir : req.path;
                for (const SuggestedFile * f : { &s.model, &s.mmproj }) {
                    if (f->file) {
                        market.enqueue({ s.repo, f->file, f->sha256, dir, f->bytes, f->save_as ? f->save_as : "" }, free_gib(dir));
                    }
                }
                dash.show_downloads();
                break;
            }
            case Dashboard::Request::download_pause: market.pause(req.repo); break;
            case Dashboard::Request::download_resume: market.resume(req.repo); break;
            case Dashboard::Request::download_remove: market.remove(req.repo); break;
            case Dashboard::Request::app_update: update_start(true); break;
            case Dashboard::Request::move_model:
            case Dashboard::Request::remove_model: {
                int idle = 0;
                if (file_job.compare_exchange_strong(idle, 1)) {
                    const bool move = req.kind == Dashboard::Request::move_model;
                    const Settings snap = models.settings;
                    const std::string path = req.path;
                    const std::string running = models.running;
                    dash.set_busy(move ? "Moving into ArtemisLM" : "Deleting the model",
                                  move ? "The old file is removed after the copy finishes" : "");
                    std::thread([&, snap, path, running, move] {
                        const std::string note = move ? library_move(snap, path, running) : library_remove(snap, path, running);
                        {
                            std::lock_guard<std::mutex> lock(file_mu);
                            file_note = note;
                        }
                        file_job = 2;
                    }).detach();
                }
                break;
            }
            case Dashboard::Request::market_download_unload: {
                // Unload by restarting (as the library does); the queue,
                // saved to disk, starts the download in the new process.
                MarketJob job;
                if (market.job(req.repo, req.file, req.path.empty() ? models.settings.download_dir : req.path, &job) &&
                    market.enqueue(job, free_gib(job.dir))) {
                    std::ofstream(kChoice) << "none\n";
                    std::printf("artemislm-ps5: restarting to download %s/%s without a model\n", job.repo.c_str(), job.file.c_str());
                    dash.set_busy("Unloading " + base_name(models.running), "The app restarts without it, then the download starts");
                    restart_at = n + 3;
                }
                break;
            }
            default: break;
        }
        if (req.kind == Dashboard::Request::clear_scratch) {
            std::error_code ec;
            for (const auto & e : std::filesystem::directory_iterator(kScratch, ec)) {
                std::filesystem::remove_all(e.path(), ec);
            }
            stats.set_scratch(folder_bytes(kScratch) / 1073741824.0);
            notify("ArtemisLM PS5: scratch folder cleared");
            std::printf("artemislm-ps5: scratch folder cleared\n");
        }
        // The same requests from the PC (scripts/artemislm-ps5.sh load|unload).
        if (n % 60 == 30 && req.kind == Dashboard::Request::none) {
            if (std::string p = take_request_file("/data/ArtemisLM/load"); !p.empty()) {
                req = { Dashboard::Request::load, p };
            } else if (!take_request_file("/data/ArtemisLM/unload").empty()) {
                req = { Dashboard::Request::unload, "" };
            } else if (std::string u = take_request_file("/data/ArtemisLM/fetch"); !u.empty()) {
                std::thread(probe_fetch, u).detach();
            } else if (!take_request_file("/data/ArtemisLM/bench-write").empty()) {
                std::thread(probe_write).detach();
            } else if (std::string q = take_request_file("/data/ArtemisLM/search"); !q.empty()) {
                market.search(q == "-" ? std::string() : q);  // a query typed on the PC
            }
        }
        if (req.kind == Dashboard::Request::unload || req.kind == Dashboard::Request::load) {
            // Unloading in place leaves part of the model's GPU memory held
            // (about 5 GiB of a 6.8 GiB model, measured), so a model change
            // restarts the app: the system replaces the process (LoadExec on
            // our own eboot.bin, "Kill for LoadExec" in the log) and the new
            // one starts from the whole pool, with model.txt saying what to load.
            std::ofstream(kChoice) << (req.kind == Dashboard::Request::load ? req.path : std::string("none")) << "\n";
            if (req.kind == Dashboard::Request::load) {
                unlink(kArgs);  // the library's choice replaces a manual override
            }
            std::printf("artemislm-ps5: restarting for %s\n", req.kind == Dashboard::Request::load ? req.path.c_str() : "no model");
            const bool loaded = !models.running.empty();
            if (req.kind == Dashboard::Request::load) {
                dash.set_busy(loaded ? "Switching models" : "Loading " + base_name(req.path),
                              loaded ? "Unloading " + base_name(models.running) + ", then the app restarts with " + base_name(req.path)
                                     : "The app restarts to give the model the whole memory");
            } else {
                dash.set_busy("Unloading " + base_name(models.running), "The app restarts without a model, about 30 seconds");
            }
            restart_at = n + 3;  // three frames with the busy screen on the TV first
        }

        if (ui_ok) {
            render();
            if (n % 60 == 45 && !take_request_file("/data/ArtemisLM/screenshot").empty()) {
                const bool ok = save_screen("/data/ArtemisLM/screen.bmp", display.width(), display.height());
                std::printf("artemislm-ps5: screenshot %s\n", ok ? "saved" : "failed");
            }
        }
        display.swap();
        if (n % 60 == 0) {
            quit_if_asked();
        }
    }
}
