// The requests other devices make, as the Home page shows them. llama-server
// calls these hooks from its HTTP threads (patches/0007): a request's body when
// it arrives, every piece of the answer as it is sent, and the status at the
// end. Here they become a line of what was asked and the text answered.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "search_parse.hpp"
#include "stats.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using json = nlohmann::ordered_json;

namespace {

std::mutex                    g_mutex;
std::map<int, std::string>    g_partial;  // an answer's stream, up to its last full line
ps5lm::StatsCollector *       g_sink = nullptr;

// One line, spaces collapsed, at most `cap` bytes, cut at a character's start.
std::string one_line(const std::string & s, size_t cap) {
    std::string out;
    for (char c : s) {
        const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (space) {
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
        } else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.size() > cap) {
        size_t at = cap;
        while (at > 0 && ((unsigned char) out[at] & 0xC0) == 0x80) {
            --at;
        }
        out = out.substr(0, at) + "\xE2\x80\xA6";
    }
    return out;
}

// A message's content: text, or text parts with the images marked.
std::string text_of(const json & content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    std::string out;
    if (content.is_array()) {
        for (const auto & part : content) {
            const std::string type = part.value("type", "");
            if (type == "text" && part.contains("text") && part["text"].is_string()) {
                out += (out.empty() ? "" : " ") + part["text"].get<std::string>();
            } else if (type == "image_url" || type == "image" || type == "input_image") {
                out += (out.empty() ? "" : " ") + std::string("[image]");
            }
        }
    }
    return out;
}

// What was asked: the last user message, a decision request's state, or the prompt.
std::string asked_from(const std::string & path, const json & j) {
    if (!j.is_object()) {
        return {};
    }
    if (path.find("systemone") != std::string::npos) {
        std::string state = j.contains("state") ? (j["state"].is_string() ? j["state"].get<std::string>() : j["state"].dump()) : "";
        const size_t questions = j.contains("questions") && j["questions"].is_object() ? j["questions"].size() : 0;
        const size_t files = (j.contains("files") && j["files"].is_array() ? j["files"].size() : 0) +
                             (j.contains("images") && j["images"].is_array() ? j["images"].size() : 0);
        std::string head = std::to_string(questions) + (questions == 1 ? " question" : " questions");
        if (files) {
            head += ", " + std::to_string(files) + (files == 1 ? " image" : " images");
        }
        return head + (state.empty() || state == "null" ? "" : ": " + state);
    }
    if (j.contains("messages") && j["messages"].is_array()) {
        for (auto it = j["messages"].rbegin(); it != j["messages"].rend(); ++it) {
            if (it->is_object() && it->value("role", "") == "user" && it->contains("content")) {
                return text_of((*it)["content"]);
            }
        }
    }
    if (j.contains("prompt")) {
        return j["prompt"].is_string() ? j["prompt"].get<std::string>() : j["prompt"].dump();
    }
    return {};
}

// Without emoji, which the TV's fonts lack: 4-byte UTF-8 and U+FE0F go.
std::string printable(const std::string & s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char) s[i];
        const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n != 4 && s.compare(i, 3, "\xEF\xB8\x8F") != 0) {
            out.append(s, i, n);
        }
        i += n;
    }
    return out;
}

// "spam yes 0.99, intent sell, urgency 1.02"
std::string answers_of(const json & answers) {
    std::string out;
    char num[32];
    for (const auto & [key, a] : answers.items()) {
        std::string v;
        if (a.contains("choice") && a["choice"].is_string()) {
            v = a["choice"].get<std::string>();
        } else if (a.contains("noul") && a["noul"].is_number()) {
            const double p = a["noul"].get<double>();
            std::snprintf(num, sizeof(num), "%s %.2f", p >= 0.5 ? "yes" : "no", p);
            v = num;
        } else if (a.contains("score") && a["score"].is_number()) {
            std::snprintf(num, sizeof(num), "%.2f", a["score"].get<double>());
            v = num;
        }
        out += (out.empty() ? "" : ", ") + key + " " + v;
    }
    return out;
}

// The text in a whole answer, or in one event of a stream.
std::string answer_from(const json & j) {
    if (!j.is_object()) {
        return {};
    }
    if (j.contains("error")) {
        const json & e = j["error"];
        return "Error: " + (e.is_object() ? e.value("message", e.dump()) : e.is_string() ? e.get<std::string>() : e.dump());
    }
    if (j.contains("answers") && j["answers"].is_object()) {
        return answers_of(j["answers"]);
    }
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
        const json & c = j["choices"][0];
        for (const char * k : { "delta", "message" }) {
            if (c.contains(k) && c[k].is_object() && c[k].contains("content") && c[k]["content"].is_string()) {
                return c[k]["content"].get<std::string>();
            }
        }
        if (c.contains("text") && c["text"].is_string()) {
            return c["text"].get<std::string>();
        }
        return {};
    }
    // Anthropic: a text delta, or a whole message's text blocks.
    if (j.value("type", "") == "content_block_delta" && j.contains("delta") && j["delta"].is_object()) {
        return j["delta"].value("type", "") == "text_delta" ? j["delta"].value("text", "") : "";
    }
    if (j.contains("content")) {
        return text_of(j["content"]);
    }
    return {};
}

