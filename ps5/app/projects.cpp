// SPDX-License-Identifier: GPL-3.0-or-later
#include "projects.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace ps5lm {

namespace {

constexpr int kMaxName = 32;
constexpr int kMaxFile = 12000;
constexpr int kMaxFiles = 40;
constexpr int kMaxParts = 4;
constexpr const char * kMarker = ".artemis.json";

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

bool name_ok(const std::string & name) {
    if (name.size() < 1 || name.size() > (size_t) kMaxName) {
        return false;
    }
    if (!std::isalnum((unsigned char) name[0])) {
        return false;
    }
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool ext_ok(const std::string & part) {
    const char * exts[] = { ".json", ".html", ".mjs", ".cjs", ".css", ".svg", ".txt", ".md", ".py", ".js" };
    for (const char * ext : exts) {
        if (ends_with(part, ext) && part.size() > std::strlen(ext)) {
            return true;
        }
    }
    return false;
}

bool component_ok(const std::string & part) {
    if (part.empty() || part.size() > 64 || part[0] == '.') {
        return false;
    }
    for (unsigned char c : part) {
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

// A relative path inside one project. No absolute path, no "..", no dotfile.
bool path_ok(const std::string & rel) {
    if (rel.empty() || rel.size() > 160 || rel[0] == '/' || rel[0] == '\\' || rel[0] == '.') {
        return false;
    }
    if (rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos || rel.find("..") != std::string::npos) {
        return false;
    }
    int parts = 0;
    size_t at = 0;
    std::string last;
    while (at < rel.size()) {
        const size_t slash = rel.find('/', at);
        const std::string part = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (!component_ok(part)) {
            return false;
        }
        last = part;
        parts++;
        if (slash == std::string::npos) {
            break;
        }
        at = slash + 1;
        if (at == rel.size()) {
            return false;
        }
    }
    return parts >= 1 && parts <= kMaxParts && ext_ok(last);
}

std::string full_path(const std::string & root, const std::string & rel) {
    const std::string full = trim_slash(root) + "/" + rel;
    const std::string normal = std::filesystem::path(full).lexically_normal().generic_string();
    const std::string base = std::filesystem::path(trim_slash(root)).lexically_normal().generic_string();
    if (normal.rfind(base + "/", 0) != 0) {
        return {};
    }
    return normal;
}

bool blank(const std::string & text) {
    for (unsigned char c : text) {
        if (!std::isspace(c)) {
            return false;
        }
    }
    return true;
}

std::string read_text(const std::string & path, std::string * error) {
    std::error_code ec;
    if (std::filesystem::is_symlink(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        *error = "Could not read the file";
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "Could not read the file";
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > (size_t) kMaxFile) {
        *error = "That file is over 12000 bytes";
        return {};
    }
    if (text.find('\0') != std::string::npos) {
        *error = "That file contains a zero byte";
        return {};
    }
    return text;
}

std::string write_text(const std::string & path, const std::string & text) {
    std::error_code ec;
    if (std::filesystem::is_symlink(path, ec)) {
        return "That path is not allowed";
    }
    const auto parent = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return "Could not create the folder";
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return "Could not write the file";
    }
    out.write(text.data(), (std::streamsize) text.size());
    if (!out) {
        return "Could not write the file";
    }
    return {};
}

struct SourceScan {
    std::string error;
    bool listen = false;
    bool create_server = false;
};

// Brackets, strings, and comments. This is not a full parser: a JavaScript
// regex literal can confuse it, so generated code should use new RegExp.
SourceScan scan_source(const std::string & text, bool python) {
    SourceScan out;
    enum class Mode { Code, Line, Block, Single, Double, TripleS, TripleD, Tmpl };
    Mode mode = Mode::Code;
    bool esc = false;
    int line = 1;
    struct Open {
        char ch;
        int line;
    };
    std::vector<Open> brackets;
    auto fail = [&](const std::string & message) {
        if (out.error.empty()) {
            out.error = "line " + std::to_string(line) + ": " + message;
        }
    };
    auto ident = [&](char c, bool start) {
        const unsigned char u = (unsigned char) c;
        if (std::isalpha(u) || c == '_') {
            return true;
        }
        if (!python && c == '$') {
            return true;
        }
        return !start && std::isdigit(u);
    };
    for (size_t i = 0; i < text.size() && out.error.empty();) {
        const char c = text[i];
        const char n = i + 1 < text.size() ? text[i + 1] : '\0';
        const char n2 = i + 2 < text.size() ? text[i + 2] : '\0';
        if (mode == Mode::Line) {
            if (c == '\n') {
                mode = Mode::Code;
                line++;
            }
            i++;
            continue;
        }
        if (mode == Mode::Block) {
            if (c == '\n') {
                line++;
            }
            if (c == '*' && n == '/') {
                mode = Mode::Code;
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (mode == Mode::Single || mode == Mode::Double || mode == Mode::Tmpl) {
            const char q = mode == Mode::Single ? '\'' : mode == Mode::Double ? '"' : '`';
            if (c == '\n') {
                if (mode != Mode::Tmpl) {
                    fail("unclosed string");
                    break;
                }
                line++;
                i++;
                continue;
            }
            if (esc) {
                esc = false;
                i++;
                continue;
            }
            if (c == '\\') {
                esc = true;
                i++;
                continue;
            }
            if (c == q) {
                mode = Mode::Code;
                i++;
                continue;
            }
            i++;
            continue;
        }
        if (mode == Mode::TripleS || mode == Mode::TripleD) {
            const char q = mode == Mode::TripleS ? '\'' : '"';
            if (c == '\n') {
                line++;
            }
            if (c == q && n == q && n2 == q) {
                mode = Mode::Code;
                i += 3;
                continue;
            }
            i++;
            continue;
        }
        if (c == '\n') {
            line++;
            i++;
            continue;
        }
        if (python && c == '#') {
            mode = Mode::Line;
            i++;
            continue;
        }
        if (!python && c == '/' && n == '/') {
            mode = Mode::Line;
            i += 2;
            continue;
        }
        if (!python && c == '/' && n == '*') {
            mode = Mode::Block;
            i += 2;
            continue;
        }
        if (c == '\'') {
            if (python && n == '\'' && n2 == '\'') {
                mode = Mode::TripleS;
                i += 3;
                continue;
            }
            mode = Mode::Single;
            i++;
            continue;
        }
        if (c == '"') {
            if (python && n == '"' && n2 == '"') {
                mode = Mode::TripleD;
                i += 3;
                continue;
            }
            mode = Mode::Double;
            i++;
            continue;
        }
        if (!python && c == '`') {
            mode = Mode::Tmpl;
            i++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            brackets.push_back({ c, line });
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            const char want = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (brackets.empty() || brackets.back().ch != want) {
                fail(std::string("extra '") + c + "'");
                break;
            }
            brackets.pop_back();
            i++;
            continue;
        }
        if (ident(c, true)) {
            const size_t start = i++;
            while (i < text.size() && ident(text[i], false)) {
                i++;
            }
            if (!python) {
                const std::string tok = text.substr(start, i - start);
                if (tok == "listen") {
                    out.listen = true;
                }
                if (tok == "createServer") {
                    out.create_server = true;
                }
            }
            continue;
        }
        i++;
    }
    if (out.error.empty() && mode != Mode::Code && mode != Mode::Line) {
        const bool comment = mode == Mode::Block;
        out.error = "line " + std::to_string(line) + (comment ? ": unclosed comment" : ": unclosed string");
    }
    if (out.error.empty() && !brackets.empty()) {
        out.error = "line " + std::to_string(brackets.front().line) + ": unclosed '" + brackets.front().ch + "'";
    }
    return out;
}

struct Item {
    std::string rel;
    std::string full;
};

std::vector<Item> list_items(const std::string & dir) {
    std::vector<Item> items;
    std::error_code ec;
    const auto opt = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(dir, opt, ec);
    if (ec) {
        return items;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code one;
        if (it->is_symlink(one) || !it->is_regular_file(one)) {
            continue;
        }
        const std::string base = trim_slash(dir);
        const std::string got = it->path().generic_string();
        if (got.rfind(base + "/", 0) != 0) {
            continue;
        }
        const std::string rel = got.substr(base.size() + 1);
        if (rel == kMarker || !path_ok(rel)) {
            continue;
        }
        items.push_back({ rel, got });
        if (items.size() > 64) {
            break;
        }
    }
    std::sort(items.begin(), items.end(), [](const Item & a, const Item & b) { return a.rel < b.rel; });
    return items;
}

bool kind_ok(const std::string & kind) {
    return kind == "node-server" || kind == "static-page" || kind == "python";
}

std::string read_kind(const std::string & dir) {
    std::string error;
    const std::string text = read_text(trim_slash(dir) + "/" + kMarker, &error);
    if (!error.empty()) {
        return {};
    }
    const nlohmann::json marker = nlohmann::json::parse(text, nullptr, false);
    if (!marker.is_object() || !marker.contains("kind") || !marker["kind"].is_string()) {
        return {};
    }
    const std::string kind = marker["kind"].get<std::string>();
    return kind_ok(kind) ? kind : std::string();
}

void push_error(std::vector<std::string> & errors, const std::string & message) {
    if (errors.size() < 20) {
        errors.push_back(message);
    }
}

bool is_js(const std::string & rel) {
    return ends_with(rel, ".mjs") || ends_with(rel, ".cjs") || ends_with(rel, ".js");
}

std::string review_note(bool ok, const std::string & name, const std::string & kind, const std::string & main_file) {
    if (!ok) {
        return "Fix every error, then call project_review again. This console cannot start Node or Python.";
    }
    std::string how = "On a computer, run python main.py in that folder.";
    if (kind == "node-server") {
        how = "On a computer, run node " + main_file + " in that folder.";
    } else if (kind == "static-page") {
        how = "On a computer, open index.html in a browser.";
    }
    return "Stored and reviewed in " + std::string(kProjects) + "/" + name + ". This console cannot start Node or Python. " + how;
}

std::string do_list(const std::string & root, const std::string & name) {
    if (name.empty()) {
        nlohmann::json projects = nlohmann::json::array();
        std::error_code ec;
        std::filesystem::directory_iterator it(trim_slash(root), ec);
        if (ec) {
            return nlohmann::json{ { "ok", true }, { "projects", projects } }.dump();
        }
        const std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            std::error_code one;
            if (!it->is_directory(one) || it->is_symlink(one)) {
                continue;
            }
            const std::string one_name = it->path().filename().generic_string();
            if (!name_ok(one_name)) {
                continue;
            }
            const std::string kind = read_kind(it->path().generic_string());
            if (kind.empty()) {
                continue;
            }
            projects.push_back({ { "name", one_name }, { "kind", kind } });
            if (projects.size() >= 100) {
                break;
            }
        }
        return nlohmann::json{ { "ok", true }, { "projects", std::move(projects) } }.dump();
    }
    if (!name_ok(name)) {
        return fail("That project name is not allowed");
    }
    const std::string dir = trim_slash(root) + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return fail("That project does not exist");
    }
    const std::string kind = read_kind(dir);
    if (kind.empty()) {
        return fail("That project has no kind");
    }
    nlohmann::json files = nlohmann::json::array();
    for (const auto & item : list_items(dir)) {
        files.push_back(item.rel);
    }
    return nlohmann::json{ { "ok", true }, { "name", name }, { "kind", kind }, { "files", std::move(files) } }.dump();
}

std::string starter_server() {
    return
        "var http = require(\"http\");\n"
        "var server = http.createServer(function (req, res) {\n"
        "  res.writeHead(200, { \"Content-Type\": \"text/plain\" });\n"
        "  res.end(\"ok\\n\");\n"
        "});\n"
        "server.listen(8080);\n";
}

std::string starter_html(const std::string & name) {
    return
        "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" + name +
        "</title>\n<link rel=\"stylesheet\" href=\"style.css\">\n</head>\n<body>\n<h1>" + name +
        "</h1>\n<script src=\"app.js\"></script>\n</body>\n</html>\n";
}

std::string do_create(const std::string & root, const std::string & name, const std::string & kind) {
    if (!name_ok(name)) {
        return fail("That project name is not allowed");
    }
    if (!kind_ok(kind)) {
        return fail("That kind is not supported");
    }
    const std::string dir = trim_slash(root) + "/" + name;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        return fail("That project already exists");
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return fail("Could not create the project folder");
    }
    std::vector<std::pair<std::string, std::string>> files;
    if (kind == "node-server") {
        files.push_back({ "package.json", nlohmann::json{ { "name", name }, { "private", true }, { "main", "server.js" } }.dump() });
        files.push_back({ "server.js", starter_server() });
    } else if (kind == "static-page") {
        files.push_back({ "index.html", starter_html(name) });
        files.push_back({ "style.css", "body { margin: 2rem; font-family: sans-serif; }\n" });
        files.push_back({ "app.js", "document.title = document.title;\n" });
    } else {
        files.push_back({ "main.py", "def main():\n    print(\"ok\")\n\nif __name__ == \"__main__\":\n    main()\n" });
    }
    const std::string marker = nlohmann::json{ { "kind", kind }, { "name", name } }.dump();
    std::string wrote = write_text(dir + "/" + kMarker, marker);
    nlohmann::json names = nlohmann::json::array();
    for (const auto & file : files) {
        if (wrote.empty()) {
            wrote = write_text(dir + "/" + file.first, file.second);
        }
        names.push_back(file.first);
    }
    if (!wrote.empty()) {
        std::filesystem::remove_all(dir, ec);
        return fail(wrote);
    }
    return nlohmann::json{ { "ok", true }, { "name", name }, { "kind", kind }, { "files", std::move(names) } }.dump();
}

std::string do_read(const std::string & root, const std::string & name, const std::string & rel) {
    if (!name_ok(name)) {
        return fail("That project name is not allowed");
    }
    if (!path_ok(rel)) {
        return fail("That path is not allowed");
    }
    const std::string dir = trim_slash(root) + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return fail("That project does not exist");
    }
    const std::string full = full_path(dir, rel);
    if (full.empty()) {
        return fail("That path is not allowed");
    }
    std::string error;
    const std::string text = read_text(full, &error);
    if (!error.empty()) {
        return fail(error);
    }
    return nlohmann::json{ { "ok", true }, { "name", name }, { "path", rel }, { "text", text } }.dump();
}

std::string do_write(const std::string & root, const std::string & name, const std::string & rel, const std::string & text) {
    if (!name_ok(name)) {
        return fail("That project name is not allowed");
    }
    if (!path_ok(rel)) {
        return fail("That path is not allowed");
    }
    if (text.size() > (size_t) kMaxFile) {
        return fail("That file is over 12000 bytes");
    }
    if (text.find('\0') != std::string::npos) {
        return fail("That file contains a zero byte");
    }
    const std::string dir = trim_slash(root) + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return fail("That project does not exist");
    }
    const std::string full = full_path(dir, rel);
    if (full.empty()) {
        return fail("That path is not allowed");
    }
    if (std::filesystem::is_directory(full, ec)) {
        return fail("That path is a folder");
    }
    const bool exists = std::filesystem::is_regular_file(full, ec);
    if (!exists && (int) list_items(dir).size() >= kMaxFiles) {
        return fail("This project already has 40 files");
    }
    const std::string wrote = write_text(full, text);
    if (!wrote.empty()) {
        return fail(wrote);
    }
    return nlohmann::json{ { "ok", true }, { "name", name }, { "path", rel }, { "bytes", text.size() } }.dump();
}

struct ReviewedFile {
    std::string rel;
    std::string text;
    SourceScan scan;
};

struct Review {
    std::string fail;
    std::string kind;
    std::string main_file = "server.js";
    nlohmann::json files = nlohmann::json::array();
    std::vector<std::string> errors;
    std::vector<ReviewedFile> scanned;

