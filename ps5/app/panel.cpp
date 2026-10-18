// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "ports.hpp"
#include "version.hpp"

#include "fetch.hpp"
#include "library.hpp"
#include "market.hpp"
#include "model_plan.hpp"
#include "search_parse.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "update.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace ps5lm {

namespace {

constexpr int kPanelPort = kLibraryPort;
constexpr const char * kSettingsPath = "/data/ArtemisLM/settings.json";
constexpr const char * kHub = "https://huggingface.co";

std::mutex g_mu;
Market * g_market = nullptr;
double g_budget = 0;
std::string g_running;
std::atomic<int> g_gen{ 0 };

void bump() { g_gen.fetch_add(1); }

std::string http_text(const std::string & url, std::string * error) {
    std::string body;
    const FetchResult r = https_get(url, "", [&](const char * p, size_t n) {
        if (body.size() >= (256u << 10)) {
            return false;
        }
        body.append(p, n);
        return body.size() < (256u << 10);
    }, -1, 64u << 10);
    if (r.status != 200) {
        *error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        return {};
    }
    return body;
}

bool repo_ok(const std::string & id) {
    const size_t slash = id.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= id.size() || id.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    if (id.find("..") != std::string::npos || id.find('@') != std::string::npos) {
        return false;
    }
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '/' && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

bool file_ok(const std::string & name) {
    if (name.size() < 6 || name.compare(name.size() - 5, 5, ".gguf") != 0) {
        return false;
    }
    return name.find("..") == std::string::npos && name.find('\\') == std::string::npos && name[0] != '/';
}

std::string query_value(const std::string & path, const char * key) {
    const std::string needle = std::string(key) + "=";
    const size_t q = path.find('?');
    if (q == std::string::npos) {
        return {};
    }
    size_t at = q + 1;
    while (at < path.size()) {
        const size_t amp = path.find('&', at);
        const std::string part = path.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (part.rfind(needle, 0) == 0) {
            return url_decode(part.substr(needle.size()));
        }
        if (amp == std::string::npos) {
            break;
        }
        at = amp + 1;
    }
    return {};
}

void send_all(int fd, const std::string & bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n <= 0) {
            return;
        }
        sent += (size_t) n;
    }
}

void reply(int fd, int code, const std::string & body) {
    const char * text = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : "Error";
    const std::string head = "HTTP/1.0 " + std::to_string(code) + " " + text +
                             "\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                             "Access-Control-Allow-Headers: Content-Type\r\n"
                             "Content-Type: application/json\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send_all(fd, head);
}

std::string library_json() {
    Settings s;
    double budget = 0;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = load_settings(kSettingsPath);
        budget = g_budget;
        running = g_running;
    }
    nlohmann::json models = nlohmann::json::array();
    const char * kv = s.kv_type == "auto" ? nullptr : s.kv_type.c_str();
    RunOptions run;
    run.flash_attn = s.flash_attn;
    run.batch = s.batch;
    run.slots = s.slots;
    run.memory = s.memory;
    for (const auto & path : find_models(library_dirs(s))) {
        if (models.size() >= 200) {
            break;
        }
        const ModelInfo m = read_model_info(path);
        const Plan p = m.ok && budget > 0 ? plan_model(m, budget, s.ctx_cap, kv, run) : Plan{};
        const Preset * pre = m.ok ? find_preset(m) : nullptr;
        const std::string label = path.substr(path.find_last_of('/') + 1);
        models.push_back({
            { "file", path },
            { "label", label.size() > 5 ? label.substr(0, label.size() - 5) : label },
            { "arch", m.arch },
            { "preset", pre ? pre->label : "" },
            { "decision", m.decision },
            { "vision", !m.mmproj.empty() },
            { "kind", !m.decision.empty() ? "Decision" : !m.mmproj.empty() ? "Vision" : "Language" },
            { "plan", m.ok ? (budget > 0 ? p.why : "Waiting for the GPU") : m.error },
            { "fits", m.ok && p.fits },
            { "size_gib", m.file_bytes / 1073741824.0 },
            { "ctx", m.n_ctx_train },
            { "folder", model_folder(path) },
            { "home", model_in_home(path) },
            { "current", path == running },
        });
    }
    nlohmann::json out = { { "home", kHomeModels }, { "budget_gib", budget }, { "models", std::move(models) } };
    return out.dump();
}

