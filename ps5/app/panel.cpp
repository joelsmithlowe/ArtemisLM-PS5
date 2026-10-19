// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "ports.hpp"
#include "version.hpp"
#include "lmstudio.hpp"

#include "fetch.hpp"
#include "library.hpp"
#include "market.hpp"
#include "files.hpp"
#include "memory.hpp"
#include "projects.hpp"
#include "reference.hpp"
#include "model_plan.hpp"
#include "search_parse.hpp"
#include "page_http.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "update.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ps5lm {

namespace {

constexpr int kPanelPort = kLibraryPort;
constexpr const char * kSettingsPath = "/data/ArtemisLM/settings.json";
constexpr const char * kHub = "https://huggingface.co";

std::mutex g_mu;
std::mutex g_panel;  // short library calls. An agent ask does not hold this.
Market * g_market = nullptr;
double g_budget = 0;
std::string g_running;
std::string g_fault;
std::atomic<int> g_gen{ 0 };
std::atomic<bool> g_panel_stop{ false };
std::atomic<int> g_panel_fd{ -1 };

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
        // One send of the whole phone page has failed to leave this console.
        const size_t chunk = std::min(bytes.size() - sent, (size_t) 16384);
        const ssize_t n = send(fd, bytes.data() + sent, chunk, 0);
        if (n <= 0) {
            return;
        }
        sent += (size_t) n;
    }
}

void reply(int fd, int code, const std::string & body) {
    const char * text = code == 200 ? "OK" : code == 204 ? "No Content" : code == 202 ? "Accepted"
                      : code == 400 ? "Bad Request" : code == 405 ? "Method Not Allowed" : "Error";
    const std::string head = "HTTP/1.0 " + std::to_string(code) + " " + text +
                             "\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                             "Access-Control-Allow-Headers: Content-Type, Authorization, Accept, MCP-Protocol-Version\r\n"
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
    std::string fault;
    std::string agent = "off";
    {
        std::lock_guard<std::mutex> lock(g_mu);
        budget = g_budget;
        running = g_running;
        fault = g_fault;
        agent = load_settings(kSettingsPath).agent;
    }
    const std::string ip = local_address();
    nlohmann::json out = {
        { "name", "ArtemisLM PS5" },
        { "version", PS5LM_VERSION },
        { "chat_port", kChatPort },
        { "library_port", kLibraryPort },
        { "budget_gib", budget },
        { "agent", agent },
        { "loaded", running.empty() ? nlohmann::json(nullptr) : nlohmann::json(running) },
        { "fault", fault },
        { "note", "One model stays on the GPU. Any model name works on the chat port. LM Studio can use the mcp address." },
    };
    if (!ip.empty()) {
        out["chat"] = std::string("http://") + ip + ":" + std::to_string(kChatPort) + "/v1";
        out["mcp"] = std::string("http://") + ip + ":" + std::to_string(kLibraryPort) + "/mcp";
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
            std::string sha;
            if (e.contains("lfs") && e["lfs"].is_object()) {
                bytes = e["lfs"].value("size", bytes);
                sha = e["lfs"].value("oid", std::string());
                if (sha.rfind("sha256:", 0) == 0) {
                    sha = sha.substr(7);
                }
            }
            files.push_back({ { "file", path }, { "bytes", bytes }, { "sha", sha } });
        }
    }
    return nlohmann::json{ { "repo", repo }, { "files", std::move(files) }, { "error", error } }.dump();
}