    const ReviewedFile * find(const std::string & rel) const {
        for (const auto & one : scanned) {
            if (one.rel == rel) {
                return &one;
            }
        }
        return nullptr;
    }
};

std::string lower_copy(std::string text) {
    for (char & c : text) {
        c = (char) std::tolower((unsigned char) c);
    }
    return text;
}

std::string attr_value(const std::string & tag, const char * key) {
    const std::string low = lower_copy(tag);
    const size_t key_n = std::strlen(key);
    size_t at = 0;
    while (at < low.size()) {
        const size_t found = low.find(key, at);
        if (found == std::string::npos) {
            return {};
        }
        if (found > 0) {
            const unsigned char prev = (unsigned char) low[found - 1];
            if (std::isalnum(prev) || low[found - 1] == '-' || low[found - 1] == '_') {
                at = found + 1;
                continue;
            }
        }
        size_t p = found + key_n;
        while (p < low.size() && std::isspace((unsigned char) low[p])) {
            p++;
        }
        if (p >= low.size() || low[p] != '=') {
            at = found + 1;
            continue;
        }
        p++;
        while (p < tag.size() && std::isspace((unsigned char) tag[p])) {
            p++;
        }
        if (p >= tag.size()) {
            return {};
        }
        const char quote = tag[p];
        if (quote == '"' || quote == '\'') {
            const size_t end = tag.find(quote, p + 1);
            if (end == std::string::npos) {
                return {};
            }
            return tag.substr(p + 1, end - p - 1);
        }
        size_t end = p;
        while (end < tag.size() && !std::isspace((unsigned char) tag[end]) && tag[end] != '>') {
            end++;
        }
        return tag.substr(p, end - p);
    }
    return {};
}

struct HtmlTag {
    size_t begin = 0;
    size_t end = 0;
    std::string name;
    std::string href;
    std::string src;
    std::string rel;
};

std::vector<HtmlTag> find_tags(const std::string & html) {
    std::vector<HtmlTag> out;
    for (size_t i = 0; i < html.size();) {
        if (html[i] != '<') {
            i++;
            continue;
        }
        if (i + 3 < html.size() && html.compare(i, 4, "<!--") == 0) {
            const size_t end = html.find("-->", i + 4);
            i = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        if (i + 1 < html.size() && (html[i + 1] == '/' || html[i + 1] == '!')) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < html.size() && std::isalpha((unsigned char) html[j])) {
            j++;
        }
        if (j == i + 1) {
            i++;
            continue;
        }
        const std::string name = lower_copy(html.substr(i + 1, j - i - 1));
        bool in_single = false;
        bool in_double = false;
        size_t k = j;
        for (; k < html.size(); k++) {
            const char c = html[k];
            if (in_single) {
                if (c == '\'') {
                    in_single = false;
                }
                continue;
            }
            if (in_double) {
                if (c == '"') {
                    in_double = false;
                }
                continue;
            }
            if (c == '\'') {
                in_single = true;
                continue;
            }
            if (c == '"') {
                in_double = true;
                continue;
            }
            if (c == '>') {
                break;
            }
        }
        if (k >= html.size()) {
            break;
        }
        HtmlTag tag;
        tag.begin = i;
        tag.end = k + 1;
        tag.name = name;
        if (name == "link" || name == "script" || name == "img") {
            const std::string raw = html.substr(i, tag.end - i);
            tag.href = attr_value(raw, "href");
            tag.src = attr_value(raw, "src");
            tag.rel = attr_value(raw, "rel");
            out.push_back(tag);
        }
        i = tag.end;
    }
    return out;
}

// A project-relative file. Remote, root, and hash links are left alone.
std::string project_rel(std::string url) {
    const size_t hash = url.find('#');
    if (hash != std::string::npos) {
        url = url.substr(0, hash);
    }
    const size_t query = url.find('?');
    if (query != std::string::npos) {
        url = url.substr(0, query);
    }
    if (url.empty() || url.find("://") != std::string::npos) {
        return {};
    }
    if (url.rfind("//", 0) == 0 || url[0] == '/' || url[0] == '#') {
        return {};
    }
    if (url.rfind("data:", 0) == 0 || url.rfind("mailto:", 0) == 0) {
        return {};
    }
    if (url.rfind("./", 0) == 0) {
        url = url.substr(2);
    }
    return url;
}

std::string import_rel(const std::string & spec) {
    if (spec.empty()) {
        return {};
    }
    const bool local = spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0 || spec.find('/') != std::string::npos
        || ends_with(spec, ".js") || ends_with(spec, ".mjs") || ends_with(spec, ".cjs") || ends_with(spec, ".json")
        || ends_with(spec, ".py") || ends_with(spec, ".txt") || ends_with(spec, ".css") || ends_with(spec, ".html");
    if (!local) {
        return {};
    }
    if (spec.rfind("./", 0) == 0) {
        return spec.substr(2);
    }
    return spec;
}

bool line_comment_at(const std::string & text, size_t i) {
    const size_t line = text.rfind('\n', i == 0 ? 0 : i - 1);
    size_t start = line == std::string::npos ? 0 : line + 1;
    while (start < text.size() && std::isspace((unsigned char) text[start])) {
        start++;
    }
    if (start < text.size() && text[start] == '#') {
        return true;
    }
    return start + 1 < text.size() && text[start] == '/' && text[start + 1] == '/';
}

template <typename Fn>
void each_quoted_call(const std::string & text, const char * name, Fn fn) {
    const size_t n = std::strlen(name);
    for (size_t i = 0; i + n < text.size();) {
        if (text.compare(i, n, name) != 0) {
            i++;
            continue;
        }
        if (i > 0) {
            const unsigned char prev = (unsigned char) text[i - 1];
            if (std::isalnum(prev) || text[i - 1] == '_' || text[i - 1] == '$' || text[i - 1] == '.') {
                i++;
                continue;
            }
        }
        if (line_comment_at(text, i)) {
            i += n;
            continue;
        }
        size_t p = i + n;
        while (p < text.size() && std::isspace((unsigned char) text[p])) {
            p++;
        }
        if (p >= text.size() || text[p] != '(') {
            i++;
            continue;
        }
        p++;
        while (p < text.size() && std::isspace((unsigned char) text[p])) {
            p++;
        }
        if (p >= text.size() || (text[p] != '"' && text[p] != '\'')) {
            i = p;
            continue;
        }
        const char quote = text[p++];
        std::string spec;
        bool esc = false;
        for (; p < text.size(); p++) {
            const char c = text[p];
            if (esc) {
                spec.push_back(c);
                esc = false;
                continue;
            }
            if (c == '\\') {
                esc = true;
                continue;
            }
            if (c == quote) {
                break;
            }
            spec.push_back(c);
        }
        fn(spec);
        i = p < text.size() ? p + 1 : p;
    }
}

void note_missing(Review & review, const std::string & file, const std::string & spec, const char * verb) {
    const std::string rel = import_rel(spec);
    if (rel.empty()) {
        return;
    }
    if (!path_ok(rel)) {
        push_error(review.errors, file + " " + verb + " a file outside this project");
        return;
    }
    if (!review.find(rel)) {
        push_error(review.errors, file + " " + verb + " " + rel);
    }
}

void check_local_refs(Review & review) {
    for (const auto & file : review.scanned) {
        if (is_js(file.rel)) {
            each_quoted_call(file.text, "require", [&](const std::string & spec) {
                note_missing(review, file.rel, spec, "needs");
            });
        } else if (ends_with(file.rel, ".py")) {
            each_quoted_call(file.text, "open", [&](const std::string & spec) {
                note_missing(review, file.rel, spec, "opens");
            });
        }
    }
    if (review.kind != "static-page") {
        return;
    }
    const ReviewedFile * page = review.find("index.html");
    if (!page) {
        return;
    }
    for (const HtmlTag & tag : find_tags(page->text)) {
        std::string url;
        if (tag.name == "link") {
            url = tag.href;
            const std::string rel = lower_copy(tag.rel);
            const bool sheet = rel.find("stylesheet") != std::string::npos || ends_with(lower_copy(url), ".css");
            if (!sheet) {
                continue;
            }
        } else if (tag.name == "script") {
            url = tag.src;
            if (url.empty()) {
                continue;
            }
        } else {
            continue;
        }
        const std::string rel = project_rel(url);
        if (rel.empty()) {
            continue;
        }
        if (!path_ok(rel)) {
            push_error(review.errors, "index.html links to a file outside this project");
            continue;
        }
        if (!review.find(rel)) {
            push_error(review.errors, "index.html links to missing " + rel);
        }
    }
}

std::string break_close(std::string text, const char * marker) {
    const size_t n = std::strlen(marker);
    size_t at = 0;
    while ((at = text.find(marker, at)) != std::string::npos) {
        text.replace(at, n, std::string("<\\/") + (marker + 2));
        at += n + 1;
    }
    return text;
}

std::string preview_page(const Review & review) {
    const ReviewedFile * page = review.find("index.html");
    if (!page) {
        return {};
    }
    struct Swap {
        size_t begin;
        size_t end;
        std::string text;
    };
    std::vector<Swap> swaps;
    const std::string & html = page->text;
    for (const HtmlTag & tag : find_tags(html)) {
        bool as_script = false;
        std::string url;
        if (tag.name == "link") {
            url = tag.href;
            const std::string rel = lower_copy(tag.rel);
            const bool sheet = rel.find("stylesheet") != std::string::npos || ends_with(lower_copy(url), ".css");
            if (!sheet) {
                continue;
            }
        } else if (tag.name == "script" && !tag.src.empty()) {
            url = tag.src;
            as_script = true;
        } else {
            continue;
        }
        const std::string rel = project_rel(url);
        if (rel.empty()) {
            continue;
        }
        const ReviewedFile * file = review.find(rel);
        if (!file) {
            continue;
        }
        size_t end = tag.end;
        if (as_script) {
            const size_t close = html.find("</script>", tag.end);
            if (close != std::string::npos) {
                end = close + std::strlen("</script>");
            }
            swaps.push_back({ tag.begin, end, "<script>\n" + break_close(file->text, "</script") + "\n</script>" });
        } else {
            swaps.push_back({ tag.begin, tag.end, "<style>\n" + break_close(file->text, "</style") + "\n</style>" });
        }
    }
    std::sort(swaps.begin(), swaps.end(), [](const Swap & a, const Swap & b) { return a.begin > b.begin; });
    std::string out = html;
    for (const Swap & swap : swaps) {
        if (swap.begin > out.size() || swap.end > out.size() || swap.end < swap.begin) {
            continue;
        }
        out.replace(swap.begin, swap.end - swap.begin, swap.text);
    }
    if (out.size() > 64000) {
        return {};
    }
    return out;
}

std::string build_note(bool ok, const std::string & kind, const std::string & main_file, bool preview) {
    if (!ok) {
        return "Fix every error, then call project_build again. This console cannot start Node or Python.";
    }
    if (preview) {
        return "Built in this process. The web page is open in the preview. This console cannot start Node or Python. On a computer, open index.html in a browser.";
    }
    std::string how = "On a computer, run python main.py in that folder.";
    if (kind == "node-server") {
        how = "On a computer, run node " + main_file + " in that folder.";
    } else if (kind == "static-page") {
        how = "On a computer, open index.html in a browser.";
    }
    return "Checked in this process. This console cannot start Node or Python. " + how;
}

Review gather(const std::string & root, const std::string & name) {
    Review review;
    if (!name_ok(name)) {
        review.fail = "That project name is not allowed";
        return review;
    }
    const std::string dir = trim_slash(root) + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        review.fail = "That project does not exist";
        return review;
    }
    review.kind = read_kind(dir);
    if (review.kind.empty()) {
        review.fail = "That project has no kind";
        return review;
    }
    const std::vector<Item> items = list_items(dir);
    if ((int) items.size() > kMaxFiles) {
        push_error(review.errors, "This project has more than 40 files");
    }
    for (const auto & item : items) {
        review.files.push_back(item.rel);
        std::string error;
        const std::string text = read_text(item.full, &error);
        if (!error.empty()) {
            push_error(review.errors, item.rel + ": " + error);
            continue;
        }
        ReviewedFile one;
        one.rel = item.rel;
        one.text = text;
        if (is_js(item.rel) || ends_with(item.rel, ".css")) {
            one.scan = scan_source(text, false);
        } else if (ends_with(item.rel, ".py")) {
            one.scan = scan_source(text, true);
        } else if (ends_with(item.rel, ".json")) {
            const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_discarded()) {
                push_error(review.errors, item.rel + " is not valid JSON");
            }
        }
        if (!one.scan.error.empty()) {
            push_error(review.errors, item.rel + " " + one.scan.error);
        }
        review.scanned.push_back(std::move(one));
    }
    if (review.kind == "node-server") {
        const ReviewedFile * package = review.find("package.json");
        if (!package) {
            push_error(review.errors, "node-server needs package.json");
        } else {
            const nlohmann::json parsed = nlohmann::json::parse(package->text, nullptr, false);
            if (!parsed.is_object() || !parsed["main"].is_string()) {
                push_error(review.errors, "package.json needs a main file");
            } else {
                review.main_file = parsed["main"].get<std::string>();
                if (review.main_file.rfind("./", 0) == 0) {
                    review.main_file = review.main_file.substr(2);
                }
                if (!path_ok(review.main_file)) {
                    push_error(review.errors, "package.json main is not in this project");
                } else {
                    const ReviewedFile * main = review.find(review.main_file);
                    if (!main) {
                        push_error(review.errors, "package.json main is missing");
                    } else if (blank(main->text)) {
                        push_error(review.errors, review.main_file + " is empty");
                    } else if (main->scan.error.empty() && !main->scan.listen && !main->scan.create_server) {
                        push_error(review.errors, review.main_file + " does not listen");
                    }
                }
            }
        }
    } else if (review.kind == "static-page") {
        const ReviewedFile * page = review.find("index.html");
        if (!page) {
            push_error(review.errors, "static-page needs index.html");
        } else if (blank(page->text) || page->text.find('<') == std::string::npos) {
            push_error(review.errors, "index.html needs a tag");
        }
    } else {
        const ReviewedFile * main = review.find("main.py");
        if (!main) {
            push_error(review.errors, "python needs main.py");
        } else if (blank(main->text)) {
            push_error(review.errors, "main.py is empty");
        }
    }
    check_local_refs(review);
    return review;
}

