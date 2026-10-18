// Short notes in /data/ArtemisLM/memory, beside the models folder.
// Each note is stored as a hashed word vector. A question is given a note
// only when the words are close. This is not a second model.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ps5lm {

inline constexpr const char * kMemory = "/data/ArtemisLM/memory";

// body is JSON: tool is save, find, list, or forget.
// root is the memory folder (tests pass a temporary one).
// A failed call returns an object whose first key is "error".
std::string memory_call(const std::string & root, const std::string & body);

}  // namespace ps5lm
