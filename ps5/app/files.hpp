// List and read folders the title already uses: projects, scratch, models,
// and a USB or M.2 drive. A path cannot leave that folder.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace ps5lm {

struct FileRoot {
    std::string id;
    std::string path;
};

// The folders a console call may name. Tests pass their own list.
std::vector<FileRoot> files_roots();

// body is JSON: tool is list or read, root is an id from files_roots.
// A failed call returns an object whose first key is "error".
std::string files_call(const std::vector<FileRoot> & roots, const std::string & body);

}  // namespace ps5lm