std::string do_review(const std::string & root, const std::string & name) {
    const Review review = gather(root, name);
    if (!review.fail.empty()) {
        return fail(review.fail);
    }
    const bool ok = review.errors.empty();
    return nlohmann::json{
        { "ok", ok },
        { "name", name },
        { "kind", review.kind },
        { "files", review.files },
        { "errors", review.errors },
        { "note", review_note(ok, name, review.kind, review.main_file) },
    }.dump();
}

std::string do_build(const std::string & root, const std::string & name) {
    const Review review = gather(root, name);
    if (!review.fail.empty()) {
        return fail(review.fail);
    }
    const bool ok = review.errors.empty();
    std::string page;
    if (ok && review.kind == "static-page") {
        page = preview_page(review);
    }
    nlohmann::json out = {
        { "ok", ok },
        { "name", name },
        { "kind", review.kind },
        { "files", review.files },
        { "errors", review.errors },
        { "note", build_note(ok, review.kind, review.main_file, !page.empty()) },
    };
    if (!page.empty()) {
        out["page"] = page;
    }
    return out.dump();
}

std::string do_remove(const std::string & root, const std::string & name, const std::string & rel) {
    if (!name_ok(name)) {
        return fail("That project name is not allowed");
    }
    if (!path_ok(rel)) {
        return fail("That path is not allowed");
    }
    const std::string dir = trim_slash(root) + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return fail("That project does not exist");
    }
    const std::string full = full_path(dir, rel);
    if (full.empty()) {
        return fail("That path is not allowed");
    }
    if (std::filesystem::is_symlink(full, ec)) {
        return fail("That path is not allowed");
    }
    if (std::filesystem::is_directory(full, ec)) {
        return fail("That path is a folder");
    }
    if (!std::filesystem::is_regular_file(full, ec)) {
        return fail("That file does not exist");
    }
    if (!std::filesystem::remove(full, ec) || ec) {
        return fail("Could not delete the file");
    }
    return nlohmann::json{ { "ok", true }, { "name", name }, { "path", rel } }.dump();
}

std::string field(const nlohmann::json & body, const char * key) {
    if (!body.contains(key) || !body[key].is_string()) {
        return {};
    }
    return body[key].get<std::string>();
}

}  // namespace

std::string project_call(const std::string & root, const std::string & body) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (root.empty() || root.find("..") != std::string::npos) {
        return fail("The projects folder is not available");
    }
    const nlohmann::json json = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    if (!json.is_object()) {
        return fail("The project request is not valid");
    }
    const std::string tool = field(json, "tool");
    const std::string name = field(json, "name");
    if (tool == "list") {
        return do_list(root, name);
    }
    if (tool == "create") {
        return do_create(root, name, field(json, "kind"));
    }
    if (tool == "read") {
        return do_read(root, name, field(json, "path"));
    }
    if (tool == "write") {
        if (!json.contains("text") || !json["text"].is_string()) {
            return fail("A write needs text");
        }
        return do_write(root, name, field(json, "path"), json["text"].get<std::string>());
    }
    if (tool == "review") {
        return do_review(root, name);
    }
    if (tool == "build") {
        return do_build(root, name);
    }
    if (tool == "remove") {
        return do_remove(root, name, field(json, "path"));
    }
    return fail("Unknown project tool");
}

}  // namespace ps5lm
