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

std::string do_review(const std::string & root, const std::string & name) {
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
    const std::vector<Item> items = list_items(dir);
    std::vector<std::string> errors;
    if ((int) items.size() > kMaxFiles) {
        push_error(errors, "This project has more than 40 files");
    }
    nlohmann::json files = nlohmann::json::array();
    struct Scanned {
        std::string text;
        SourceScan scan;
    };
    std::vector<std::pair<std::string, Scanned>> scanned;
    for (const auto & item : items) {
        files.push_back(item.rel);
        std::string error;
        const std::string text = read_text(item.full, &error);
        if (!error.empty()) {
            push_error(errors, item.rel + ": " + error);
            continue;
        }
        Scanned one;
        one.text = text;
        if (is_js(item.rel) || ends_with(item.rel, ".css")) {
            one.scan = scan_source(text, false);
        } else if (ends_with(item.rel, ".py")) {
            one.scan = scan_source(text, true);
        } else if (ends_with(item.rel, ".json")) {
            const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_discarded()) {
                push_error(errors, item.rel + " is not valid JSON");
            }
        }
        if (!one.scan.error.empty()) {
            push_error(errors, item.rel + " " + one.scan.error);
        }
        scanned.push_back({ item.rel, std::move(one) });
    }
    std::string main_file = "server.js";
    auto find_scan = [&](const std::string & rel) -> const Scanned * {
        for (const auto & one : scanned) {
            if (one.first == rel) {
                return &one.second;
            }
        }
        return nullptr;
    };
    if (kind == "node-server") {
        const Scanned * package = find_scan("package.json");
        if (!package) {
            push_error(errors, "node-server needs package.json");
        } else {
            const nlohmann::json parsed = nlohmann::json::parse(package->text, nullptr, false);
            if (!parsed.is_object() || !parsed["main"].is_string()) {
                push_error(errors, "package.json needs a main file");
            } else {
                main_file = parsed["main"].get<std::string>();
                if (main_file.rfind("./", 0) == 0) {
                    main_file = main_file.substr(2);
                }
                if (!path_ok(main_file)) {
                    push_error(errors, "package.json main is not in this project");
                } else {
                    const Scanned * main = find_scan(main_file);
                    if (!main) {
                        push_error(errors, "package.json main is missing");
                    } else if (blank(main->text)) {
                        push_error(errors, main_file + " is empty");
                    } else if (main->scan.error.empty() && !main->scan.listen && !main->scan.create_server) {
                        push_error(errors, main_file + " does not listen");
                    }
                }
            }
        }
    } else if (kind == "static-page") {
        const Scanned * page = find_scan("index.html");
        if (!page) {
            push_error(errors, "static-page needs index.html");
        } else if (blank(page->text) || page->text.find('<') == std::string::npos) {
            push_error(errors, "index.html needs a tag");
        }
    } else {
        const Scanned * main = find_scan("main.py");
        if (!main) {
            push_error(errors, "python needs main.py");
        } else if (blank(main->text)) {
            push_error(errors, "main.py is empty");
        }
    }
    const bool ok = errors.empty();
    return nlohmann::json{
        { "ok", ok },
        { "name", name },
        { "kind", kind },
        { "files", std::move(files) },
        { "errors", errors },
        { "note", review_note(ok, name, kind, main_file) },
    }.dump();
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
    if (tool == "remove") {
        return do_remove(root, name, field(json, "path"));
    }
    return fail("Unknown project tool");
}

}  // namespace ps5lm
