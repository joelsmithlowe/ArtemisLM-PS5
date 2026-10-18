// SPDX-License-Identifier: GPL-3.0-or-later
#include "files.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace ps5lm {

namespace {

constexpr int kMaxEntries = 80;
constexpr int kMaxRead = 12000;
constexpr int kMaxParts = 4;

std::mutex g_mu;

std::string fail(const std::string & message) {
    return nlohmann::json{ { "error", message } }.dump();
}

std::string trim_slash(std::string path) {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    return path;
}

bool ends_with(const std::string & text, const char * suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

bool text_ext(const std::string & name) {
    std::string lower = name;
    for (char & c : lower) {
        c = (char) std::tolower((unsigned char) c);
    }
    const char * exts[] = { ".txt", ".md", ".json", ".html", ".css", ".js", ".py", ".log", ".csv", ".svg" };
    for (const char * ext : exts) {
        if (ends_with(lower, ext) && lower.size() > std::strlen(ext)) {
            return true;
        }
    }
    return false;
}

bool component_ok(const std::string & part) {
    if (part.empty() || part.size() > 80 || part[0] == '.' || part[0] == ' ') {
        return false;
    }
    for (unsigned char c : part) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ') {
            continue;
        }
        return false;
    }
    return part.back() != ' ';
}

// A relative path inside one root. Empty is the root itself. No "..",
// no absolute path, no dotfile.
bool rel_ok(const std::string & rel) {
    if (rel.empty()) {
        return true;
    }
    if (rel.size() > 240 || rel[0] == '/' || rel[0] == '\\' || rel[0] == '.') {
        return false;
    }
    if (rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos || rel.find("..") != std::string::npos ||
        rel.find('\0') != std::string::npos) {
        return false;
    }
    int parts = 0;
    size_t at = 0;
    while (at < rel.size()) {
        const size_t slash = rel.find('/', at);
        const std::string part = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (!component_ok(part)) {
            return false;
        }
        ++parts;
        if (slash == std::string::npos) {
            break;
        }
        at = slash + 1;
        if (at == rel.size()) {
            return false;
        }
    }
    return parts >= 1 && parts <= kMaxParts;
}

std::string resolve(const std::string & root, const std::string & rel, std::string * error) {
    std::error_code ec;
    const std::string base = std::filesystem::path(trim_slash(root)).lexically_normal().generic_string();
    if (base.empty() || std::filesystem::is_symlink(base, ec)) {
        *error = "That path is not allowed";
        return {};
    }
    std::filesystem::path cur(base);
    if (!rel.empty()) {
        size_t at = 0;
        while (at < rel.size()) {
            const size_t slash = rel.find('/', at);
            const std::string part = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
            cur /= part;
            if (std::filesystem::is_symlink(cur, ec)) {
                *error = "That path is not allowed";
                return {};
            }
            if (slash == std::string::npos) {
                break;
            }
            at = slash + 1;
        }
    }
    const std::string normal = cur.lexically_normal().generic_string();
    if (normal != base && normal.rfind(base + "/", 0) != 0) {
        *error = "That path is not allowed";
        return {};
    }
    return normal;
}

std::string field(const nlohmann::json & body, const char * key) {
    if (!body.contains(key) || !body[key].is_string()) {
        return {};
    }
    return body[key].get<std::string>();
}

const FileRoot * find_root(const std::vector<FileRoot> & roots, const std::string & id) {
    for (const auto & root : roots) {
        if (root.id == id) {
            return &root;
        }
    }
    return nullptr;
}

bool root_ok(const FileRoot & root) {
    return !root.id.empty() && !root.path.empty() && root.path[0] == '/' && root.path.find("..") == std::string::npos;
}

