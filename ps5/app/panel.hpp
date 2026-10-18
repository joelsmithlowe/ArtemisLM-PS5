// A small HTTP API on port 47282 for the phone and desktop page: the model
// library, settings, Hugging Face search and downloads. Chat stays on 47281.
// With no model loaded, page_start() serves that same page on 47281. It does
// not load a model and it does not start llama-server.
// The thread is its own; the dashboard reads panel_generation() and reloads
// settings when the page saves them.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ps5lm {

class Market;

void panel_start(Market * market, double budget_gib);
void page_start();
void panel_set_budget(double budget_gib);
void panel_set_running(const std::string & path);
int  panel_generation();

}  // namespace ps5lm
