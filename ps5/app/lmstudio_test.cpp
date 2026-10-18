// Host check for the LM Studio ask builder and the MCP envelope.
//   c++ -std=c++20 -I <llama vendor> ps5/app/lmstudio_test.cpp ps5/app/lmstudio.cpp ps5/app/reference.cpp -o /tmp/lmstudio_test
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lmstudio.hpp"

#include <cstdio>
#include <string>

namespace ps5lm {
std::string http_post(int, const char *, const std::string &, int) { return {}; }
}

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

std::string tool_echo(const std::string & name, const nlohmann::json & args, std::string * error) {
    if (name == "artemis_ask" && args.value("prompt", "") == "boom") {
        if (error) {
            *error = "The model did not answer";
        }
        return {};
    }
    return name + ":" + args.value("prompt", args.value("path", ""));
}

}  // namespace

int main() {
    ps5lm::Settings settings;
    const ps5lm::AskBuilt plain = ps5lm::build_ask({ { "prompt", "Hello" } }, settings);
    expect(plain.ok && !plain.want_plan && plain.user_text == "Hello" && plain.messages.size() == 1, "plain ask");

    settings.agent = "plan";
    settings.plan_tokens = 128;
    const ps5lm::AskBuilt planned = ps5lm::build_ask({ { "prompt", "Plan me" }, { "system", "Be brief" } }, settings);
    expect(planned.ok && planned.want_plan && planned.plan_tokens == 128, "plan follows settings");
    expect(planned.messages.size() == 2 && planned.messages[0].value("role", "") == "system", "system then user");

    const ps5lm::AskBuilt skipped = ps5lm::build_ask({ { "prompt", "No plan" }, { "plan", false } }, settings);
    expect(skipped.ok && !skipped.want_plan, "plan false wins");

    const ps5lm::AskBuilt capped = ps5lm::build_ask({ { "prompt", "Cap" }, { "max_tokens", 90000 } }, settings);
    expect(capped.max_tokens == 8192, "max tokens cap");

    const ps5lm::AskBuilt empty = ps5lm::build_ask(nlohmann::json::object(), settings);
    expect(!empty.ok && empty.error == "Say what to ask", "empty ask");

    nlohmann::json messages = nlohmann::json::array({
        { { "role", "user" }, { "content", nlohmann::json::array({ { { "type", "text" }, { "text", "From parts" } } }) } },
    });
    const ps5lm::AskBuilt parts = ps5lm::build_ask({ { "messages", messages }, { "system", "Note" } }, settings);
    expect(parts.ok && parts.user_text == "From parts" && parts.messages[0].value("role", "") == "system", "text parts");

    nlohmann::json only_system = nlohmann::json::array({ { { "role", "system" }, { "content", "Note" } } });
    const ps5lm::AskBuilt joined = ps5lm::build_ask({ { "messages", only_system }, { "prompt", "Hi" } }, settings);
    expect(joined.ok && joined.user_text == "Hi" && joined.messages.back().value("content", "") == "Hi", "prompt joins messages");

    const ps5lm::McpReply bad = ps5lm::mcp_handle("nope", tool_echo);
    expect(bad.status == 400, "bad json");

    const ps5lm::McpReply note = ps5lm::mcp_handle(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})", tool_echo);
    expect(note.status == 202 && note.body.empty(), "notification");

    const ps5lm::McpReply hello = ps5lm::mcp_handle(
        R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})", tool_echo);
    const nlohmann::json hello_json = nlohmann::json::parse(hello.body, nullptr, false);
    expect(hello.status == 200 && hello_json["result"]["serverInfo"]["name"] == "ArtemisLM PS5", "initialize");
    expect(hello_json["result"]["protocolVersion"] == "2025-03-26", "protocol");

    const ps5lm::McpReply list = ps5lm::mcp_handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", tool_echo);
    const nlohmann::json list_json = nlohmann::json::parse(list.body, nullptr, false);
    int names = 0;
    for (const auto & tool : list_json["result"]["tools"]) {
        const std::string name = tool.value("name", "");
        if (name == "artemis_status" || name == "artemis_models" || name == "artemis_ask" || name == "artemis_load" ||
            name == "artemis_reference") {
            ++names;
        }
    }
    expect(names == 5, "five tools");

    const ps5lm::McpReply call = ps5lm::mcp_handle(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"artemis_ask","arguments":{"prompt":"Hi"}}})",
        tool_echo);
    const nlohmann::json call_json = nlohmann::json::parse(call.body, nullptr, false);
    expect(call_json["result"]["isError"] == false && call_json["result"]["content"][0]["text"] == "artemis_ask:Hi", "ask tool");

    const ps5lm::McpReply missing = ps5lm::mcp_handle(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"artemis_ask","arguments":{"prompt":"boom"}}})",
        tool_echo);
    const nlohmann::json missing_json = nlohmann::json::parse(missing.body, nullptr, false);
    expect(missing_json["result"]["isError"] == true, "ask error");

    const ps5lm::McpReply unknown = ps5lm::mcp_handle(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"shell"}})", tool_echo);
    const nlohmann::json unknown_json = nlohmann::json::parse(unknown.body, nullptr, false);
    expect(unknown_json["result"]["isError"] == true, "unknown tool");

    const ps5lm::McpReply ref = ps5lm::mcp_handle(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"artemis_reference","arguments":{"tool":"note","text":"5 km to miles"}}})",
        tool_echo);
    const nlohmann::json ref_json = nlohmann::json::parse(ref.body, nullptr, false);
    expect(ref_json["result"]["isError"] == false &&
               ref_json["result"]["content"][0]["text"].get<std::string>().rfind("artemis_reference:", 0) == 0,
           "reference tool");

    const ps5lm::AskResult answer{ "The sum is 4.", "Add the numbers.", {} };
    const nlohmann::json shaped = nlohmann::json::parse(ps5lm::ask_json(answer, "/data/ArtemisLM/models/a.gguf"), nullptr, false);
    expect(shaped["choices"][0]["message"]["content"] == "The sum is 4." && shaped["artemis"]["one_model"] == true, "ask json");
    expect(shaped["artemis"]["plan"] == "Add the numbers." && !shaped.contains("error"), "ask json has no error");

    if (fails) {
        std::fprintf(stderr, "lmstudio_test: %d failed\n", fails);
        return 1;
    }
    std::printf("lmstudio_test: ok\n");
    return 0;
}