// OpenAI's model list, plus an artemislm object a strict client can ignore.
// context_length is the context this console would load, which can be shorter
// than the file's trained context.
std::string catalog_json() {
    Settings s;
    double budget = 0;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = load_settings(kSettingsPath);
        budget = g_budget;
        running = g_running;
    }
    const char * kv = s.kv_type == "auto" ? nullptr : s.kv_type.c_str();
    RunOptions run;
    run.flash_attn = s.flash_attn;
    run.batch = s.batch;
    run.slots = s.slots;
    run.memory = s.memory;
    nlohmann::json data = nlohmann::json::array();
    for (const auto & path : find_models(library_dirs(s))) {
        if (data.size() >= 200) {
            break;
        }
        const ModelInfo m = read_model_info(path);
        const Plan p = m.ok && budget > 0 ? plan_model(m, budget, s.ctx_cap, kv, run) : Plan{};
        const std::string label = path.substr(path.find_last_of('/') + 1);
        const std::string id = label.size() > 5 ? label.substr(0, label.size() - 5) : label;
        data.push_back({
            { "id", id },
            { "object", "model" },
            { "owned_by", "artemislm" },
            { "context_length", p.ctx ? p.ctx : m.n_ctx_train },
            { "artemislm", {
                { "path", path },
                { "kind", !m.decision.empty() ? "Decision" : !m.mmproj.empty() ? "Vision" : "Language" },
                { "vision", !m.mmproj.empty() },
                { "fits", m.ok && p.fits },
                { "loaded", path == running },
                { "plan", m.ok ? (budget > 0 ? p.why : "Waiting for the GPU") : m.error },
                { "trained_ctx", m.n_ctx_train },
            } },
        });
    }
    return nlohmann::json{ { "object", "list" }, { "data", std::move(data) } }.dump();
}

std::string artemis_json() {
    double budget = 0;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        budget = g_budget;
        running = g_running;
    }
    const std::string ip = local_address();
    nlohmann::json out = {
        { "name", "ArtemisLM PS5" },
        { "version", PS5LM_VERSION },
        { "chat_port", kChatPort },
        { "library_port", kLibraryPort },
        { "budget_gib", budget },
        { "loaded", running.empty() ? nlohmann::json(nullptr) : nlohmann::json(running) },
    };
    if (!ip.empty()) {
        out["chat"] = std::string("http://") + ip + ":" + std::to_string(kChatPort) + "/v1";
    }
    return out.dump();
}

bool load_path_ok(const std::string & path) {
    if (path.size() < 6 || path.size() > 512 || path[0] != '/') {
        return false;
    }
    if (path.find("..") != std::string::npos) {
        return false;
    }
    for (unsigned char c : path) {
        if (c <= 32 || c == 127) {
            return false;
        }
    }
    return true;
}

std::string act_load(const std::string & body) {
    const nlohmann::json j = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    const std::string path = j.is_object() ? j.value("path", std::string()) : std::string();
    if (!load_path_ok(path)) {
        return "{\"error\":\"That model is not in the library\"}";
    }
    Settings s;
    double budget = 0;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = load_settings(kSettingsPath);
        budget = g_budget;
        running = g_running;
    }
    bool known = false;
    for (const auto & have : find_models(library_dirs(s))) {
        if (have == path) {
            known = true;
            break;
        }
    }
    if (!known) {
        return "{\"error\":\"That model is not in the library\"}";
    }
    if (path == running) {
        return "{\"error\":\"That model is already loaded\"}";
    }
    if (budget <= 0) {
        return "{\"error\":\"Waiting for the GPU\"}";
    }
    const ModelInfo m = read_model_info(path);
    const char * kv = s.kv_type == "auto" ? nullptr : s.kv_type.c_str();
    RunOptions run;
    run.flash_attn = s.flash_attn;
    run.batch = s.batch;
    run.slots = s.slots;
    run.memory = s.memory;
    const Plan p = m.ok ? plan_model(m, budget, s.ctx_cap, kv, run) : Plan{};
    if (!m.ok || !p.fits) {
        return "{\"error\":\"That model does not fit\"}";
    }
    std::ofstream out("/data/ArtemisLM/load", std::ios::trunc);
    if (!out) {
        return "{\"error\":\"The console could not start the load\"}";
    }
    out << path << "\n";
    return "{\"ok\":true,\"note\":\"The console is restarting with this model\"}";
}

