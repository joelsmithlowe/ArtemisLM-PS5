// Where models are read from, and how a copy outside the home folder is
// moved there. The home folder is always /data/ArtemisLM/models. PS5LM and
// PS5LM Plus share /data/PS5LM/models, so one switch reads both.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings.hpp"

#include <string>
#include <vector>

namespace ps5lm {

constexpr const char * kHomeModels = "/data/ArtemisLM/models";

// Home first, then the optional folders. A missing folder is skipped by the scan.
std::vector<std::string> library_dirs(const Settings & s);

bool model_in_home(const std::string & path);
std::string model_folder(const std::string & path);

// The file is a .gguf in one of the folders this settings object reads.
bool library_visible(const Settings & s, const std::string & path);

// Copy one model, or every model outside the home folder when path is empty,
// into kHomeModels, then delete the source. A projector and the other parts
// of a split file go with it. Refuses the model that is loaded. Blocks.
// One move at a time. The source is removed only after the copy's size matches.
std::string library_move(const Settings & s, const std::string & path, const std::string & running);

// A move of the model that is loaded. Writes /data/ArtemisLM/move and asks
// the app to restart with nothing loaded. The next boot copies, checks the
// new file, deletes the old one, and loads the new path.
constexpr const char * kPendingMove = "/data/ArtemisLM/move";

// The note for the screen. *restart is true when the console must restart
// before the copy can finish.
std::string library_move_request(const Settings & s, const std::string & path, const std::string & running, bool * restart);

// A move saved by library_move_request, run once nothing is loaded.
// On success *load_path is the file in the home folder and *from_path is the
// old path. Saved settings that still name the old path are pointed at the new one.
std::string library_finish_move(Settings * settings, std::string * load_path, std::string * from_path);

// Delete one visible .gguf, its projector and its split parts. Refuses the
// model that is loaded.
std::string library_remove(const Settings & s, const std::string & path, const std::string & running);

}  // namespace ps5lm
