// Host check for model_plan.cpp:
//   c++ -std=c++17 -O1 ps5/app/model_plan_test.cpp ps5/app/model_plan.cpp -o build/model_plan_test
//   build/model_plan_test          synthetic headers only
//   build/model_plan_test models   also the real GGUF files in models/
// Uses the budget the console reported (11.38 GiB free) and the two
// configurations measured there.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_plan.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// A GGUF header with string and uint32 metadata, no tensors.
struct Gguf {
    std::string kvs;
    uint64_t    n = 0;
    template <class T> static std::string raw(T v) { return std::string((const char *) &v, sizeof(v)); }
    static std::string str(const std::string & v) { return raw<uint64_t>(v.size()) + v; }
    Gguf & kv(const std::string & k, const std::string & v) { kvs += str(k) + raw<uint32_t>(8) + str(v); n++; return *this; }
    Gguf & kv(const std::string & k, uint32_t v) { kvs += str(k) + raw<uint32_t>(4) + raw(v); n++; return *this; }
    // magic, version 3, tensor count, metadata count, metadata
    std::string bytes() const { return "GGUF" + raw<uint32_t>(3) + raw<uint64_t>(0) + raw(n) + kvs; }
};

bool has_args(const ps5lm::Plan & p, const char * flag, const char * value) {
    auto it = std::find(p.args.begin(), p.args.end(), flag);
    return it != p.args.end() && it + 1 != p.args.end() && *(it + 1) == value;
}

}  // namespace

