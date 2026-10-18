// LM Studio calls this console as an agent. The chat port stays the OpenAI
// server. This builds one ask, and it speaks MCP on the library port.
// One model stays on the GPU. A plan is a short completion, then the answer,
// both on the loaded model.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace ps5lm {

struct AskBuilt {
    bool             ok = false;
    std::string      error;
    bool             want_plan = false;
    int              plan_tokens = 256;
    int              max_tokens = 0;  // 0: the loaded server decides
    nlohmann::json   messages = nlohmann::json::array();
    std::string      user_text;
};

struct AskResult {
    std::string answer;
    std::string plan;
    std::string error;
};

// HTTP result for one MCP POST. 202 has an empty body (a notification).
struct McpReply {
    int         status = 200;
    std::string body;
};

// `in` is the tool arguments or the POST /v1/agent body.
AskBuilt build_ask(const nlohmann::json & in, const Settings & settings);

// Posts to the chat port on this console. Does not restart onto another model.
AskResult run_ask(int port, const AskBuilt & ask);

// OpenAI chat.completion shape, plus an artemis object. No "error" key.
std::string ask_json(const AskResult & got, const std::string & loaded);

// Tool returns the text LM Studio should read. On a failure, set *error.
using McpTool = std::string (*)(const std::string & name, const nlohmann::json & args, std::string * error);

McpReply mcp_handle(const std::string & body, McpTool tool);

}  // namespace ps5lm
