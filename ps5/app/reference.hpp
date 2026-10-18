// Small tables a model can call without a download or a new library.
// Units, calendar dates, physical constants, element names, HTTP statuses,
// SI prefixes, and short text codecs (base64, hex, SHA-256, counts).
// The figures are compiled into the title. Nothing is written under /data.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ps5lm {

// body is JSON. tool is convert, calendar, lookup, text, or note.
// A failed call returns an object whose first key is "error".
// A note that matches nothing still returns {"text":"..."} and says so.
std::string reference_call(const std::string & body);

// One sentence when the question is a plain conversion, a calendar date,
// a named constant, an element, an HTTP status, or an explicit encoding.
// Empty for weather, prices, exchange rates, and ordinary chat.
// The TV chat and an agent ask paste this in. They do not send tool definitions.
std::string reference_note(const std::string & question);

}  // namespace ps5lm