std::string act_unload() {
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        running = g_running;
    }
    if (running.empty()) {
        return "{\"error\":\"Nothing is loaded\"}";
    }
    std::ofstream out("/data/ArtemisLM/unload", std::ios::trunc);
    if (!out) {
        return "{\"error\":\"The console could not unload the model\"}";
    }
    out << "1\n";
    return "{\"ok\":true,\"note\":\"The console is restarting with no model\"}";
}

std::string search_repos(const std::string & query) {
    std::string error;
    const std::string body = http_text(std::string(kHub) + "/api/models?filter=gguf&sort=downloads&direction=-1&limit=20" +
                                            (query.empty() ? "" : "&search=" + url_encode(query)),
                                        &error);
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    nlohmann::json repos = nlohmann::json::array();
    if (j.is_array()) {
        for (const auto & m : j) {
            const std::string id = m.value("id", std::string());
            if (repo_ok(id)) {
                repos.push_back({ { "id", id }, { "downloads", m.value("downloads", (uint64_t) 0) } });
            }
        }
    }
    return nlohmann::json{ { "repos", std::move(repos) }, { "error", error } }.dump();
}

std::string search_files(const std::string & repo) {
    if (!repo_ok(repo)) {
        return "{\"error\":\"That repository name is not usable\",\"files\":[]}";
    }
    std::string error;
    const std::string body = http_text(std::string(kHub) + "/api/models/" + repo + "/tree/main", &error);
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    nlohmann::json files = nlohmann::json::array();
    if (j.is_array()) {
        for (const auto & e : j) {
            const std::string path = e.value("path", std::string());
            if (!file_ok(path) || path.find("mmproj") != std::string::npos ||
                (path.find("-of-") != std::string::npos && path.find("-00001-of-") == std::string::npos)) {
                continue;
            }
            uint64_t bytes = e.value("size", (uint64_t) 0);
            if (e.contains("lfs") && e["lfs"].is_object()) {
                bytes = e["lfs"].value("size", bytes);
            }
            files.push_back({ { "file", path }, { "bytes", bytes } });
        }
    }
    return nlohmann::json{ { "repo", repo }, { "files", std::move(files) }, { "error", error } }.dump();
}

std::string save_from_page(const std::string & body) {
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) {
        return "{\"error\":\"Settings were not a JSON object\"}";
    }
    Settings s = load_settings(kSettingsPath);
    const auto take_bool = [&](const char * key, bool & dest) {
        if (j.contains(key) && j[key].is_boolean()) {
            dest = j[key].get<bool>();
        }
    };
    const auto take_text = [&](const char * key, std::string & dest) {
        if (j.contains(key) && j[key].is_string()) {
            dest = j[key].get<std::string>();
        }
    };
    // A JSON integer is usually stored signed, so is_number_unsigned() would
    // ignore 65536 and the context cap would never save.
    const auto take_u = [&](const char * key, uint32_t & dest) {
        if (!j.contains(key)) {
            return;
        }
        if (j[key].is_number_unsigned()) {
            const uint64_t v = j[key].get<uint64_t>();
            if (v <= 0xffffffffu) {
                dest = (uint32_t) v;
            }
        } else if (j[key].is_number_integer()) {
            const int64_t v = j[key].get<int64_t>();
            if (v >= 0) {
                dest = (uint32_t) v;
            }
        }
    };
    take_bool("auto_load", s.auto_load);
    take_bool("sounds", s.sounds);
    take_bool("tools", s.tools);
    take_bool("web_search", s.web_search);
    take_bool("datetime", s.datetime);
    take_bool("scan_ps5lm", s.scan_ps5lm);
    take_bool("scan_usb", s.scan_usb);
    take_text("default_model", s.default_model);
    take_text("kv_type", s.kv_type);
    take_text("flash_attn", s.flash_attn);
    take_text("batch", s.batch);
    take_text("sampling", s.sampling);
    take_text("reasoning", s.reasoning);
    take_text("system_prompt", s.system_prompt);
    take_text("download_dir", s.download_dir);
    if (j.contains("extra_dir") && j["extra_dir"].is_string()) {
        const std::string next = j["extra_dir"].get<std::string>();
        if (!extra_dir_ok(next)) {
            return "{\"error\":\"An extra folder is an absolute path under /data or /mnt, with no spaces\"}";
        }
        s.extra_dir = next;
    }
    take_text("migrate", s.migrate);
    take_text("memory", s.memory);
    take_u("ctx_cap", s.ctx_cap);
    take_u("slots", s.slots);
    take_u("scratch_limit_gib", s.scratch_limit_gib);
    if (j.contains("max_tokens") && j["max_tokens"].is_number_integer()) {
        s.max_tokens = j["max_tokens"].get<int32_t>();
    }
    if (!save_settings(kSettingsPath, s)) {
        return "{\"error\":\"Could not save settings\"}";
    }
    bump();
    return "{\"ok\":true}";
}