// A short plan from the loaded model. The TV cannot restart onto a second
// model in the middle of a question, so the same model answers afterwards.
std::string coordinator_plan(int port, const std::string & text, int plan_tokens) {
    if (plan_tokens != 128 && plan_tokens != 256 && plan_tokens != 512) {
        plan_tokens = 256;
    }
    json body;
    body["stream"] = false;
    body["max_tokens"] = plan_tokens;
    body["chat_template_kwargs"] = { { "enable_thinking", false } };
    body["messages"] = json::array({
        { { "role", "system" },
          { "content", "You are the coordinator on ArtemisLM PS5. Write a short plan for the answer. At most 8 lines. Do not answer the question. One model is loaded." } },
        { { "role", "user" }, { "content", "Plan this, do not answer:\n" + text } },
    });
    const std::string raw = ps5lm::http_post(port, "/v1/chat/completions", body.dump(), 120);
    const json got = json::parse(raw, nullptr, false);
    if (!got.is_object() || !got.contains("choices") || !got["choices"].is_array() || got["choices"].empty()) {
        return {};
    }
    const json & choice = got["choices"][0];
    if (!choice.contains("message") || !choice["message"].is_object()) {
        return {};
    }
    const json & content = choice["message"]["content"];
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (choice["message"].contains("reasoning_content") && choice["message"]["reasoning_content"].is_string()) {
        return choice["message"]["reasoning_content"].get<std::string>();
    }
    return {};
}

}  // namespace

namespace ps5lm {

void set_activity_sink(StatsCollector * stats) { g_sink = stats; }

void ask_self(int port, const std::string & text, bool decision, bool clock_on, bool web_on, int search_hits,
              const std::string & agent, int plan_tokens) {
    json body;
    if (decision) {
        // Two questions any text answers: what its writer wants, and their mood.
        body["state"] = text;
        body["questions"]["intent"] = { { "type", "choice" },
                                        { "instructions", "What does the writer want?" },
                                        { "criteria", { { "refund", nullptr }, { "information", nullptr },
                                                        { "cancel", nullptr }, { "praise", nullptr } } } };
        body["questions"]["angry"] = { { "type", "noul" }, { "instructions", "Is the writer angry?" } };
    } else {
        // The clock and the search run on a side thread, so the dashboard stays
        // responsive. The model answers from the notes. The TV request does not
        // carry tool definitions, so the model is not asked to call them itself.
        std::thread([port, text, clock_on, web_on, search_hits, agent, plan_tokens] {
            json messages = json::array();
            const LiveFacts facts = live_facts(text, clock_on, web_on, search_hits);
            std::string plan;
            if (agent == "plan" || agent == "handoff") {
                plan = coordinator_plan(port, text, plan_tokens);
            }
            if (facts.used_clock || facts.used_search || facts.used_calc || facts.used_ref || !plan.empty()) {
                std::string note = "You are ArtemisLM PS5. Answer from the notes below. "
                                   "Do not invent a rate, a price or a date. If the search failed, say so.";
                if (facts.used_clock) {
                    note += "\nConsole clock (get_datetime): " + facts.clock;
                }
                if (facts.used_calc) {
                    note += "\nCalculation: " + facts.calc;
                }
                if (facts.used_ref) {
                    note += "\nReference: " + facts.ref;
                }
                if (facts.used_search) {
                    note += "\nWeb search (web_search):\n" + facts.search;
                }
                if (!plan.empty()) {
                    note += "\nCoordinator plan:\n" + plan + "\nFollow the plan. This console keeps one model loaded.";
                }
                messages.push_back({ { "role", "system" }, { "content", note } });
            }
            messages.push_back({ { "role", "user" }, { "content", text } });
            json body;
            body["messages"] = std::move(messages);
            body["stream"] = true;
            body["max_tokens"] = 512;
            body["chat_template_kwargs"] = { { "enable_thinking", false } };
            http_post(port, "/v1/chat/completions", body.dump(), 300);
        }).detach();
        return;
    }
    std::thread([port, b = body.dump()] {
        http_post(port, "/v1/systemone", b, 300);
    }).detach();
}

}  // namespace ps5lm

extern "C" {

void ps5lm_activity_begin(int id, const char * method, const char * path, const char * remote, const char * body,
                          size_t body_len) {
    (void) method;
    if (!g_sink) {
        return;
    }
    const json j = json::parse(body, body + body_len, nullptr, false);
    const int questions = j.is_object() && j.contains("questions") && j["questions"].is_object() ? (int) j["questions"].size() : 0;
    g_sink->activity_begin(id, remote ? remote : "", path ? path : "", one_line(printable(asked_from(path ? path : "", j)), 400), questions);
}

void ps5lm_activity_data(int id, const char * data, size_t len) {
    if (!g_sink) {
        return;
    }
    std::string out;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string & buf = g_partial[id];
        buf.append(data, len);
        const bool stream = buf.rfind("data:", 0) == 0 || buf.rfind("event:", 0) == 0;
        if (!stream) {
            // A whole JSON answer arrives in one piece.
            const json j = json::parse(buf, nullptr, false);
            if (!j.is_discarded()) {
                out = answer_from(j);
                buf.clear();
            }
        } else {
            for (size_t nl = buf.find('\n'); nl != std::string::npos; nl = buf.find('\n')) {
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.rfind("data:", 0) != 0) {
                    continue;
                }
                const std::string payload = line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
                if (payload == "[DONE]") {
                    continue;
                }
                out += answer_from(json::parse(payload, nullptr, false));
            }
        }
    }
    out = printable(out);
    if (!out.empty()) {
        g_sink->activity_text(id, out);
    }
}

void ps5lm_activity_end(int id, int status) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_partial.erase(id);
    }
    if (g_sink) {
        g_sink->activity_end(id, status);
    }
}

}  // extern "C"
