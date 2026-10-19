// Reading a GitHub release and unpacking the title zip. The network call
// lives in update.cpp. These functions stay free of the console libraries
// so a host build can check them.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace ps5lm {

constexpr const char * kUpdateRepo = "joelsmithlowe/ArtemisLM-PS5";
constexpr const char * kInstallDir = "/data/homebrew/PPSA98010";
constexpr const char * kUpdateDir  = "/data/ArtemisLM/update";

struct ReleaseInfo {
    std::string version;  // 0.9.1, without a leading v
    std::string url;      // the zip on this repository's releases
    std::string sha256;   // lowercase hex from the release, or empty
    std::string name;
};

// Negative when a is older, 0 when equal, positive when a is newer.
// A version is three numbers. A leading v is ignored.
int version_cmp(const std::string & a, const std::string & b);

// The latest-release JSON from api.github.com. The zip must be named
// artemislm-ps5-app-v<version>.zip on this repository.
bool parse_release(const std::string & json, ReleaseInfo * out, std::string * error);

// Unpacks PPSA98010/ from a zip into `dest` (dest/eboot.bin). A path that
// leaves the title folder is refused and dest is removed.
bool extract_title_zip(const std::string & zip_path, const std::string & dest, std::string * error);

// dest/sce_sys/param.json names this title, and eboot.bin is present.
bool stage_ok(const std::string & dest, std::string * error);

// Copies the staged title onto install_dir, replacing each file by a rename.
// Each file is marked executable, which is what the console requires to start it.
bool install_title(const std::string & stage, const std::string & install_dir, std::string * error);

// Where a folder install should be written. `linked` is the console's record of
// the folder it launched, when that record is a real directory. `present` is
// every other folder copy found. A disk image is refused. /app0 is the running
// mount, not the folder to write.
struct InstallChoice {
    bool                     image = false;
    std::string              linked;
    std::vector<std::string> present;
    std::string              fallback;
    bool                     fallback_parent = false;
};

bool choose_install_dir(const InstallChoice & choice, std::string * dir, std::string * error);

// Lowercase hex SHA-256 of a file, or empty when it cannot be read.
std::string sha256_file(const std::string & path);

}  // namespace ps5lm
