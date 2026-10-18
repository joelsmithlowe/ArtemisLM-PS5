// Chat, the page, and the OpenAI and Anthropic APIs share kChatPort.
// The library, settings, search, and model changes listen on kLibraryPort.
// These are the ports a browser on this console can open. 47281 is above
// IPPORT_EPHEMERALFIRST (10000): the page never accepted a connection there,
// and a model load then waited and stopped before the weights reached the GPU.
// The paths stay /v1/chat/completions, /v1/messages, and /v1/systemone.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5lm {

constexpr int kChatPort    = 8081;
constexpr int kLibraryPort = 8082;

}
