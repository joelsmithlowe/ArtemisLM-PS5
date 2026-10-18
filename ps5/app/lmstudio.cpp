// SPDX-License-Identifier: GPL-3.0-or-later
#include "lmstudio.hpp"

#include "reference.hpp"
#include "stats.hpp"
#include "version.hpp"

namespace ps5lm {

namespace {

std::string content_text(const nlohmann::json & content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (!content.is_array()) {
        return {};
    }
    std::string out;
    for (const auto & part : content) {
        if (part.is_string()) {
            out += part.get<std::string>();
        } else if (part.is_object() && part.value("type", "") == "text") {
            out += part.value("text", "");
        }
    }
    return out;
}

bool role_ok(const std::string & role) {
    return role == "system" || role == "user" || role == "assistant" || role == "tool";
}

nlohmann::json message_of(const std::string & role, const std::string & text) {
    nlohmann::json message = nlohmann::json::object();
    message["role"] = role;
    message["content"] = text;
    return message;
}

void add_message(nlohmann::json * messages, const std::string & role, const std::string & text) {
    if (!messages || text.empty() || !role_ok(role)) {
        return;
    }
    messages->push_back(message_of(role, text));
}

void attach_plan(nlohmann::json * messages, const std::string & plan) {
    if (!messages || plan.empty()) {
        return;
    }
    const std::string line = "\n\nCoordinator plan:\n" + plan + "\nFollow the plan. This console keeps one model loaded.";
    if (!messages->empty() && (*messages)[0].is_object() && (*messages)[0].value("role", "") == "system" &&
        (*messages)[0].contains("content") && (*messages)[0]["content"].is_string()) {
        (*messages)[0]["content"] = (*messages)[0]["content"].get<std::string>() + line;
        return;
    }
    messages->insert(messages->begin(), message_of("system", std::string("You are ArtemisLM PS5.") + line));
}

std::string completion_text(const std::string & raw, std::string * error) {
    const nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (!j.is_object()) {
        if (error) {
            *error = "The model did not answer";
        }
        return {};
    }
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty() && j["choices"][0].is_object()) {
        const nlohmann::json & choice = j["choices"][0];
        if (choice.contains("message") && choice["message"].is_object()) {
            const std::string text = content_text(choice["message"].value("content", nlohmann::json()));
            if (!text.empty()) {
                return text;
            }
            if (choice["message"].contains("reasoning_content") && choice["message"]["reasoning_content"].is_string()) {
                const std::string reason = choice["message"]["reasoning_content"].get<std::string>();
                if (!reason.empty()) {
                    return reason;
                }
            }
        }
    }
    if (error) {
        if (j.contains("error") && j["error"].is_string()) {
            *error = j["error"].get<std::string>();
        } else if (j.contains("error") && j["error"].is_object()) {
            *error = j["error"].value("message", "The model did not answer");
        } else {
            *error = "The model did not answer";
        }
    }
    return {};
}

nlohmann::json rpc_ok(const nlohmann::json & id, const nlohmann::json & result) {
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
}

nlohmann::json rpc_fail(const nlohmann::json & id, int code, const std::string & message) {
    return { { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
}

nlohmann::json tool_list() {
    const auto text = [](const char * about) {
        return nlohmann::json{ { "type", "string" }, { "description", about } };
    };
    nlohmann::json tools = nlohmann::json::array();
    tools.push_back({
        { "name", "artemis_status" },
        { "description", "Read ArtemisLM on the PlayStation. Reports the version, the loaded model, agent mode, and the chat address. One model stays on the GPU." },
        { "inputSchema", { { "type", "object" }, { "properties", nlohmann::json::object() } } },
    });
    tools.push_back({
        { "name", "artemis_models" },
        { "description", "List the models on the PlayStation. Each line has the name, whether it is loaded, whether it fits, the context, and the file path." },
        { "inputSchema", { { "type", "object" }, { "properties", nlohmann::json::object() } } },
    });
    tools.push_back({
        { "name", "artemis_ask" },
        { "description", "Ask the model loaded on the PlayStation. One model stays on the GPU. plan true writes a short plan first, then the answer, on that same model. This can take a few minutes." },
        { "inputSchema", {
            { "type", "object" },
            { "properties", {
                { "prompt", text("The question. Used when messages is empty.") },
                { "system", text("Optional note placed before the question.") },
                { "messages", { { "type", "array" }, { "description", "OpenAI chat messages. Used instead of prompt when present." } } },
                { "max_tokens", { { "type", "integer" }, { "description", "Cap on the answer. Omit it to let the loaded model decide." } } },
                { "plan", { { "type", "boolean" }, { "description", "Write a short plan first. Omit it to follow Agent mode on the console." } } },
            } },
        } },
    });
    tools.push_back({
        { "name", "artemis_load" },
        { "description", "Restart the PlayStation app onto a model file from artemis_models. One model stays on the GPU. After this, wait and call artemis_status before the next ask." },
        { "inputSchema", {
            { "type", "object" },
            { "properties", { { "path", text("Absolute path of a .gguf from artemis_models.") } } },
            { "required", { "path" } },
        } },
    });
    tools.push_back({
        { "name", "artemis_reference" },
        { "description", "Look up a small table stored on the PlayStation. No download. tool is convert, calendar, lookup, text, or note. convert needs value, from, and to. calendar op is weekday, between, or add. lookup kind is constant, element, http, or prefix. text op is base64, unbase64, hex, sha256, or count." },
        { "inputSchema", {
            { "type", "object" },
            { "properties", {
                { "tool", text("convert, calendar, lookup, text, or note.") },
                { "value", { { "type", "number" }, { "description", "The number to convert." } } },
                { "from", text("Unit to convert from, such as km or celsius.") },
                { "to", text("Unit to convert to, such as miles or fahrenheit.") },
                { "op", text("calendar: weekday, between, or add. text: base64, unbase64, hex, sha256, or count.") },
                { "date", text("YYYY-MM-DD, or a date such as 18 October 2026.") },
                { "end", text("The second date for calendar between.") },
                { "days", { { "type", "integer" }, { "description", "Days to add. Negative goes backward." } } },
                { "kind", text("lookup kind: constant, element, http, or prefix.") },
                { "name", text("Constant, element, or SI prefix. pi, iron, Fe, or nano.") },
                { "code", { { "type", "integer" }, { "description", "HTTP status code, such as 404." } } },
                { "text", text("Text to encode, or the question for tool note.") },
            } },
            { "required", { "tool" } },
        } },
    });
    return tools;
}

}  // namespace

AskBuilt build_ask(const nlohmann::json & in, const Settings & settings) {
    AskBuilt ask;
    if (!in.is_object()) {
        ask.error = "The ask was not a JSON object";
        return ask;
    }
    const std::string system = in.value("system", "");
    const std::string prompt = in.value("prompt", "");
    if (in.contains("messages") && in["messages"].is_array()) {
        for (const auto & message : in["messages"]) {
            if (!message.is_object()) {
                continue;
            }
            add_message(&ask.messages, message.value("role", ""), content_text(message.value("content", nlohmann::json())));
        }
    }
    if (ask.messages.empty()) {
        add_message(&ask.messages, "system", system);
        add_message(&ask.messages, "user", prompt);
    } else if (!system.empty()) {
        if (!ask.messages.empty() && ask.messages[0].value("role", "") == "system" && ask.messages[0]["content"].is_string()) {
            ask.messages[0]["content"] = ask.messages[0]["content"].get<std::string>() + "\n" + system;
        } else {
            ask.messages.insert(ask.messages.begin(), message_of("system", system));
        }
    }
    for (const auto & message : ask.messages) {
        if (message.value("role", "") == "user" && message.contains("content") && message["content"].is_string()) {
            ask.user_text = message["content"].get<std::string>();
        }
    }
    if (ask.user_text.empty() && !prompt.empty()) {
        add_message(&ask.messages, "user", prompt);
        ask.user_text = prompt;
    }
    if (ask.messages.empty() || ask.user_text.empty()) {
        ask.error = "Say what to ask";
        return ask;
    }
    if (in.contains("plan") && in["plan"].is_boolean()) {
        ask.want_plan = in["plan"].get<bool>();
    } else {
        ask.want_plan = settings.agent == "plan" || settings.agent == "handoff";
    }
    ask.plan_tokens = settings.plan_tokens == 128 || settings.plan_tokens == 512 ? (int) settings.plan_tokens : 256;
    if (in.contains("max_tokens") && in["max_tokens"].is_number_integer()) {
        int cap = in["max_tokens"].get<int>();
        if (cap < 1) {
            cap = 1;
        }
        if (cap > 8192) {
            cap = 8192;
        }
        ask.max_tokens = cap;
    }
    ask.ok = true;
    return ask;
}

void attach_reference(nlohmann::json * messages, const std::string & user) {
    const std::string note = reference_note(user);
    if (note.empty() || !messages) {
        return;
    }
    const std::string line = "Reference:\n" + note + "\nUse this figure. Do not replace it.";
    if (!messages->empty() && (*messages)[0].is_object() && (*messages)[0].value("role", "") == "system" &&
        (*messages)[0].contains("content") && (*messages)[0]["content"].is_string()) {
        (*messages)[0]["content"] = (*messages)[0]["content"].get<std::string>() + "\n\n" + line;
    } else {
        nlohmann::json note = nlohmann::json::object();
        note["role"] = "system";
        note["content"] = "You are ArtemisLM PS5.\n" + line;
        messages->insert(messages->begin(), note);
    }
}

AskResult run_ask(int port, const AskBuilt & ask) {
    AskResult got;
    nlohmann::json messages = ask.messages;
    attach_reference(&messages, ask.user_text);
    if (ask.want_plan && !ask.user_text.empty()) {
        nlohmann::json plan;
        plan["model"] = "artemis";
        plan["stream"] = false;
        plan["max_tokens"] = ask.plan_tokens;
        plan["chat_template_kwargs"] = { { "enable_thinking", false } };
        plan["messages"] = nlohmann::json::array({
            message_of("system", "You are the coordinator on ArtemisLM PS5. Write a short plan for the answer. At most 8 lines. Do not answer the question. One model is loaded."),
            message_of("user", "Plan this, do not answer:\n" + ask.user_text),
        });
        std::string ignored;
        got.plan = completion_text(http_post(port, "/v1/chat/completions", plan.dump(), 120), &ignored);
        if (got.plan.size() > 4000) {
            got.plan.resize(4000);
        }
        attach_plan(&messages, got.plan);
    }
    nlohmann::json body;
    body["model"] = "artemis";
    body["stream"] = false;
    body["messages"] = std::move(messages);
    body["chat_template_kwargs"] = { { "enable_thinking", false } };
    if (ask.max_tokens > 0) {
        body["max_tokens"] = ask.max_tokens;
    }
    got.answer = completion_text(http_post(port, "/v1/chat/completions", body.dump(), 300), &got.error);
    if (got.answer.empty() && got.error.empty()) {
        got.error = "The model did not answer";
    }
    if (!got.answer.empty()) {
        got.error.clear();
    }
    return got;
}

std::string ask_json(const AskResult & got, const std::string & loaded) {
    return nlohmann::json{
        { "object", "chat.completion" },
        { "model", "artemis" },
        { "choices", nlohmann::json::array({
            { { "index", 0 },
              { "finish_reason", "stop" },
              { "message", { { "role", "assistant" }, { "content", got.answer } } } },
        }) },
        { "artemis", { { "plan", got.plan }, { "loaded", loaded }, { "one_model", true } } },
    }.dump();
}

McpReply mcp_handle(const std::string & body, McpTool tool) {
    McpReply out;
    const nlohmann::json msg = nlohmann::json::parse(body, nullptr, false);
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method") || !msg["method"].is_string()) {
        out.status = 400;
        out.body = rpc_fail(nullptr, -32700, "The request was not JSON-RPC").dump();
        return out;
    }
    const bool has_id = msg.contains("id") && !msg["id"].is_null();
    const nlohmann::json id = has_id ? msg["id"] : nlohmann::json();
    const std::string method = msg["method"].get<std::string>();
    if (!has_id || method.rfind("notifications/", 0) == 0) {
        out.status = 202;
        return out;
    }
    const nlohmann::json params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : nlohmann::json::object();
    if (method == "initialize") {
        std::string version = params.value("protocolVersion", "2025-03-26");
        if (version.empty() || version.size() > 32) {
            version = "2025-03-26";
        }
        out.body = rpc_ok(id, {
            { "protocolVersion", version },
            { "capabilities", { { "tools", nlohmann::json::object() } } },
            { "serverInfo", { { "name", "ArtemisLM PS5" }, { "version", PS5LM_VERSION } } },
        }).dump();
        return out;
    }
    if (method == "ping") {
        out.body = rpc_ok(id, nlohmann::json::object()).dump();
        return out;
    }
    if (method == "tools/list") {
        out.body = rpc_ok(id, { { "tools", tool_list() } }).dump();
        return out;
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        nlohmann::json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : nlohmann::json::object();
        std::string error;
        std::string text;
        const bool known = name == "artemis_status" || name == "artemis_models" || name == "artemis_ask" ||
                       name == "artemis_load" || name == "artemis_reference";
        if (!known) {
            error = "That tool is not on this console";
        } else if (!tool) {
            error = "The console could not run that";
        } else {
            text = tool(name, args, &error);
        }
        if (text.empty() && error.empty()) {
            error = "The console could not run that";
        }
        if (text.size() > 24000) {
            text.resize(24000);
            text += "\nThe reply was cut.";
        }
        const std::string shown = !error.empty() && text.empty() ? error : text;
        out.body = rpc_ok(id, {
            { "content", nlohmann::json::array({ { { "type", "text" }, { "text", shown } } }) },
            { "isError", !error.empty() && text.empty() },
        }).dump();
        return out;
    }
    out.body = rpc_fail(id, -32601, "That method is not on this console").dump();
    return out;
}

}  // namespace ps5lm