// What Agent mode can promise. Each named model is planned on its own.
// The two totals are not added: the console loads one model at a time.
std::string agent_note_for(const Settings & s, double budget) {
    std::string note = "The console keeps one model on the GPU.";
    if (s.agent == "handoff") {
        note += " Hand off restarts onto the worker after the plan, about 30 seconds. On the TV, the loaded model writes the plan and the answer.";
    } else if (s.agent == "plan") {
        note += " Plan writes a short plan, then the answer, on the loaded model.";
    } else {
        note += " Agent mode is off.";
    }
    if (!s.coordinator.empty() && s.coordinator == s.worker) {
        note += " Both roles are the same model, so there is no restart.";
    }
    if (s.coordinator.empty() && s.worker.empty()) {
        return note;
    }
    const auto have = find_models(library_dirs(s));
    const auto one = [&](const std::string & path, const char * role) -> std::string {
        if (path.empty()) {
            return {};
        }
        bool known = false;
        for (const auto & got : have) {
            if (got == path) {
                known = true;
                break;
            }
        }
        if (!known) {
            return std::string(role) + " is not in the library.";
        }
        if (budget <= 0) {
            return std::string(role) + " waits for the GPU.";
        }
        const ModelInfo m = read_model_info(path);
        if (!m.decision.empty()) {
            return std::string(role) + " is a decision model, which does not chat.";
        }
        const char * kv = s.kv_type == "auto" ? nullptr : s.kv_type.c_str();
        RunOptions run;
        run.flash_attn = s.flash_attn;
        run.batch = s.batch;
        run.slots = s.slots;
        run.memory = s.memory;
        const Plan p = m.ok ? plan_model(m, budget, s.ctx_cap, kv, run) : Plan{};
        if (!m.ok || !p.fits) {
            return std::string(role) + " does not fit.";
        }
        return std::string(role) + " fits on its own.";
    };
    const std::string coordinator = one(s.coordinator, "The coordinator");
    const std::string worker = one(s.worker, "The worker");
    if (!coordinator.empty()) {
        note += " " + coordinator;
    }
    if (!worker.empty()) {
        note += " " + worker;
    }
    return note;
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
    take_text("agent", s.agent);
    take_text("coordinator", s.coordinator);
    take_text("worker", s.worker);
    take_text("repeat_penalty", s.repeat_penalty);
    take_text("min_p", s.min_p);
    take_text("seed", s.seed);
    take_u("ctx_cap", s.ctx_cap);
    take_u("slots", s.slots);
    take_u("scratch_limit_gib", s.scratch_limit_gib);
    take_u("plan_tokens", s.plan_tokens);
    take_u("remember", s.remember);
    take_u("search_hits", s.search_hits);
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
    job.sha256 = j.value("sha", std::string());
    if (job.sha256.rfind("sha256:", 0) == 0) {
        job.sha256 = job.sha256.substr(7);
    }
    bool sha_ok = job.sha256.size() == 64;
    for (unsigned char c : job.sha256) {
        sha_ok = sha_ok && std::isxdigit(c);
    }
    if (!sha_ok) {
        job.sha256.clear();
    }
    if (!market->enqueue(job, drive_free_gib(dir))) {
        return nlohmann::json{ { "error", market->snapshot().note } }.dump();
    }
    return nlohmann::json{ { "ok", true }, { "note", market->snapshot().note }, { "dir", dir } }.dump();
}

const char * download_state(DownloadState state) {
    switch (state) {
        case DownloadState::queued: return "queued";
        case DownloadState::running: return "running";
        case DownloadState::retrying: return "retrying";
        case DownloadState::paused: return "paused";
        case DownloadState::failed: return "failed";
        case DownloadState::done: return "done";
    }
    return "queued";
}

