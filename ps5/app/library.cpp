// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.hpp"

#include "model_plan.hpp"

#include "stats.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace ps5lm {

namespace {

std::mutex g_files;

void add_dir(std::vector<std::string> & dirs, const std::string & path) {
    if (path.empty()) {
        return;
    }
    for (const auto & have : dirs) {
        if (have == path) {
            return;
        }
    }
    dirs.push_back(path);
}

std::string parent_dir(const std::string & path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string base_name(const std::string & path) {
    const size_t slash = path.find_last_of('/');
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

bool is_gguf(const std::string & name) {
    return name.size() > 5 && (name.compare(name.size() - 5, 5, ".gguf") == 0 || name.compare(name.size() - 5, 5, ".GGUF") == 0);
}

uint64_t file_bytes(const std::string & path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? (uint64_t) st.st_size : 0;
}



// The model, a projector beside it, and the other parts of a split file.
std::vector<std::string> bundle(const std::string & model) {
    std::vector<std::string> files;
    if (file_bytes(model) > 0) {
        files.push_back(model);
    }
    const std::string mm = find_mmproj(model);
    if (!mm.empty() && mm != model) {
        files.push_back(mm);
    }
    const std::string name = base_name(model);
    const size_t of = name.find("-00001-of-");
    if (of == std::string::npos) {
        return files;
    }
    const std::string dir = parent_dir(model);
    const std::string prefix = name.substr(0, of + 1);  // include the dash
    DIR * d = opendir(dir.c_str());
    if (!d) {
        return files;
    }
    while (dirent * e = readdir(d)) {
        const std::string n = e->d_name;
        if (!is_gguf(n) || n.rfind(prefix, 0) != 0 || n.find("-of-") == std::string::npos) {
            continue;
        }
        const std::string path = dir + "/" + n;
        bool seen = false;
        for (const auto & have : files) {
            seen = seen || have == path;
        }
        if (!seen) {
            files.push_back(path);
        }
    }
    closedir(d);
    return files;
}

bool copy_file(const std::string & from, const std::string & to, std::string * error) {
    std::error_code ec;
    const auto src = std::filesystem::file_size(from, ec);
    if (ec) {
        *error = "could not read " + base_name(from);
        return false;
    }
    if (std::filesystem::exists(to)) {
        const auto dst = std::filesystem::file_size(to, ec);
        if (!ec && dst == src) {
            return true;
        }
        *error = base_name(to) + " is already in the home folder and is a different size";
        return false;
    }
    std::filesystem::create_directories(parent_dir(to), ec);
    const std::string tmp = to + ".moving";
    std::filesystem::copy_file(from, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        *error = "the copy of " + base_name(from) + " did not finish";
        return false;
    }
    ec.clear();
    if (std::filesystem::file_size(tmp, ec) != src) {
        std::filesystem::remove(tmp);
        *error = "the copy of " + base_name(from) + " was short";
        return false;
    }
    std::filesystem::rename(tmp, to, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        *error = "could not save " + base_name(from);
        return false;
    }
    return true;
}

std::string move_one(const std::string & model, const std::string & running, double * free_gib) {
    const auto files = bundle(model);
    if (files.empty()) {
        return base_name(model) + " could not be read";
    }
    for (const auto & f : files) {
        if (f == running) {
            return base_name(model) + " is loaded. Unload it first";
        }
    }
    uint64_t need = 0;
    for (const auto & f : files) {
        const std::string to = std::string(kHomeModels) + "/" + base_name(f);
        std::error_code ec;
        const auto src = std::filesystem::file_size(f, ec);
        if (ec) {
            return "could not read " + base_name(f);
        }
        if (std::filesystem::exists(to)) {
            ec.clear();
            const auto dst = std::filesystem::file_size(to, ec);
            if (!ec && dst == src) {
                continue;
            }
            return base_name(f) + " is already in the home folder and is a different size";
        }
        need += (uint64_t) src;
    }
    // A same-size file is already there: the only work left is deleting the
    // old copy, which does not need a spare gigabyte. statvfs on a title is a
    // stub, so `free_gib` comes from ps5-exporter and is -1 when it is down.
    if (need > 0 && free_gib && *free_gib >= 0 && *free_gib < (double) need / 1073741824.0 + 1.0) {
        return "not enough free space in /data/ArtemisLM/models";
    }
    std::error_code ec;
    std::filesystem::create_directories(kHomeModels, ec);
    for (const auto & f : files) {
        std::string error;
        if (!copy_file(f, std::string(kHomeModels) + "/" + base_name(f), &error)) {
            return error;
        }
    }
    for (const auto & f : files) {
        std::filesystem::remove(f, ec);
    }
    if (free_gib && *free_gib >= 0 && need > 0) {
        *free_gib -= (double) need / 1073741824.0;
    }
    return {};
}

}  // namespace

std::vector<std::string> library_dirs(const Settings & s) {
    std::vector<std::string> dirs;
    add_dir(dirs, kHomeModels);
    if (s.scan_ps5lm) {
        add_dir(dirs, "/data/PS5LM/models");
        add_dir(dirs, "/mnt/ext0/PS5LM/models");
        add_dir(dirs, "/mnt/usb0/PS5LM/models");
        add_dir(dirs, "/mnt/usb1/PS5LM/models");
    }
    if (s.scan_usb) {
        add_dir(dirs, "/mnt/ext0/ArtemisLM/models");
        add_dir(dirs, "/mnt/usb0/ArtemisLM/models");
        add_dir(dirs, "/mnt/usb1/ArtemisLM/models");
        add_dir(dirs, "/mnt/usb0");
        add_dir(dirs, "/mnt/usb1");
    }
    if (extra_dir_ok(s.extra_dir)) {
        add_dir(dirs, s.extra_dir);
    }
    return dirs;
}

bool model_in_home(const std::string & path) {
    const std::string prefix = std::string(kHomeModels) + "/";
    return path.rfind(prefix, 0) == 0 && path.find('/', prefix.size()) == std::string::npos;
}

std::string model_folder(const std::string & path) {
    if (model_in_home(path)) {
        return "ArtemisLM";
    }
    if (path.rfind("/data/PS5LM/", 0) == 0 || path.find("/PS5LM/") != std::string::npos) {
        return "PS5LM";
    }
    if (path.rfind("/mnt/ext", 0) == 0) {
        return "M.2";
    }
    if (path.rfind("/mnt/usb", 0) == 0) {
        return "USB";
    }
    return "Other";
}

bool library_visible(const Settings & s, const std::string & path) {
    if (!is_gguf(base_name(path)) || path.find("..") != std::string::npos) {
        return false;
    }
    const std::string dir = parent_dir(path);
    for (const auto & have : library_dirs(s)) {
        if (have == dir) {
            return true;
        }
    }
    return false;
}

std::string library_move(const Settings & s, const std::string & path, const std::string & running) {
    std::lock_guard<std::mutex> lock(g_files);
    std::vector<std::string> targets;
    if (!path.empty()) {
        if (!library_visible(s, path)) {
            return "That file is not in a library folder";
        }
        if (model_in_home(path)) {
            return "It is already in the home folder";
        }
        targets.push_back(path);
    } else {
        for (const auto & found : find_models(library_dirs(s))) {
            if (!model_in_home(found)) {
                targets.push_back(found);
            }
        }
    }
    if (targets.empty()) {
        return "No models outside the home folder";
    }
    double free = drive_free_gib(kHomeModels);
    int moved = 0;
    std::string errors;
    for (const auto & target : targets) {
        const std::string error = move_one(target, running, &free);
        if (error.empty()) {
            ++moved;
        } else if (errors.size() < 240) {
            if (!errors.empty()) {
                errors += " ";
            }
            errors += error;
        }
    }
    if (moved == 0) {
        return errors.empty() ? "Nothing was moved" : errors;
    }
    std::string note = "Moved " + std::to_string(moved) + (moved == 1 ? " model" : " models") + " into /data/ArtemisLM/models";
    if (!errors.empty()) {
        note += ". " + errors;
    }
    return note;
}

std::string library_remove(const Settings & s, const std::string & path, const std::string & running) {
    std::lock_guard<std::mutex> lock(g_files);
    if (!library_visible(s, path)) {
        return "That file is not in a library folder";
    }
    const auto files = bundle(path);
    if (files.empty()) {
        return "That file could not be read";
    }
    for (const auto & f : files) {
        if (f == running) {
            return "Unload it before deleting it";
        }
    }
    std::error_code ec;
    for (const auto & f : files) {
        std::filesystem::remove(f, ec);
        if (ec) {
            return "Could not delete " + base_name(f);
        }
    }
    return "Deleted " + base_name(path);
}

}  // namespace ps5lm