int main(int argc, char ** argv) {
    const double budget = 11.38;

    // A decision model (Clef is Qwen 3.5 9B with a decision head): found from
    // "<arch>.decision.type", a wide ubatch, a 4k cap and no chat sampling.
    Gguf clef;
    clef.kv("general.architecture", "qwen35").kv("general.name", "Clef Flash Qwen3.5 9B")
        .kv("qwen35.block_count", 32u).kv("qwen35.attention.head_count", 16u)
        .kv("qwen35.attention.head_count_kv", 4u).kv("qwen35.embedding_length", 4096u)
        .kv("qwen35.context_length", 262144u).kv("qwen35.decision.type", "clef");
    const ps5lm::ModelInfo d = ps5lm::read_model_header("clef-flash-Q4_K_M.gguf", clef.bytes(), 5ull << 30);
    assert(d.ok && d.decision == "clef");
    assert(!ps5lm::find_preset(d));
    const ps5lm::Plan dp = ps5lm::plan_model(d, budget);
    assert(dp.fits && dp.ctx == 4096 && has_args(dp, "-ub", "2048") && has_args(dp, "-b", "2048"));
    assert(dp.why.find("decision model (clef)") == 0);

    // The same model without the decision key is a chat model with the narrow batch.
    Gguf chat;
    chat.kv("general.architecture", "qwen35").kv("general.name", "Qwen3.5 9B")
        .kv("qwen35.block_count", 32u).kv("qwen35.attention.head_count", 16u)
        .kv("qwen35.attention.head_count_kv", 4u).kv("qwen35.embedding_length", 4096u)
        .kv("qwen35.context_length", 262144u);
    const ps5lm::ModelInfo c = ps5lm::read_model_header("Qwen3.5-9B-Q4_K_M.gguf", chat.bytes(), 5ull << 30);
    assert(c.ok && c.decision.empty() && ps5lm::find_preset(c));
    const ps5lm::Plan cp = ps5lm::plan_model(c, budget);
    assert(cp.fits && cp.ctx > 4096 && has_args(cp, "-ub", "64"));
    // The measured path stays put unless a setting asks otherwise: flash
    // attention on, the small batch, one slot, the family's sampling.
    assert(has_args(cp, "-fa", "on") && has_args(cp, "-b", "256") && has_args(cp, "-np", "1"));
    assert(has_args(cp, "--temp", "0.6") && !has_args(cp, "--reasoning", "off") && !has_args(cp, "-n", "512"));
    assert(!has_args(cp, "--seed", "42") && !has_args(cp, "--repeat-penalty", "1.10"));

    ps5lm::RunOptions custom;
    custom.batch = "wide";
    custom.flash_attn = "off";
    custom.slots = 2;
    custom.sampling = "precise";
    custom.max_tokens = 512;
    custom.reasoning = "off";
    custom.system_prompt = "concise";
    custom.repeat_penalty = "medium";
    custom.min_p = "0.05";
    custom.seed = "42";
    const ps5lm::Plan tuned = ps5lm::plan_model(c, budget, 65536, nullptr, custom);
    assert(has_args(tuned, "-fa", "off") && has_args(tuned, "-b", "1024") && has_args(tuned, "-ub", "256"));
    assert(has_args(tuned, "-np", "2") && has_args(tuned, "--temp", "0.2") && !has_args(tuned, "--temp", "0.6"));
    assert(has_args(tuned, "-n", "512") && has_args(tuned, "--reasoning", "off"));
    assert(has_args(tuned, "-sys", "Answer in as few words as the question allows."));
    assert(has_args(tuned, "--repeat-penalty", "1.10") && has_args(tuned, "--min-p", "0.05"));
    assert(!has_args(tuned, "--min-p", "0") && has_args(tuned, "--seed", "42"));
    assert(has_args(tuned, "-ngl", "999") && has_args(tuned, "-fit", "off"));
    // A decision model still needs the wide ubatch, and it does not chat.
    const ps5lm::Plan dwide = ps5lm::plan_model(d, budget, 65536, nullptr, custom);
    assert(has_args(dwide, "-ub", "2048") && has_args(dwide, "-b", "2048") && !has_args(dwide, "--reasoning", "off"));
    assert(!has_args(dwide, "--seed", "42") && !has_args(dwide, "--repeat-penalty", "1.10"));

    // Tight and roomy only change the spare memory. The measured GPU flags stay.
    ps5lm::RunOptions tight;
    tight.memory = "tight";
    const ps5lm::Plan tp = ps5lm::plan_model(c, budget, 65536, nullptr, tight);
    assert(tp.reserve_gib < cp.reserve_gib && tp.reserve_gib > 0);
    assert(has_args(tp, "-ngl", "999") && has_args(tp, "-fit", "off"));
    assert(has_args(tp, "-fa", "on") && has_args(tp, "-b", "256") && has_args(tp, "-ub", "64"));
    ps5lm::RunOptions roomy;
    roomy.memory = "roomy";
    const ps5lm::Plan rp = ps5lm::plan_model(c, budget, 65536, nullptr, roomy);
    assert(rp.reserve_gib > cp.reserve_gib);
    assert(has_args(rp, "-ngl", "999") && has_args(rp, "-fit", "off") && has_args(rp, "-cram", "0"));

    // Nothing fits in too little memory. Spill keeps that file up by leaving
    // layers in RAM. A model that already fits stays on the measured GPU path.
    ps5lm::ModelInfo big;
    big.ok = true; big.file_bytes = 20ull << 30; big.n_layer = 32; big.n_head_kv = 8;
    big.key_length = big.value_length = 128;
    assert(!ps5lm::plan_model(big, budget).fits);
    ps5lm::RunOptions spill;
    spill.memory = "spill";
    const ps5lm::Plan sp = ps5lm::plan_model(big, budget, 65536, nullptr, spill);
    assert(sp.fits && sp.why.find("RAM") != std::string::npos);
    assert(!has_args(sp, "-ngl", "999") && has_args(sp, "-fit", "off") && has_args(sp, "-lm", "none"));
    assert(ps5lm::plan_model(c, budget, 65536, nullptr, spill).fits);
    assert(has_args(ps5lm::plan_model(c, budget, 65536, nullptr, spill), "-ngl", "999"));
    // A plan that fits with almost no spare still dies on the first token.
    // Measured keeps full GPU layers when the spare is comfortable.
    ps5lm::ModelInfo cusp;
    cusp.ok = true;
    cusp.file_bytes = (10ull << 30) + (1ull << 29);
    cusp.n_layer = 32;
    cusp.n_head_kv = 1;
    cusp.key_length = cusp.value_length = 1;
    const ps5lm::Plan cusp_plan = ps5lm::plan_model(cusp, budget);
    assert(cusp_plan.fits && cusp_plan.why.find("RAM") != std::string::npos);
    assert(!has_args(cusp_plan, "-ngl", "999") && has_args(cusp_plan, "-fit", "off") && has_args(cusp_plan, "-lm", "none"));
    // f16 does not fit, but a short q4 cache holds every layer. Spill must
    // load that plan instead of refusing the file.
    ps5lm::ModelInfo forced;
    forced.ok = true;
    forced.file_bytes = 10ull << 30;
    forced.n_layer = 32;
    forced.n_head_kv = 8;
    forced.key_length = forced.value_length = 256;
    assert(!ps5lm::plan_model(forced, budget, 65536, "f16").fits);
    const ps5lm::Plan rescued = ps5lm::plan_model(forced, budget, 65536, "f16", spill);
    assert(rescued.fits && rescued.ctx == 4096 && rescued.kv_type == "q4_0");
    assert(has_args(rescued, "-ngl", "999") && has_args(rescued, "-fit", "off") && has_args(rescued, "-lm", "none"));

    if (argc < 2) {
        std::puts("model_plan_test: ok (synthetic only)");
        return 0;
    }
    bool saw_q2k = false, saw_iq2xxs = false;
    for (const auto & path : ps5lm::find_models({ argv[1] })) {
        const ps5lm::ModelInfo m = ps5lm::read_model_info(path);
        assert(m.ok);
        const ps5lm::Plan p = ps5lm::plan_model(m, budget);
        const ps5lm::Preset * pre = ps5lm::find_preset(m);
        std::printf("%-34s %s L%u kvL%u kvh%u k%u v%u state %.0f MiB | %s | %s\n", m.file_name.c_str(),
                    m.arch.c_str(), m.n_layer, m.n_kv_layers(), m.n_head_kv, m.key_length, m.value_length,
                    m.state_bytes / 1048576.0, p.why.c_str(), pre ? pre->label : "-");
        if (m.arch == "qwen35") {
            // Qwen 3.8 27B: 16 of 64 layers keep a KV cache, 4 heads of 256.
            assert(m.n_layer == 64 && m.n_kv_layers() == 16 && m.n_head_kv == 4 && m.key_length == 256);
            assert(kv_bytes_per_token(m, "f16") == 65536.0);
            assert(m.state_bytes > 140.0 * 1048576 && m.state_bytes < 170.0 * 1048576);
            assert(pre && std::string(pre->label) == "Qwen 3.8");
        }
        if (m.file_name.find("UD-Q2_K_XL") != std::string::npos) {
            // Ran on the console: 64k with a q4_0 cache.
            assert(p.fits && p.ctx == 65536 && p.kv_type == "q4_0");
            saw_q2k = true;
        }
        if (m.file_name.find("UD-IQ2_XXS") != std::string::npos) {
            // 4k at f16 ran; 64k needs at most q8_0 at this size.
            assert(p.fits && p.ctx == 65536 && p.kv_type == "q8_0");
            saw_iq2xxs = true;
        }
    }
    assert(saw_q2k && saw_iq2xxs);
    std::puts("model_plan_test: ok");
}