std::string downloads_json() {
    Market * market = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        market = g_market;
    }
    nlohmann::json rows = nlohmann::json::array();
    std::string note;
    if (market) {
        const MarketView view = market->snapshot();
        note = view.note;
        for (const auto & d : view.downloads) {
            rows.push_back({ { "repo", d.repo }, { "file", d.file }, { "dir", d.dir }, { "bytes", d.bytes },
                             { "done", d.done }, { "mbps", d.mbps }, { "state", download_state(d.state) }, { "note", d.note } });
        }
    }
    return nlohmann::json{ { "downloads", std::move(rows) }, { "note", note } }.dump();
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
    bool restart = false;
    const std::string note = library_move_request(s, path, running, &restart);
    bump();
    if (restart) {
        return nlohmann::json{ { "ok", true }, { "restart", true }, { "note", note } }.dump();
    }
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
        Settings s;
        double budget = 0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = load_settings(kSettingsPath);
            budget = g_budget;
        }
        out = nlohmann::json{
            { "auto_load", s.auto_load }, { "default_model", s.default_model }, { "ctx_cap", s.ctx_cap },
            { "kv_type", s.kv_type }, { "flash_attn", s.flash_attn }, { "batch", s.batch }, { "slots", s.slots },
            { "sampling", s.sampling }, { "max_tokens", s.max_tokens }, { "reasoning", s.reasoning },
            { "system_prompt", s.system_prompt }, { "sounds", s.sounds }, { "tools", s.tools },
            { "web_search", s.web_search }, { "datetime", s.datetime }, { "scratch_limit_gib", s.scratch_limit_gib },
            { "download_dir", s.download_dir }, { "scan_ps5lm", s.scan_ps5lm }, { "scan_usb", s.scan_usb },
            { "extra_dir", s.extra_dir }, { "migrate", s.migrate }, { "memory", s.memory },
            { "agent", s.agent }, { "coordinator", s.coordinator }, { "worker", s.worker },
            { "plan_tokens", s.plan_tokens }, { "repeat_penalty", s.repeat_penalty },
            { "min_p", s.min_p }, { "seed", s.seed }, { "remember", s.remember },
            { "search_hits", s.search_hits }, { "agent_note", agent_note_for(s, budget) },
        }.dump();
    } else if (method == "GET" && route == "/search") {
        const std::string repo = query_value(path, "repo");
        out = repo.empty() ? search_repos(query_value(path, "q")) : search_files(repo);
    } else if (method == "POST" && route == "/settings") {
        out = save_from_page(body);
    } else if (method == "GET" && route == "/downloads") {
        out = downloads_json();
    } else if (method == "POST" && route == "/download") {
        out = download_file(body);
    } else if (method == "POST" && route == "/move") {
        out = act_move(body);
    } else if (method == "POST" && route == "/remove") {
        out = act_remove(body);
    } else if (method == "POST" && route == "/project") {
        Settings s;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = load_settings(kSettingsPath);
        }
        out = s.tools ? project_call(kProjects, body) : std::string("{\"error\":\"File tools are off\"}");
    } else if (method == "POST" && route == "/memory") {
        Settings s;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = load_settings(kSettingsPath);
        }
        out = s.tools ? memory_call(kMemory, body) : std::string("{\"error\":\"File tools are off\"}");
    } else if (method == "POST" && route == "/files") {
        Settings s;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = load_settings(kSettingsPath);
        }
        out = s.tools ? files_call(files_roots(), body) : std::string("{\"error\":\"File tools are off\"}");
    } else if (method == "POST" && route == "/reference") {
        out = reference_call(body);
    } else if (method == "POST" && route == "/load") {
        out = act_load(body);
    } else if (method == "POST" && route == "/unload") {
        out = act_unload();
    } else if (method == "GET" && route == "/update") {
        out = update_json();
    } else if (method == "GET" && route == "/v1/agent") {
        out = artemis_json();
    } else if (method == "GET" && route == "/mcp") {
        code = 405;
        out = "{\"note\":\"POST JSON-RPC here. LM Studio uses this path as an agent.\"}";
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

// The value of one header, matched at the start of a line. Names ignore case.
bool header_length(const std::string & headers, const char * name, size_t * length) {
    const size_t n = std::strlen(name);
    for (size_t at = 0; at < headers.size();) {
        const size_t end = headers.find("\r\n", at);
        const size_t line_end = end == std::string::npos ? headers.size() : end;
        if (line_end >= at + n + 1 && headers[at + n] == ':') {
            bool match = true;
            for (size_t i = 0; i < n; ++i) {
                if (std::tolower(static_cast<unsigned char>(headers[at + i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                *length = static_cast<size_t>(std::strtoul(headers.c_str() + at + n + 1, nullptr, 10));
                return true;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 2;
    }
    return false;
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
    // Header names are case-insensitive. With no Content-Length, keep the body
    // bytes already read. Treating that as length zero would drop them.
    const bool have_length = header_length(headers, "content-length", &length);
    if (have_length && length > 262144) {
        return "body too large";
    }
    *body = raw.substr(head + 4);
    if (have_length) {
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
    } else if (body->size() > 262144) {
        return "body too large";
    }
    return {};
}

std::string lmstudio_tool(const std::string & name, const nlohmann::json & args, std::string * error) {
    if (name == "artemis_status") {
        Settings s;
        double budget = 0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = load_settings(kSettingsPath);
            budget = g_budget;
        }
        const nlohmann::json info = nlohmann::json::parse(artemis_json(), nullptr, false);
        std::string text = info.value("name", "ArtemisLM PS5");
        text += " ";
        text += info.value("version", "");
        text += "\nLoaded: ";
        text += info.contains("loaded") && info["loaded"].is_string() ? info["loaded"].get<std::string>() : "none";
        text += "\nAgent mode: " + info.value("agent", "off") + "\n";
        if (info.contains("chat") && info["chat"].is_string()) {
            text += "Chat: " + info["chat"].get<std::string>() + "\n";
        }
        if (info.contains("mcp") && info["mcp"].is_string()) {
            text += "MCP: " + info["mcp"].get<std::string>() + "\n";
        }
        text += agent_note_for(s, budget);
        return text;
    }
    if (name == "artemis_models") {
        const nlohmann::json catalog = nlohmann::json::parse(catalog_json(), nullptr, false);
        if (!catalog.contains("data") || !catalog["data"].is_array() || catalog["data"].empty()) {
            return "No models in the library.";
        }
        std::string text;
        for (const auto & model : catalog["data"]) {
            const nlohmann::json extra = model.contains("artemislm") && model["artemislm"].is_object() ? model["artemislm"]
                                                                                                      : nlohmann::json::object();
            if (!text.empty()) {
                text += "\n";
            }
            text += model.value("id", "model");
            if (extra.value("loaded", false)) {
                text += "  loaded";
            }
            text += extra.value("fits", false) ? "  fits" : "  does not fit";
            if (model.contains("context_length") && model["context_length"].is_number()) {
                text += "  ctx " + std::to_string(model["context_length"].get<int>());
            }
            if (extra.contains("path") && extra["path"].is_string()) {
                text += "  " + extra["path"].get<std::string>();
            }
        }
        return text;
    }
    if (name == "artemis_ask") {
        Settings s;
        std::string running;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            s = load_settings(kSettingsPath);
            running = g_running;
        }
        if (running.empty()) {
            if (error) {
                *error = "No model loaded. Open Library on the console.";
            }
            return {};
        }
        const ModelInfo info = read_model_info(running);
        if (!info.decision.empty()) {
            if (error) {
                *error = "That model does not chat.";
            }
            return {};
        }
        const AskBuilt built = build_ask(args, s);
        if (!built.ok) {
            if (error) {
                *error = built.error;
            }
            return {};
        }
        const AskResult got = run_ask(kChatPort, built);
        if (got.answer.empty()) {
            if (error) {
                *error = got.error.empty() ? "The model did not answer" : got.error;
            }
            return {};
        }
        std::string text;
        if (!got.plan.empty()) {
            text += "Plan:\n" + got.plan + "\n\n";
        }
        text += got.answer;
        if (s.agent == "handoff") {
            text += "\n\nHand off restarts the app from the phone page. This answer stays on the loaded model.";
        }
        return text;
    }
    if (name == "artemis_reference") {
        const std::string raw = reference_call(args.dump());
        const nlohmann::json got = nlohmann::json::parse(raw, nullptr, false);
        if (!got.is_object()) {
            if (error) {
                *error = "The reference table did not answer";
            }
            return {};
        }
        if (got.contains("error") && got["error"].is_string()) {
            if (error) {
                *error = got["error"].get<std::string>();
            }
            return {};
        }
        return got.value("text", "");
    }
    if (name == "artemis_load") {
        const std::string raw = act_load(nlohmann::json{ { "path", args.value("path", "") } }.dump());
        const nlohmann::json got = nlohmann::json::parse(raw, nullptr, false);
        const std::string note = got.is_object() ? got.value("error", got.value("note", "")) : "";
        if (note == "That model is already loaded") {
            return note;
        }
        if (got.is_object() && got.contains("error")) {
            if (error) {
                *error = note.empty() ? "The console could not start the load" : note;
            }
            return {};
        }
        return note.empty() ? "The console is restarting with this model" : note;
    }
    if (error) {
        *error = "That tool is not on this console";
    }
    return {};
}

std::string agent_post(const std::string & body) {
    const nlohmann::json in = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    Settings s;
    std::string running;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = load_settings(kSettingsPath);
        running = g_running;
    }
    if (running.empty()) {
        return "{\"error\":\"No model loaded\"}";
    }
    const ModelInfo info = read_model_info(running);
    if (!info.decision.empty()) {
        return "{\"error\":\"That model does not chat\"}";
    }
    const AskBuilt built = build_ask(in, s);
    if (!built.ok) {
        return nlohmann::json{ { "error", built.error } }.dump();
    }
    const AskResult got = run_ask(kChatPort, built);
    if (got.answer.empty()) {
        return nlohmann::json{ { "error", got.error.empty() ? "The model did not answer" : got.error } }.dump();
    }
    return ask_json(got, running);
}

void reply_mcp(int fd, const McpReply & got) {
    if (got.status == 202) {
        send_all(fd, "HTTP/1.0 202 Accepted\r\nAccess-Control-Allow-Origin: *\r\n"
                     "MCP-Protocol-Version: 2025-03-26\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    const char * text = got.status == 400 ? "Bad Request" : "OK";
    const std::string head = "HTTP/1.0 " + std::to_string(got.status) + " " + text +
                             "\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Access-Control-Allow-Headers: Content-Type, Authorization, Accept, MCP-Protocol-Version\r\n"
                             "MCP-Protocol-Version: 2025-03-26\r\n"
                             "Content-Type: application/json\r\nContent-Length: " +
                             std::to_string(got.body.size()) + "\r\nConnection: close\r\n\r\n" + got.body;
    send_all(fd, head);
}

void connection(int fd) {
    try {
        timeval tv = { 8, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        std::string method, path, body, error = read_request(fd, &method, &path, &body);
        if (!error.empty()) {
            reply(fd, 400, std::string("{\"error\":\"") + error + "\"}");
        } else {
            const size_t q = path.find('?');
            const std::string route = path.substr(0, q);
            if (method == "POST" && route == "/mcp") {
                reply_mcp(fd, mcp_handle(body, lmstudio_tool));
            } else if (method == "POST" && route == "/v1/agent") {
                const std::string out = agent_post(body);
                reply(fd, out.rfind("{\"error\"", 0) == 0 ? 400 : 200, out);
            } else {
                std::lock_guard<std::mutex> lock(g_panel);
                handle(fd, method, path, body);
            }
        }
    } catch (const std::exception &) {
        reply(fd, 500, "{\"error\":\"The library could not answer\"}");
    }
    close(fd);
}

bool accept_gone(int err) {
    return err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ECONNABORTED || err == EPROTO || err == EBADF;
}

// Bind, or keep trying. A model load restarts the app with LoadExec while the
// previous process can still hold this port. Giving up on the first try left
// the page with no library for the rest of the run.
void serve() {
    int logged = 0;
    while (!g_panel_stop.load()) {
        net_up();
        const int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) {
            if (logged < 3) {
                std::printf("artemislm-ps5: library port failed to open, errno %d\n", errno);
                logged++;
            }
            sleep(1);
            continue;
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
        if (bind(srv, (sockaddr *) &addr, sizeof(addr)) != 0 || listen(srv, 16) != 0) {
            const int err = errno;
            if (logged < 3 || logged % 10 == 0) {
                std::printf("artemislm-ps5: library port %d is not open, errno %d%s\n", kPanelPort, err,
                            err == EADDRINUSE ? " (already in use)" : "");
            }
            logged++;
            close(srv);
            sleep(1);
            continue;
        }
        logged = 0;
        std::printf("artemislm-ps5: library on port %d\n", kPanelPort);
        g_panel_fd.store(srv);
        if (g_panel_stop.load()) {
            const int held = g_panel_fd.exchange(-1);
            if (held >= 0) {
                close(held);
            }
            return;
        }
        for (;;) {
            const int fd = accept(srv, nullptr, nullptr);
            if (fd < 0) {
                const int err = errno;
                if (g_panel_stop.load()) {
                    break;
                }
                if (accept_gone(err)) {
                    continue;
                }
                std::printf("artemislm-ps5: library accept stopped, errno %d\n", err);
                break;
            }
            std::thread(connection, fd).detach();
        }
        const int held = g_panel_fd.exchange(-1);
        if (held >= 0) {
            close(held);
        }
        if (g_panel_stop.load()) {
            return;
        }
        sleep(1);
    }
}

}  // namespace

// The page on the chat port when nothing is loaded. llama-server is not started: with
// no model it would be router mode and could try to spawn a child, which this
// title cannot do. GET / is the phone page. GET /v1 is only the API prefix, so a
// browser that opens it is sent to /. Anything else answers that no model is
// loaded. A later load restarts the app, and llama-server then owns this port.
namespace {

constexpr int kPagePort = kChatPort;
constexpr const char * kPageFile = "/app0/assets/web/index.html";
constexpr const char * kIdleJson =
    "{\"error\":\"No model loaded. Open Library to download one, or Settings to add a folder.\"}";

std::atomic<int> g_page_gen{ 0 };
std::atomic<bool> g_page_stop{ false };
std::atomic<int> g_page_fd{ -1 };

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

void tune_socket(int fd) {
#ifdef SO_NOSIGPIPE
    int nosig = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
#ifdef TCP_NODELAY
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#endif
}

void reply_raw(int fd, int code, const char * type, const std::string & body, bool head_only) {
    const char * text = code == 200 ? "OK" : code == 204 ? "No Content" : code == 404 ? "Not Found"
                      : code == 400 ? "Bad Request" : code == 500 ? "Internal Server Error" : "Error";
    const std::string head = "HTTP/1.0 " + std::to_string(code) + " " + text +
                             "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nContent-Type: " + type +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n";
    send_all(fd, head);
    if (!head_only && !body.empty()) {
        send_all(fd, body);
    }
}

void reply_redirect(int fd) {
    send_all(fd, "HTTP/1.0 302 Found\r\nLocation: /\r\nContent-Length: 0\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
}

void idle_client(int fd, const std::string & page) {
    timeval tv = { 4, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    tune_socket(fd);
    std::string method, path, body, error = read_request(fd, &method, &path, &body);
    if (!error.empty()) {
        reply_raw(fd, 400, "application/json", std::string("{\"error\":\"") + error + "\"}", false);
        return;
    }
    if (method == "OPTIONS") {
        reply_raw(fd, 204, "text/plain", "", true);
        return;
    }
    const bool head = method == "HEAD";
    const std::string idle_info = std::string("{\"name\":\"ArtemisLM PS5\",\"version\":\"") + PS5LM_VERSION +
                                  "\",\"chat_port\":" + std::to_string(kChatPort) +
                                  ",\"library_port\":" + std::to_string(kLibraryPort) + ",\"model\":null}";
    switch (idle_answer(method, idle_route(path))) {
    case IdleAnswer::page:
        if (!page.empty()) {
            reply_raw(fd, 200, "text/html; charset=utf-8", page, head);
        } else {
            reply_raw(fd, 500, "text/plain", "The page file is missing", head);
        }
        break;
    case IdleAnswer::redirect:
        reply_redirect(fd);
        break;
    case IdleAnswer::models:
        reply_raw(fd, 200, "application/json", "{\"object\":\"list\",\"data\":[]}", head);
        break;
    case IdleAnswer::health:
        reply_raw(fd, 503, "application/json", kIdleJson, head);
        break;
    case IdleAnswer::info:
        reply_raw(fd, 200, "application/json", idle_info, head);
        break;
    case IdleAnswer::other:
        reply_raw(fd, 503, "application/json", kIdleJson, head);
        break;
    }
}

bool page_body(const std::string & body) {
    return body.find("<!doctype") != std::string::npos || body.find("<html") != std::string::npos;
}

void check_page(std::function<void(const std::string &)> note, int gen) {
    std::string body;
    for (int i = 0; i < 3 && gen == g_page_gen.load(); ++i) {
        body = http_get(kPagePort, "/", 2);
        if (!page_body(body)) {
            // Loopback on this console can miss a socket bound to every address.
            // The phone uses the LAN address, so the check has to as well.
            const std::string ip = local_address();
            if (!ip.empty()) {
                body = http_get_host(ip.c_str(), kPagePort, "/", 2);
            }
        }
        if (page_body(body)) {
            if (gen == g_page_gen.load() && note) {
                note("");
            }
            std::printf("artemislm-ps5: page check ok, %zu bytes\n", body.size());
            return;
        }
        sleep(1);
    }
    if (gen != g_page_gen.load() || !note) {
        return;
    }
    const char * why = "The page port did not answer";
    if (body.find("missing") != std::string::npos) {
        why = "The page file is missing";
    } else if (!body.empty()) {
        why = "The page port answered, but not with the page";
    }
    std::printf("artemislm-ps5: page check failed: %s\n", why);
    note(why);
}

bool accept_transient(int err) {
    return err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ECONNABORTED || err == EPROTO;
}

void serve_page(const std::function<void(const std::string &)> & note) {
    // Shared with each request. A model load returns from this function while
    // a browser can still be reading the page, so the text cannot live on this stack.
    const auto page = std::make_shared<const std::string>(load_page_file());
    if (page->empty()) {
        std::printf("artemislm-ps5: %s is missing\n", kPageFile);
        if (note) {
            note("The page file is missing");
        }
    } else {
        std::printf("artemislm-ps5: page file %zu bytes\n", page->size());
    }
    for (;;) {
        if (g_page_stop.load()) {
            return;
        }
        net_up();
        const int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) {
            std::printf("artemislm-ps5: page port failed to open, errno %d\n", errno);
            if (note) {
                note("The page port did not open");
            }
            sleep(1);
            continue;
        }
        int on = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        tune_socket(srv);
        sockaddr_in addr = {};
#ifdef __FreeBSD__
        addr.sin_len = sizeof(addr);
#endif
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t) kPagePort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(srv, (sockaddr *) &addr, sizeof(addr)) != 0 || listen(srv, 16) != 0) {
            const int err = errno;
            std::printf("artemislm-ps5: page port %d is not open, errno %d\n", kPagePort, err);
            if (note) {
                char text[80];
                std::snprintf(text, sizeof(text), "Port %d did not open, errno %d%s", kPagePort, err,
                              err == EADDRINUSE ? ", already in use" : "");
                note(text);
            }
            close(srv);
            sleep(1);
            continue;
        }
        const std::string ip = local_address();
        std::printf("artemislm-ps5: page on port %d, no model loaded%s%s\n", kPagePort, ip.empty() ? "" : ", ",
                    ip.c_str());
        g_page_fd.store(srv);
        if (g_page_stop.load()) {
            const int held = g_page_fd.exchange(-1);
            if (held >= 0) {
                close(held);
            }
            return;
        }
        const int gen = g_page_gen.fetch_add(1) + 1;
        std::thread(check_page, note, gen).detach();
        for (;;) {
            const int fd = accept(srv, nullptr, nullptr);
            if (fd < 0) {
                const int err = errno;
                if (g_page_stop.load()) {
                    break;
                }
                if (accept_transient(err)) {
                    continue;
                }
                std::printf("artemislm-ps5: page accept stopped, errno %d\n", err);
                if (note) {
                    note("The page port stopped. Opening it again");
                }
                break;
            }
            std::thread([fd, page] {
                try {
                    idle_client(fd, *page);
                } catch (const std::exception &) {
                }
                close(fd);
            }).detach();
        }
        const int held = g_page_fd.exchange(-1);
        if (held >= 0) {
            close(held);
        }
        if (g_page_stop.load()) {
            return;
        }
        sleep(1);
    }
}

}  // namespace

void page_start(std::function<void(const std::string &)> note) {
    // The console's default thread stack is small. Reading the page stays on this one.
    auto saved = note;
    auto * run = new std::function<void()>([note = std::move(note)] { serve_page(note); });
    auto fail = [&](const char * why) {
        std::printf("artemislm-ps5: %s\n", why);
        delete run;
        if (saved) {
            saved("The page thread did not start");
        }
    };
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        fail("page thread setup failed");
        return;
    }
    if (pthread_attr_setstacksize(&attr, 4u << 20) != 0) {
        pthread_attr_destroy(&attr);
        fail("page thread stack failed");
        return;
    }
    pthread_t thread{};
    const int rc = pthread_create(&thread, &attr, [](void * p) -> void * {
        // The request threads hold their own copy of the page text. This
        // function can go once serve_page has returned.
        std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()> *>(p));
        (*owned)();
        return nullptr;
    }, run);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fail("page thread failed to start");
        return;
    }
    pthread_detach(thread);
}

void page_stop() {
    g_page_stop.store(true);
    const int fd = g_page_fd.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

void panel_start(Market * market, double budget_gib) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_market = market;
        g_budget = budget_gib;
    }
    std::thread(serve).detach();
}

void panel_stop() {
    g_panel_stop.store(true);
    const int fd = g_panel_fd.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

void panel_set_budget(double budget_gib) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_budget = budget_gib;
}

void panel_set_running(const std::string & path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_running = path;
}

void panel_set_fault(const std::string & text) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_fault = text;
}

int panel_generation() { return g_gen.load(); }

}  // namespace ps5lm
