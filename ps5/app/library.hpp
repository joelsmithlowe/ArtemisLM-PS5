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
// One move at a time.
std::string library_move(const Settings & s, const std::string & path, const std::string & running);

// Delete one visible .gguf, its projector and its split parts. Refuses the
// model that is loaded.
std::string library_remove(const Settings & s, const std::string & path, const std::string & running);

}  // namespace ps5lm
