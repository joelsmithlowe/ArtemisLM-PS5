// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <initializer_list>

namespace ps5lm {

namespace {

// A value the planner would hand to llama-server unchanged. Anything else
// falls back, so a hand-edited file cannot make the server refuse to start.
bool model_choice_ok(const std::string & path);

bool one_of(const std::string & v, std::initializer_list<const char *> ok) {
    for (const char * s : ok) {
        if (v == s) {
            return true;
        }
    }
    return false;
}

void keep_known(Settings & s) {
    if (!one_of(s.kv_type, { "auto", "f16", "q8_0", "q4_0" })) {
        s.kv_type = "auto";
    }
    if (!one_of(s.flash_attn, { "on", "auto", "off" })) {
        s.flash_attn = "on";
    }
    if (!one_of(s.batch, { "safe", "medium", "wide" })) {
        s.batch = "safe";
    }
    if (s.slots < 1 || s.slots > 2) {
        s.slots = 1;
    }
    if (!one_of(s.sampling, { "model", "precise", "balanced", "creative" })) {
        s.sampling = "model";
    }
    if (s.max_tokens != -1 && s.max_tokens != 512 && s.max_tokens != 2048 && s.max_tokens != 8192) {
        s.max_tokens = -1;
    }
    if (!one_of(s.reasoning, { "auto", "on", "off" })) {
        s.reasoning = "auto";
    }
    if (!one_of(s.system_prompt, { "none", "helpful", "concise", "home" })) {
        s.system_prompt = "none";
    }
    static const uint32_t caps[] = { 4096, 8192, 16384, 32768, 65536, 131072 };
    bool cap_ok = false;
    for (uint32_t c : caps) {
        cap_ok = cap_ok || s.ctx_cap == c;
    }
    if (!cap_ok) {
        s.ctx_cap = 65536;
    }
    if (!one_of(s.migrate, { "off", "ask", "on" })) {
        s.migrate = "ask";
    }
    if (!one_of(s.memory, { "measured", "tight", "roomy" })) {
        s.memory = "measured";
    }
    if (!extra_dir_ok(s.extra_dir)) {
        s.extra_dir.clear();
    }
    if (!one_of(s.agent, { "off", "plan", "handoff" })) {
        s.agent = "off";
    }
    if (!model_choice_ok(s.coordinator)) {
        s.coordinator.clear();
    }
    if (!model_choice_ok(s.worker)) {
        s.worker.clear();
    }
    if (s.plan_tokens != 128 && s.plan_tokens != 256 && s.plan_tokens != 512) {
        s.plan_tokens = 256;
    }
    if (!one_of(s.repeat_penalty, { "off", "light", "medium", "strong" })) {
        s.repeat_penalty = "off";
    }
    if (!one_of(s.min_p, { "off", "0.05", "0.10" })) {
        s.min_p = "off";
    }
    if (!one_of(s.seed, { "random", "42", "7", "1234" })) {
        s.seed = "random";
    }
    if (s.remember != 8 && s.remember != 16 && s.remember != 32) {
        s.remember = 16;
    }
    if (s.search_hits != 3 && s.search_hits != 5 && s.search_hits != 8) {
        s.search_hits = 5;
    }
}

// A library file the agent rows may name. Empty is "no model". A bad value
// is dropped so a hand-edited file cannot point the load at something else.
bool model_choice_ok(const std::string & path) {
    if (path.empty()) {
        return true;
    }
    if (path.size() < 6 || path.size() > 512 || path[0] != '/' || path.compare(path.size() - 5, 5, ".gguf") != 0) {
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

}  // namespace

bool extra_dir_ok(const std::string & path) {
    if (path.empty()) {
        return true;
    }
    if (path.size() > 180 || path[0] != '/' || path.back() == '/') {
        return false;
    }
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos || path.find(' ') != std::string::npos) {
        return false;
    }
    return path.rfind("/data/", 0) == 0 || path.rfind("/mnt/", 0) == 0;
}

Settings load_settings(const char * path) {
    Settings      s;
    std::ifstream in(path);
    if (!in) {
        return s;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) {
        return s;
    }
    s.auto_load     = j.value("auto_load", s.auto_load);
    s.default_model = j.value("default_model", s.default_model);
    s.ctx_cap       = j.value("ctx_cap", s.ctx_cap);
    s.kv_type       = j.value("kv_type", s.kv_type);
    s.flash_attn    = j.value("flash_attn", s.flash_attn);
    s.batch         = j.value("batch", s.batch);
    s.slots         = j.value("slots", s.slots);
    s.sampling      = j.value("sampling", s.sampling);
    s.max_tokens    = j.value("max_tokens", s.max_tokens);
    s.reasoning     = j.value("reasoning", s.reasoning);
    s.system_prompt = j.value("system_prompt", s.system_prompt);
    s.sounds        = j.value("sounds", s.sounds);
    s.tools         = j.value("tools", s.tools);
    s.web_search    = j.value("web_search", s.web_search);
    s.datetime      = j.value("datetime", s.datetime);
    s.scratch_limit_gib = j.value("scratch_limit_gib", s.scratch_limit_gib);
    s.download_dir  = j.value("download_dir", s.download_dir);
    s.scan_ps5lm    = j.value("scan_ps5lm", s.scan_ps5lm);
    s.scan_usb      = j.value("scan_usb", s.scan_usb);
    s.extra_dir     = j.value("extra_dir", s.extra_dir);
    s.migrate       = j.value("migrate", s.migrate);
    s.memory        = j.value("memory", s.memory);
    s.agent         = j.value("agent", s.agent);
    s.coordinator   = j.value("coordinator", s.coordinator);
    s.worker        = j.value("worker", s.worker);
    s.plan_tokens   = j.value("plan_tokens", s.plan_tokens);
    s.repeat_penalty = j.value("repeat_penalty", s.repeat_penalty);
    s.min_p         = j.value("min_p", s.min_p);
    s.seed          = j.value("seed", s.seed);
    s.remember      = j.value("remember", s.remember);
    s.search_hits   = j.value("search_hits", s.search_hits);
    keep_known(s);
    return s;
}

bool save_settings(const char * path, const Settings & s) {
    const nlohmann::json j = {
        { "auto_load", s.auto_load }, { "default_model", s.default_model }, { "ctx_cap", s.ctx_cap },
        { "kv_type", s.kv_type },     { "flash_attn", s.flash_attn }, { "batch", s.batch },
        { "slots", s.slots },         { "sampling", s.sampling },     { "max_tokens", s.max_tokens },
        { "reasoning", s.reasoning }, { "system_prompt", s.system_prompt },
        { "sounds", s.sounds },       { "tools", s.tools },           { "web_search", s.web_search },
        { "datetime", s.datetime },
        { "scratch_limit_gib", s.scratch_limit_gib }, { "download_dir", s.download_dir },
        { "scan_ps5lm", s.scan_ps5lm }, { "scan_usb", s.scan_usb }, { "extra_dir", s.extra_dir },
        { "migrate", s.migrate }, { "memory", s.memory },
        { "agent", s.agent }, { "coordinator", s.coordinator }, { "worker", s.worker },
        { "plan_tokens", s.plan_tokens }, { "repeat_penalty", s.repeat_penalty },
        { "min_p", s.min_p }, { "seed", s.seed },
        { "remember", s.remember }, { "search_hits", s.search_hits },
    };
    std::ofstream out(path);
    out << j.dump(2) << "\n";
    return (bool) out;
}

}  // namespace ps5lm
