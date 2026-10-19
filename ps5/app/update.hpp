// Check joelsmithlowe/ArtemisLM-PS5 releases and install a newer title zip into
// the folder the console launched. One check runs at launch. Cross on the
// update row, or the phone page, downloads it, checks the checksum, and closes
// the app. Open ArtemisLM again from the home screen.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ps5lm {

// install false: look up the latest release and remember it.
// install true: download that release when it is newer, then install it.
void update_start(bool install);

std::string update_status();
bool        update_installing();
bool        update_take_notice(std::string * text);
// The new files are in place. Close the app. Reloading /app0 would start the
// copy that is already running.
bool        update_take_close();

// For the phone page: version, status, whether a newer release is known.
std::string update_json();

}  // namespace ps5lm
