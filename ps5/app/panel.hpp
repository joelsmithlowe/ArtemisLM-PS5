// A small HTTP API on port 8082 for the phone and desktop page: the model
// library, settings, Hugging Face search, downloads, the reference tables,
// and LM Studio's agent.
// Chat stays on 8081. MCP is POST /mcp on this port.
// With no model loaded, page_start() serves that same page on 8081. It does
// not load a model and it does not start llama-server.
// The thread is its own; the dashboard reads panel_generation() and reloads
// settings when the page saves them.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "stats.hpp"

#include <functional>
#include <string>

namespace ps5lm {

class Market;

void panel_start(Market * market, double budget_gib);
// Close the library socket. A model change restarts the process, and the
// next one cannot listen while this socket is still open.
void panel_stop();
// Serves the phone page on 8081 until a model load restarts the app.
// `note` is called with an empty string when a browser can open the page,
// or with a short reason when it cannot. It may run on the page thread.
void page_start(std::function<void(const std::string &)> note);
// Close the page socket before that restart. See panel_stop().
void page_stop();
void panel_set_budget(double budget_gib);
void panel_set_running(const std::string & path);
// Shown on /artemis when the model stops. Empty clears it.
void panel_set_fault(const std::string & text);
// The latest console readings, for /artemis. The page shows them beside chat.
void panel_set_console(const Live & live);
int  panel_generation();

}  // namespace ps5lm
