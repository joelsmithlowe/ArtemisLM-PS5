// Check joelsmithlowe/ArtemisLM-PS5 on GitHub and install a newer title zip.
// The check runs at launch. Installing waits for Cross or the phone page.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update.hpp"

#include "fetch.hpp"
#include "stats.hpp"
#include "update_plan.hpp"
#include "version.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ps5lm {

namespace {

std::mutex mu;
std::string status_text = "Checking GitHub";
std::string notice;
std::string latest;
ReleaseInfo cached;
bool have_cached = false;
bool ready = false;
std::atomic<bool> busy{ false };
std::atomic<bool> installing{ false };
std::atomic<bool> want_install{ false };
std::atomic<bool> restart{ false };
int tmpl = -1;

void set_status(const std::string & text) {
    std::string shown = text.size() > 120 ? text.substr(0, 120) : text;
    {
        std::lock_guard<std::mutex> lock(mu);
        status_text = shown;
    }
    std::printf("artemislm-ps5: update: %s\n", shown.c_str());
}

void remember(const ReleaseInfo & info, bool newer) {
    std::lock_guard<std::mutex> lock(mu);
    cached = info;
    have_cached = true;
    latest = info.version;
    ready = newer;
}

bool fetch_latest(ReleaseInfo * info, std::string * error) {
    if (tmpl < 0) {
        tmpl = https_template("ArtemisLM-PS5-update");
    }
    if (tmpl < 0) {
        if (error) {
            *error = "Could not reach GitHub";
        }
        return false;
    }
    const std::string url = std::string("https://api.github.com/repos/") + kUpdateRepo + "/releases/latest";
    std::string body;
    bool too_big = false;
    const FetchResult result = https_get(
        url, "",
        [&](const char * data, size_t n) {
            if (body.size() + n > (2u << 20)) {
                too_big = true;
                return false;
            }
            body.append(data, n);
            return true;
        },
        tmpl, 64u << 10);
    if (too_big || result.status != 200 || body.empty()) {
        std::printf("artemislm-ps5: update: github status %d (%s)\n", result.status, result.error.c_str());
        if (error) {
            *error = "Could not reach GitHub";
        }
        return false;
    }
    return parse_release(body, info, error);
}

void run_check() {
    set_status("Checking GitHub");
    ReleaseInfo info;
    std::string error;
    if (!fetch_latest(&info, &error)) {
        set_status(error.empty() ? "Could not reach GitHub" : error);
        std::lock_guard<std::mutex> lock(mu);
        ready = false;
        return;
    }
    const bool newer = version_cmp(info.version, PS5LM_VERSION) > 0;
    remember(info, newer);
    if (!newer) {
        set_status(std::string("This is ") + PS5LM_VERSION + ", the latest release");
        return;
    }
    set_status(info.version + " is ready. Cross installs it");
    std::lock_guard<std::mutex> lock(mu);
    notice = std::string("ArtemisLM PS5 ") + info.version + " is on GitHub. Settings, Sound and storage, App update.";
}

bool take_cached(ReleaseInfo * info) {
    std::lock_guard<std::mutex> lock(mu);
    if (!have_cached) {
        return false;
    }
    *info = cached;
    return true;
}

void remove_work() {
    const std::string dir = kUpdateDir;
    std::error_code ec;
    std::filesystem::remove_all(dir + "/stage", ec);
    unlink((dir + "/artemislm-ps5-app.zip").c_str());
    unlink((dir + "/artemislm-ps5-app.zip.part").c_str());
}

bool download_zip(const ReleaseInfo & info, const std::string & zip, std::string * error) {
    const std::string part = std::string(kUpdateDir) + "/artemislm-ps5-app.zip.part";
    unlink(part.c_str());
    FILE * out = std::fopen(part.c_str(), "wb");
    if (!out) {
        if (error) {
            *error = "The download did not finish";
        }
        return false;
    }
    uint64_t got = 0;
    uint64_t shown = 0;
    bool too_big = false;
    bool write_failed = false;
    if (tmpl < 0) {
        tmpl = https_template("ArtemisLM-PS5-update");
    }
    const FetchResult result = https_get(
        info.url, "",
        [&](const char * data, size_t n) {
            if (got + n > (80ull << 20)) {
                too_big = true;
                return false;
            }
            if (std::fwrite(data, 1, n, out) != n) {
                write_failed = true;
                return false;
            }
            got += n;
            if (got - shown >= (4ull << 20)) {
                shown = got;
                set_status("Downloading " + info.version + ", " + std::to_string(got / (1ull << 20)) + " MB");
            }
            return true;
        },
        tmpl, 1u << 20);
    const bool closed = std::fclose(out) == 0;
    if (tmpl < 0 || too_big || write_failed || !closed || result.status != 200 || got == 0) {
        unlink(part.c_str());
        std::printf("artemislm-ps5: update: download status %d bytes %llu (%s)\n", result.status,
                    (unsigned long long) got, result.error.c_str());
        if (error) {
            *error = too_big ? "The zip is too large" : "The download did not finish";
        }
        return false;
    }
    unlink(zip.c_str());
    if (rename(part.c_str(), zip.c_str()) != 0) {
        unlink(part.c_str());
        if (error) {
            *error = "The download did not finish";
        }
        return false;
    }
    return true;
}

// True when the new title is in place and the app should restart.
bool run_install() {
    ReleaseInfo info;
    std::string error;
    if (!take_cached(&info) || version_cmp(info.version, PS5LM_VERSION) <= 0) {
        ReleaseInfo fresh;
        if (!fetch_latest(&fresh, &error)) {
            if (!take_cached(&info)) {
                set_status(error.empty() ? "Could not reach GitHub" : error);
                return false;
            }
        } else {
            info = fresh;
            remember(info, version_cmp(info.version, PS5LM_VERSION) > 0);
        }
    }
    if (version_cmp(info.version, PS5LM_VERSION) <= 0) {
        set_status(std::string("This is ") + PS5LM_VERSION + ", the latest release");
        std::lock_guard<std::mutex> lock(mu);
        ready = false;
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory("/data/homebrew", ec)) {
        set_status("Copy ArtemisLM PS5 to /data/homebrew once, then updates can install");
        return false;
    }
    const double free = drive_free_gib("/data/ArtemisLM/models");
    if (free >= 0.0 && free < 0.5) {
        set_status("Less than 0.5 GiB free. Free some space, then try again");
        return false;
    }
    mkdir("/data/ArtemisLM", 0777);
    if (mkdir(kUpdateDir, 0777) != 0 && errno != EEXIST) {
        set_status("The download did not finish");
        return false;
    }
    set_status("Downloading " + info.version);
    const std::string zip = std::string(kUpdateDir) + "/artemislm-ps5-app.zip";
    const std::string stage = std::string(kUpdateDir) + "/stage";
    if (!download_zip(info, zip, &error)) {
        set_status(error.empty() ? "The download did not finish" : error);
        remove_work();
        return false;
    }
    set_status("Checking the checksum");
    const std::string sum = sha256_file(zip);
    if (sum.empty() || sum != info.sha256) {
        std::printf("artemislm-ps5: update: checksum mismatch\n");
        set_status("The checksum did not match");
        remove_work();
        return false;
    }
    set_status("Unpacking " + info.version);
    if (!extract_title_zip(zip, stage, &error) || !stage_ok(stage, &error)) {
        set_status(error.empty() ? "The zip could not be unpacked" : error);
        remove_work();
        return false;
    }
    const auto eboot_bytes = std::filesystem::file_size(stage + "/eboot.bin", ec);
    if (ec || eboot_bytes < (8ull << 20)) {
        set_status("The new app is too small");
        remove_work();
        return false;
    }
    set_status("Installing " + info.version);
    if (!install_title(stage, kInstallDir, &error)) {
        set_status(error.empty() ? "The install did not finish" : error);
        remove_work();
        return false;
    }
    remove_work();
    {
        std::lock_guard<std::mutex> lock(mu);
        ready = false;
        latest = info.version;
    }
    restart.store(true);
    set_status("Installed " + info.version + ". Restarting");
    return true;
}

void thread_main() {
    // A download can be making its HTTP templates on another thread. Several
    // of those at once has crashed the app, so the first check waits its turn.
    static std::atomic<bool> waited{ false };
    if (!waited.exchange(true)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    try {
        for (;;) {
            const bool do_install = want_install.exchange(false);
            if (!do_install) {
                run_check();
                if (!want_install.load()) {
                    break;
                }
                continue;
            }
            installing.store(true);
            const bool installed = run_install();
            installing.store(false);
            if (installed) {
                want_install.store(false);
                break;
            }
            if (!want_install.load()) {
                break;
            }
        }
    } catch (const std::exception & ex) {
        std::printf("artemislm-ps5: update: %s\n", ex.what());
        set_status("The update stopped");
        installing.store(false);
    } catch (...) {
        set_status("The update stopped");
        installing.store(false);
    }
    busy.store(false);
    if (want_install.load()) {
        update_start(true);
    }
}

}  // namespace

void update_start(bool install) {
    if (install) {
        want_install.store(true);
    }
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread(thread_main).detach();
}

std::string update_status() {
    std::lock_guard<std::mutex> lock(mu);
    return status_text;
}

bool update_installing() { return installing.load(); }

bool update_take_notice(std::string * text) {
    if (!text) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (notice.empty()) {
        return false;
    }
    *text = notice;
    notice.clear();
    return true;
}

bool update_take_restart() { return restart.exchange(false); }

std::string update_json() {
    std::string shown;
    std::string lat;
    bool is_ready = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        shown = status_text;
        lat = latest;
        is_ready = ready;
    }
    return nlohmann::json{
        { "version", PS5LM_VERSION },
        { "status", shown },
        { "latest", lat },
        { "ready", is_ready },
        { "installing", installing.load() },
    }.dump();
}

}  // namespace ps5lm