std::string download_file(const std::string & body) {
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) {
        return "{\"error\":\"Missing repository\"}";
    }
    const std::string repo = j.value("repo", std::string());
    const std::string file = j.value("file", std::string());
    const uint64_t bytes = j.value("bytes", (uint64_t) 0);
    if (!repo_ok(repo) || !file_ok(file)) {
        return "{\"error\":\"That file cannot be downloaded\"}";
    }
    Market * market = nullptr;
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        market = g_market;
        dir = load_settings(kSettingsPath).download_dir;
    }
    if (!market || dir.empty()) {
        return "{\"error\":\"Downloads are not ready\"}";
    }
    if (dir.rfind("/data/ArtemisLM", 0) != 0 && dir.rfind("/mnt/", 0) != 0) {
        dir = kHomeModels;
    }
    MarketJob job;
    job.repo = repo;
    job.file = file;
    job.dir = dir;
    job.bytes = bytes;
    if (!market->enqueue(job, drive_free_gib(dir))) {
        return nlohmann::json{ { "error", market->snapshot().note } }.dump();
    }
    return nlohmann::json{ { "ok", true }, { "note", market->snapshot().note }, { "dir", dir } }.dump();
}

std::string act_move(const std::string & body) {
    const nlohmann::json j = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    if (!j.is_object()) {
        return "{\"error\":\"Missing a path\"}";
    }
    const std::string path = j.value("path", std::string());
    Settings s;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = load_settings(kSettingsPath);
        running = g_running;
    }
    const std::string note = library_move(s, path, running);
    bump();
    const bool bad = note.rfind("Moved ", 0) != 0;
    return nlohmann::json{ { bad ? "error" : "note", note } }.dump();
}

std::string act_remove(const std::string & body) {
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    const std::string path = j.is_object() ? j.value("path", std::string()) : std::string();
    Settings s;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = load_settings(kSettingsPath);
        running = g_running;
    }
    const std::string note = library_remove(s, path, running);
    bump();
    const bool bad = note.rfind("Deleted ", 0) != 0;
    return nlohmann::json{ { bad ? "error" : "note", note } }.dump();
}

