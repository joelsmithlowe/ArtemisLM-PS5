// What the user can change on the Settings page, kept in
// /data/ArtemisLM/settings.json. Unknown or missing keys keep their defaults, so
// an old file still loads after a new setting is added.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace ps5lm {

struct Settings {
    bool        auto_load = false;  // load default_model at launch
    std::string default_model;      // a path from the library, or empty
    uint32_t    ctx_cap   = 65536;  // the planner's longest context
    std::string kv_type   = "auto"; // "auto", "f16", "q8_0" or "q4_0"
    // Vulkan flash attention is supported on this RADV (subgroup ops, fp16
    // shaders; no cooperative matrix). "on" is the path the console was
    // measured with. "auto" lets llama.cpp decide. "off" builds the full
    // score matrix and needs more memory.
    std::string flash_attn = "on";  // "on", "auto" or "off"
    // Prompt batch. "safe" is 256/64, the size that stays inside a GPU
    // submission this console finishes. Larger values fill a prompt faster
    // and can stall the device. Vision and decision models keep 2048.
    std::string batch = "safe";     // "safe", "medium" or "wide"
    uint32_t    slots = 1;          // chats at once; each one keeps its own cache (1 or 2)
    std::string sampling = "model"; // "model", "precise", "balanced" or "creative"
    int32_t     max_tokens = -1;    // where a reply stops; -1 runs until the model does
    std::string reasoning = "auto"; // "auto", "on" or "off": thinking, for models that have it
    std::string system_prompt = "home";  // "none", "helpful", "concise" or "home"
    bool        sounds    = true;
    bool        tools     = true;   // scratch, projects, folder listing, and notes under /data/ArtemisLM
    bool        web_search = true;  // web_search and web_fetch. A research question also reads one page
    bool        datetime  = true;   // the console's clock, as a tool
    uint32_t    scratch_limit_gib = 2;  // warn when the scratch folder grows past this
    std::string download_dir = "/data/ArtemisLM/models";  // where the market saves models
    // The home folder /data/ArtemisLM/models is always read. These add the
    // places the other apps use. PS5LM and PS5LM Plus both keep models in
    // /data/PS5LM/models.
    bool        scan_ps5lm = true;
    bool        scan_usb = true;     // ArtemisLM/models on M.2 and USB, and a .gguf on the USB root
    std::string extra_dir;           // one more absolute folder under /data or /mnt, or empty
    // "off" leaves other copies where they are. "ask" waits for Move them now.
    // "on" copies them into the home folder at the next scan and deletes the old file.
    std::string migrate = "ask";
    // "measured" is the spare the console was tested with. "tight" keeps less,
    // so a longer context can fit, and a load can fail. "roomy" keeps more.
    // None of them moves a layer off the GPU.
    std::string memory = "measured";
    // Agent mode never loads two models at once. "off" is one reply. "plan"
    // writes a short plan, then the answer, on the model that is loaded.
    // "handoff" does that plan and then, from the page, restarts onto worker.
    std::string agent = "off";       // "off", "plan" or "handoff"
    std::string coordinator;         // a library path, or empty
    std::string worker;              // a library path, or empty
    uint32_t    plan_tokens = 256;   // 128, 256 or 512: the plan only, not the answer
    // Sampling extras. They are llama-server flags, applied at the next load.
    // They do not change -ngl, -fit, flash attention, or the batch.
    std::string repeat_penalty = "off";  // "off", "light", "medium" or "strong"
    std::string min_p = "off";           // "off", "0.05" or "0.10"
    std::string seed = "random";         // "random", "42", "7" or "1234"
    uint32_t    remember = 16;           // messages the page sends back: 8, 16 or 32
    uint32_t    search_hits = 5;         // 3, 5 or 8

    bool operator==(const Settings &) const = default;
};

// An extra library folder: empty, or an absolute path under /data or /mnt
// with no ".." in it. Anything else is ignored.
bool extra_dir_ok(const std::string & path);

Settings load_settings(const char * path);
bool     save_settings(const char * path, const Settings & s);

}  // namespace ps5lm