std::string list_dir(const std::string & full, const std::string & id) {
    std::error_code ec;
    if (!std::filesystem::exists(full, ec) || ec) {
        return nlohmann::json{ { "entries", nlohmann::json::array() },
                               { "note", "That folder is not on this console" },
                               { "ok", true },
                               { "root", id } }
            .dump();
    }
    if (std::filesystem::is_symlink(full, ec) || !std::filesystem::is_directory(full, ec)) {
        return fail("That path is not a folder");
    }
    nlohmann::json entries = nlohmann::json::array();
    bool more = false;
    std::filesystem::directory_iterator it(full, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        return fail("Could not read that folder");
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if ((int) entries.size() >= kMaxEntries) {
            more = true;
            break;
        }
        std::error_code one;
        if (it->is_symlink(one)) {
            continue;
        }
        const bool dir = it->is_directory(one);
        const bool file = it->is_regular_file(one);
        if (!dir && !file) {
            continue;
        }
        const std::string name = it->path().filename().generic_string();
        if (name.empty() || name[0] == '.') {
            continue;
        }
        nlohmann::json row = { { "kind", dir ? "dir" : "file" }, { "name", name } };
        if (file) {
            const auto bytes = it->file_size(one);
            if (!one) {
                row["bytes"] = bytes > 0xffffffffu ? 0xffffffffu : (uint32_t) bytes;
            }
        }
        entries.push_back(std::move(row));
    }
    std::sort(entries.begin(), entries.end(), [](const nlohmann::json & a, const nlohmann::json & b) {
        return a.value("name", std::string()) < b.value("name", std::string());
    });
    nlohmann::json out = { { "entries", std::move(entries) }, { "ok", true }, { "root", id } };
    if (more) {
        out["note"] = "Showing 80 names";
    }
    return out.dump();
}

std::string read_file(const std::string & full, std::string * error) {
    std::error_code ec;
    if (std::filesystem::is_symlink(full, ec)) {
        *error = "That path is not allowed";
        return {};
    }
    if (!std::filesystem::is_regular_file(full, ec)) {
        *error = "That file does not exist";
        return {};
    }
    const auto bytes = std::filesystem::file_size(full, ec);
    if (ec) {
        *error = "Could not read the file";
        return {};
    }
    if (bytes > (uintmax_t) kMaxRead) {
        *error = "That file is over 12000 bytes";
        return {};
    }
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        *error = "Could not read the file";
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.find('\0') != std::string::npos) {
        *error = "That file is not text";
        return {};
    }
    return text;
}

}  // namespace

std::vector<FileRoot> files_roots() {
    return {
        { "projects", "/data/ArtemisLM/projects" }, { "scratch", "/data/ArtemisLM/scratch" },
        { "models", "/data/ArtemisLM/models" },     { "usb0", "/mnt/usb0" },
        { "usb1", "/mnt/usb1" },                    { "m2", "/mnt/ext0" },
    };
}

std::string files_call(const std::vector<FileRoot> & roots, const std::string & body) {
    std::lock_guard<std::mutex> lock(g_mu);
    const nlohmann::json json = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    if (!json.is_object()) {
        return fail("The file request is not valid");
    }
    const std::string tool = field(json, "tool");
    const std::string id = field(json, "root");
    if (tool == "list" && id.empty()) {
        nlohmann::json listed = nlohmann::json::array();
        for (const auto & root : roots) {
            if (!root_ok(root)) {
                continue;
            }
            std::error_code ec;
            const bool present = std::filesystem::is_directory(root.path, ec) && !std::filesystem::is_symlink(root.path, ec);
            listed.push_back({ { "id", root.id }, { "path", root.path }, { "present", present } });
        }
        return nlohmann::json{ { "ok", true }, { "roots", std::move(listed) } }.dump();
    }
    if (tool != "list" && tool != "read") {
        return fail("Unknown file tool");
    }
    const FileRoot * root = find_root(roots, id);
    if (!root || !root_ok(*root)) {
        return fail("Use projects, scratch, models, usb0, usb1, or m2");
    }
    const std::string rel = field(json, "path");
    if (!rel_ok(rel)) {
        return fail("That path is not allowed");
    }
    if (tool == "read" && (rel.empty() || !text_ext(rel))) {
        return fail("That file is not a text file");
    }
    std::string error;
    const std::string full = resolve(root->path, rel, &error);
    if (full.empty()) {
        return fail(error.empty() ? "That path is not allowed" : error);
    }
    if (tool == "list") {
        return list_dir(full, id);
    }
    std::string read_error;
    const std::string text = read_file(full, &read_error);
    if (!read_error.empty()) {
        return fail(read_error);
    }
    return nlohmann::json{ { "ok", true }, { "path", rel }, { "root", id }, { "text", text } }.dump();
}

}  // namespace ps5lm