void handle(int fd, const std::string & method, const std::string & path, const std::string & body) {
    if (method == "OPTIONS") {
        reply(fd, 204, "");
        return;
    }
    const size_t q = path.find('?');
    const std::string route = path.substr(0, q);
    std::string out;
    int code = 200;
    if (method == "GET" && route == "/library") {
        out = library_json();
    } else if (method == "GET" && (route == "/v1/models" || route == "/models")) {
        out = catalog_json();
    } else if (method == "GET" && (route == "/artemis" || route == "/health")) {
        out = artemis_json();
    } else if (method == "GET" && route == "/settings") {
        const Settings s = load_settings(kSettingsPath);
        out = nlohmann::json{
            { "auto_load", s.auto_load }, { "default_model", s.default_model }, { "ctx_cap", s.ctx_cap },
            { "kv_type", s.kv_type }, { "flash_attn", s.flash_attn }, { "batch", s.batch }, { "slots", s.slots },
            { "sampling", s.sampling }, { "max_tokens", s.max_tokens }, { "reasoning", s.reasoning },
            { "system_prompt", s.system_prompt }, { "sounds", s.sounds }, { "tools", s.tools },
            { "web_search", s.web_search }, { "datetime", s.datetime }, { "scratch_limit_gib", s.scratch_limit_gib },
            { "download_dir", s.download_dir }, { "scan_ps5lm", s.scan_ps5lm }, { "scan_usb", s.scan_usb },
            { "extra_dir", s.extra_dir }, { "migrate", s.migrate }, { "memory", s.memory },
        }.dump();
    } else if (method == "GET" && route == "/search") {
        const std::string repo = query_value(path, "repo");
        out = repo.empty() ? search_repos(query_value(path, "q")) : search_files(repo);
    } else if (method == "POST" && route == "/settings") {
        out = save_from_page(body);
    } else if (method == "POST" && route == "/download") {
        out = download_file(body);
    } else if (method == "POST" && route == "/move") {
        out = act_move(body);
    } else if (method == "POST" && route == "/remove") {
        out = act_remove(body);
    } else if (method == "POST" && route == "/load") {
        out = act_load(body);
    } else if (method == "POST" && route == "/unload") {
        out = act_unload();
    } else if (method == "GET" && route == "/update") {
        out = update_json();
    } else if (method == "POST" && route == "/update") {
        const nlohmann::json j = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
        if (j.is_object() && j.contains("install") && j["install"].is_boolean() && j["install"].get<bool>()) {
            update_start(true);
        }
        out = update_json();
    } else {
        code = 404;
        out = "{\"error\":\"Not found\"}";
    }
    if (code == 200 && out.rfind("{\"error\"", 0) == 0) {
        code = 400;
    }
    reply(fd, code, out);
}

std::string read_request(int fd, std::string * method, std::string * path, std::string * body) {
    std::string raw;
    char buf[2048];
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 65536) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, (size_t) n);
    }
    const size_t head = raw.find("\r\n\r\n");
    if (head == std::string::npos) {
        return "short request";
    }
    const size_t line = raw.find("\r\n");
    const std::string start = raw.substr(0, line);
    const size_t sp1 = start.find(' ');
    const size_t sp2 = start.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        return "bad request";
    }
    *method = start.substr(0, sp1);
    *path = start.substr(sp1 + 1, sp2 - sp1 - 1);
    if (path->empty() || path->find("..") != std::string::npos) {
        return "bad path";
    }
    size_t length = 0;
    const std::string headers = raw.substr(0, head);
    const std::string key = "Content-Length:";
    const size_t at = headers.find(key);
    if (at != std::string::npos) {
        length = (size_t) std::strtoul(headers.c_str() + at + key.size(), nullptr, 10);
    }
    if (length > 65536) {
        return "body too large";
    }
    *body = raw.substr(head + 4);
    while (body->size() < length) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        body->append(buf, (size_t) n);
    }
    if (body->size() > length) {
        body->resize(length);
    }
    return {};
}

void serve() {
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::printf("artemislm-ps5: library port failed to open\n");
        return;
    }
    int on = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr = {};
#ifdef __FreeBSD__
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) kPanelPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (sockaddr *) &addr, sizeof(addr)) != 0 || listen(srv, 4) != 0) {
        std::printf("artemislm-ps5: library port %d is in use\n", kPanelPort);
        close(srv);
        return;
    }
    std::printf("artemislm-ps5: library on port %d\n", kPanelPort);
    for (;;) {
        const int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        timeval tv = { 8, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        std::string method, path, body, error = read_request(fd, &method, &path, &body);
        if (error.empty()) {
            handle(fd, method, path, body);
        } else {
            reply(fd, 400, std::string("{\"error\":\"") + error + "\"}");
        }
        close(fd);
    }
}

}  // namespace

