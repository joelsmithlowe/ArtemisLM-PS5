// Chat, the page, and the OpenAI and Anthropic APIs share kChatPort.
// The library, settings, search, and model changes listen on kLibraryPort.
// Both sit off the usual 8080 range. The paths stay /v1/chat/completions,
// /v1/messages, and /v1/systemone.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5lm {

constexpr int kChatPort    = 47281;
constexpr int kLibraryPort = 47282;

}
