// Projects live in /data/ArtemisLM/projects, next to the models folder.
// The console stores the files, reviews them, and checks a build in this
// process. A web page can be previewed. It cannot start Node, Python, or
// any other program.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ps5lm {

inline constexpr const char * kProjects = "/data/ArtemisLM/projects";

// body is JSON: tool is list, create, read, write, review, build, or remove.
// root is the projects folder (tests pass a temporary one).
// A failed call returns an object whose first key is "error".
std::string project_call(const std::string & root, const std::string & body);

}  // namespace ps5lm