// The page on the chat port when nothing is loaded. llama-server is not started: with
// no model it would be router mode and could try to spawn a child, which this
// title cannot do. Only the chat page is served. Anything else, including
// /v1, answers that no model is loaded. A later load restarts the app, and
// llama-server then owns this port.
namespace {

constexpr int kPagePort = kChatPort;
constexpr const char * kPageFile = "/app0/assets/web/index.html";
constexpr const char * kIdleJson =
    "{\"error\":\"No model loaded. Open Library to download one, or Settings to add a folder.\"}";

std::string load_page_file() {
    std::ifstream in(kPageFile, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (body.size() > (1u << 20)) {
        return {};
    }
    return body;
}

std::string page_route(const std::string & path) {
    const size_t q = path.find('?');
    std::string route = q == std::string::npos ? path : path.substr(0, q);
    if (route.size() > 1 && route.back() == '/') {
        route.pop_back();
    }
    return route;
}

void reply_raw(int fd, int code, const char * type, const std::string & body, bool head_only) {
    const char * text = code == 200 ? "OK" : code == 204 ? "No Content" : code == 404 ? "Not Found"
                      : code == 400 ? "Bad Request" : "Error";
    const std::string head = "HTTP/1.0 " + std::to_string(code) + " " + text +
                             "\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: " + type +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n";
    send_all(fd, head);
    if (!head_only && !body.empty()) {
        send_all(fd, body);
    }
}

void serve_page() {
    const std::string page = load_page_file();
    if (page.empty()) {
        std::printf("artemislm-ps5: %s is missing\n", kPageFile);
    }
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::printf("artemislm-ps5: page port failed to open\n");
        return;
    }
    int on = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr = {};
#ifdef __FreeBSD__
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) kPagePort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (sockaddr *) &addr, sizeof(addr)) != 0 || listen(srv, 4) != 0) {
        std::printf("artemislm-ps5: page port %d is in use\n", kPagePort);
        close(srv);
        return;
    }
    std::printf("artemislm-ps5: page on port %d, no model loaded\n", kPagePort);
    for (;;) {
        const int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        timeval tv = { 8, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        std::string method, path, body, error = read_request(fd, &method, &path, &body);
        if (!error.empty()) {
            reply_raw(fd, 400, "application/json", std::string("{\"error\":\"") + error + "\"}", false);
        } else if (method == "OPTIONS") {
            reply_raw(fd, 204, "text/plain", "", true);
        } else {
            const std::string route = page_route(path);
            const bool head = method == "HEAD";
            const bool read = method == "GET" || head;
            const bool page_get = read && (route == "/" || route == "/index.html");
            const std::string idle_info = std::string("{\"name\":\"ArtemisLM PS5\",\"version\":\"") + PS5LM_VERSION +
                                          "\",\"chat_port\":" + std::to_string(kChatPort) +
                                          ",\"library_port\":" + std::to_string(kLibraryPort) + ",\"model\":null}";
            if (page_get && !page.empty()) {
                reply_raw(fd, 200, "text/html; charset=utf-8", page, head);
            } else if (page_get) {
                reply_raw(fd, 500, "text/plain", "The page file is missing", head);
            } else if (read && route == "/v1/models") {
                reply_raw(fd, 200, "application/json", "{\"object\":\"list\",\"data\":[]}", head);
            } else if (read && (route == "/artemis" || route == "/health")) {
                reply_raw(fd, route == "/health" ? 503 : 200, "application/json",
                          route == "/health" ? std::string(kIdleJson) : idle_info, head);
            } else {
                reply_raw(fd, 503, "application/json", kIdleJson, head);
            }
        }
        close(fd);
    }
}

}  // namespace

void page_start() { std::thread(serve_page).detach(); }

void panel_start(Market * market, double budget_gib) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_market = market;
        g_budget = budget_gib;
    }
    std::thread(serve).detach();
}

void panel_set_budget(double budget_gib) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_budget = budget_gib;
}

void panel_set_running(const std::string & path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_running = path;
}

int panel_generation() { return g_gen.load(); }

}  // namespace ps5lm
