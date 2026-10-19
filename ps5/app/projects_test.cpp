// Host check for the projects folder and the in-process review:
//   c++ -std=c++20 -O1 -I ps5/app -I third_party/llama.cpp/vendor ps5/app/projects_test.cpp ps5/app/projects.cpp -o build/projects_test
// The test uses a temporary directory. It does not touch /data.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "projects.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}

nlohmann::json call(const std::string & root, const nlohmann::json & body) {
    const std::string raw = ps5lm::project_call(root, body.dump());
    const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) {
        std::printf("FAIL bad json for %s\n", body.value("tool", std::string("?")).c_str());
        ++fails;
        return nlohmann::json::object();
    }
    return parsed;
}

bool has_error(const nlohmann::json & j) { return j.contains("error") && j["error"].is_string(); }

void test_rejects(const std::string & root) {
    expect(has_error(call(root, { { "tool", "create" }, { "name", "../x" }, { "kind", "python" } })), "reject dotdot name");
    expect(has_error(call(root, { { "tool", "create" }, { "name", "has space" }, { "kind", "python" } })), "reject space");
    expect(has_error(call(root, { { "tool", "create" }, { "name", ".hidden" }, { "kind", "python" } })), "reject dot name");
    expect(has_error(call(root, { { "tool", "create" }, { "name", "" }, { "kind", "python" } })), "reject empty name");
    expect(has_error(call(root, { { "tool", "create" }, { "name", "-bad" }, { "kind", "python" } })), "reject leading dash");
    expect(has_error(call(root, { { "tool", "create" }, { "name", "thisnameiswaytoolongforthelimitxx" }, { "kind", "python" } })), "reject long name");
    expect(has_error(call(root, { { "tool", "create" }, { "name", "ok" }, { "kind", "ruby" } })), "reject kind");
    expect(has_error(call(root, { { "tool", "nope" } })), "reject tool");

    const auto made = call(root, { { "tool", "create" }, { "name", "gate" }, { "kind", "python" } });
    expect(made.value("ok", false), "gate create");
    const char * bad_paths[] = { "../x.py", "/etc/passwd", "foo.exe", ".artemis.json", "a/b/c/d/e.py", "foo/../../x.py", "C:foo.py", "dir/.hidden.py", "noext", "sub/" };
    for (const char * path : bad_paths) {
        const auto wrote = call(root, { { "tool", "write" }, { "name", "gate" }, { "path", path }, { "text", "print(1)\n" } });
        expect(has_error(wrote), path);
    }
    expect(has_error(call(root, { { "tool", "write" }, { "name", "missing" }, { "path", "a.py" }, { "text", "print(1)\n" } })), "write missing project");
    expect(has_error(call(root, { { "tool", "write" }, { "name", "gate" }, { "path", "a.txt" } })), "write without text");
    const auto nul = call(root, nlohmann::json::parse(std::string("{\"tool\":\"write\",\"name\":\"gate\",\"path\":\"a.txt\",\"text\":\"a\\u0000b\"}")));
    expect(has_error(nul), "nul rejected");
}

void test_kinds(const std::string & root) {
    const auto node = call(root, { { "tool", "create" }, { "name", "hello" }, { "kind", "node-server" } });
    expect(node.value("ok", false) && node.value("kind", std::string()) == "node-server", "create node");
    expect(has_error(call(root, { { "tool", "create" }, { "name", "hello" }, { "kind", "node-server" } })), "create twice");
    const auto node_review = call(root, { { "tool", "review" }, { "name", "hello" } });
    const std::string node_note = node_review.value("note", std::string());
    expect(node_review.value("ok", false), "node review");
    expect(node_note.find("Stored and reviewed") != std::string::npos, "node stored");
    expect(node_note.find("/data/ArtemisLM/projects/hello") != std::string::npos, "node path");
    expect(node_note.find("cannot start Node or Python") != std::string::npos, "node honest");
    expect(node_note.find("On a computer, run node server.js") != std::string::npos, "node command");
    expect(node_note.find("server is running") == std::string::npos, "node not running");

    const auto page = call(root, { { "tool", "create" }, { "name", "site" }, { "kind", "static-page" } });
    expect(page.value("ok", false), "create page");
    const auto page_review = call(root, { { "tool", "review" }, { "name", "site" } });
    const std::string page_note = page_review.value("note", std::string());
    expect(page_review.value("ok", false), "page review");
    expect(page_note.find("open index.html") != std::string::npos, "page command");
    expect(page_note.find("cannot start Node or Python") != std::string::npos, "page honest");

    const auto py = call(root, { { "tool", "create" }, { "name", "tool" }, { "kind", "python" } });
    expect(py.value("ok", false), "create python");
    const auto py_review = call(root, { { "tool", "review" }, { "name", "tool" } });
    const std::string py_note = py_review.value("note", std::string());
    expect(py_review.value("ok", false), "python review");
    expect(py_note.find("run python main.py") != std::string::npos, "python command");
    expect(py_note.find("cannot start Node or Python") != std::string::npos, "python honest");
}

void test_review_errors(const std::string & root) {
    expect(call(root, { { "tool", "create" }, { "name", "broken" }, { "kind", "node-server" } }).value("ok", false), "broken create");
    const char * unclosed =
        "var http = require(\"http\");\n"
        "http.createServer(function (req, res) {\n"
        "  res.end(\"ok);\n"
        "});\n"
        "server.listen(8080);\n";
    expect(call(root, { { "tool", "write" }, { "name", "broken" }, { "path", "server.js" }, { "text", unclosed } }).value("ok", false), "write unclosed");
    const auto reviewed = call(root, { { "tool", "review" }, { "name", "broken" } });
    expect(!reviewed.value("ok", true), "unclosed fails review");
    const std::string first = reviewed["errors"].is_array() && !reviewed["errors"].empty() ? reviewed["errors"][0].get<std::string>() : std::string();
    expect(first.find("server.js line 3") != std::string::npos, "unclosed line");

    const char * comment_only =
        "// listen\n"
        "// createServer\n"
        "var x = 1;\n";
    expect(call(root, { { "tool", "write" }, { "name", "broken" }, { "path", "server.js" }, { "text", comment_only } }).value("ok", false), "write comment");
    const auto commented = call(root, { { "tool", "review" }, { "name", "broken" } });
    expect(!commented.value("ok", true), "comment listen fails");
    bool says_listen = false;
    if (commented["errors"].is_array()) {
        for (const auto & err : commented["errors"]) {
            if (err.is_string() && err.get<std::string>().find("does not listen") != std::string::npos) {
                says_listen = true;
            }
        }
    }
    expect(says_listen, "does not listen");

    const char * in_string = "var s = \"listen createServer\";\nvar x = 1;\n";
    call(root, { { "tool", "write" }, { "name", "broken" }, { "path", "server.js" }, { "text", in_string } });
    expect(!call(root, { { "tool", "review" }, { "name", "broken" } }).value("ok", true), "string listen fails");

    const char * in_comment_brace =
        "/* { */\n"
        "var http = require(\"http\");\n"
        "http.createServer(function () {});\n"
        "server.listen(8080);\n";
    call(root, { { "tool", "write" }, { "name", "broken" }, { "path", "server.js" }, { "text", in_comment_brace } });
    expect(call(root, { { "tool", "review" }, { "name", "broken" } }).value("ok", false), "comment brace ignored");

    call(root, { { "tool", "write" }, { "name", "broken" }, { "path", "package.json" }, { "text", "{" } });
    const auto bad_json = call(root, { { "tool", "review" }, { "name", "broken" } });
    expect(!bad_json.value("ok", true), "bad json fails");

    expect(call(root, { { "tool", "create" }, { "name", "page2" }, { "kind", "static-page" } }).value("ok", false), "page2");
    call(root, { { "tool", "write" }, { "name", "page2" }, { "path", "index.html" }, { "text", "hello" } });
    expect(call(root, { { "tool", "review" }, { "name", "page2" } }).value("ok", true) == false, "page needs a tag");

    expect(call(root, { { "tool", "create" }, { "name", "py2" }, { "kind", "python" } }).value("ok", false), "py2");
    call(root, { { "tool", "write" }, { "name", "py2" }, { "path", "main.py" }, { "text", "print(\"hi)\n" } });
    expect(!call(root, { { "tool", "review" }, { "name", "py2" } }).value("ok", true), "python string fails");
}

void test_round_trip(const std::string & root) {
    expect(call(root, { { "tool", "create" }, { "name", "edit" }, { "kind", "python" } }).value("ok", false), "edit create");
    const auto first = call(root, { { "tool", "write" }, { "name", "edit" }, { "path", "note.txt" }, { "text", "one" } });
    expect(first.value("bytes", 0) == 3, "first write");
    expect(call(root, { { "tool", "write" }, { "name", "edit" }, { "path", "note.txt" }, { "text", "two" } }).value("ok", false), "overwrite");
    const auto read = call(root, { { "tool", "read" }, { "name", "edit" }, { "path", "note.txt" } });
    expect(read.value("text", std::string()) == "two", "read back");
    expect(call(root, { { "tool", "write" }, { "name", "edit" }, { "path", "a/b/c/d.txt" }, { "text", "deep" } }).value("ok", false), "four parts");
    const auto listed = call(root, { { "tool", "list" }, { "name", "edit" } });
    expect(listed.value("kind", std::string()) == "python", "list kind");
    int files = listed["files"].is_array() ? (int) listed["files"].size() : 0;
    expect(files == 3, "list files");
    const auto all = call(root, { { "tool", "list" } });
    expect(all["projects"].is_array() && all["projects"].size() >= 2, "list projects");

    expect(call(root, { { "tool", "create" }, { "name", "cap" }, { "kind", "python" } }).value("ok", false), "cap create");
    expect(call(root, { { "tool", "write" }, { "name", "cap" }, { "path", "a.txt" }, { "text", std::string(12000, 'a') } }).value("ok", false), "12000 ok");
    expect(has_error(call(root, { { "tool", "write" }, { "name", "cap" }, { "path", "b.txt" }, { "text", std::string(12001, 'b') } })), "12001 rejected");

    expect(call(root, { { "tool", "create" }, { "name", "many" }, { "kind", "python" } }).value("ok", false), "many create");
    bool filled = true;
    for (int i = 0; i < 39; i++) {
        char path[16];
        std::snprintf(path, sizeof(path), "n%02d.txt", i);
        if (!call(root, { { "tool", "write" }, { "name", "many" }, { "path", path }, { "text", "x" } }).value("ok", false)) {
            filled = false;
        }
    }
    expect(filled, "fill 40");
    expect(has_error(call(root, { { "tool", "write" }, { "name", "many" }, { "path", "over.txt" }, { "text", "x" } })), "file 41");
}

void test_build(const std::string & root) {
    const auto page = call(root, { { "tool", "build" }, { "name", "site" } });
    expect(page.value("ok", false), "build page");
    const std::string html = page.value("page", std::string());
    expect(html.find("font-family: sans-serif") != std::string::npos, "inlined css");
    expect(html.find("document.title") != std::string::npos, "inlined script");
    expect(html.find("href=\"style.css\"") == std::string::npos, "css link replaced");
    const std::string note = page.value("note", std::string());
    expect(note.find("preview") != std::string::npos, "preview note");
    expect(note.find("cannot start Node or Python") != std::string::npos, "build honest");

    const auto node = call(root, { { "tool", "build" }, { "name", "hello" } });
    expect(node.value("ok", false), "build node");
    expect(node.value("page", std::string()).empty(), "node has no page");
    const std::string node_note = node.value("note", std::string());
    expect(node_note.find("cannot start Node or Python") != std::string::npos, "node build honest");
    expect(node_note.find("run node server.js") != std::string::npos, "node build command");

    expect(call(root, { { "tool", "create" }, { "name", "gap" }, { "kind", "static-page" } }).value("ok", false), "gap create");
    const char * gap_html = "<!DOCTYPE html>\n<html><body>\n<link rel=\"stylesheet\" href=\"gone.css\">\n</body></html>\n";
    expect(call(root, { { "tool", "write" }, { "name", "gap" }, { "path", "index.html" }, { "text", gap_html } }).value("ok", false), "gap write");
    const auto gap = call(root, { { "tool", "review" }, { "name", "gap" } });
    expect(!gap.value("ok", true), "missing css fails");
    bool says = false;
    if (gap["errors"].is_array()) {
        for (const auto & err : gap["errors"]) {
            if (err.is_string() && err.get<std::string>().find("links to missing gone.css") != std::string::npos) {
                says = true;
            }
        }
    }
    expect(says, "missing css message");
    const auto gap_build = call(root, { { "tool", "build" }, { "name", "gap" } });
    expect(!gap_build.value("ok", true), "build missing css");
    expect(gap_build.value("page", std::string()).empty(), "no preview when broken");

    expect(call(root, { { "tool", "create" }, { "name", "req" }, { "kind", "node-server" } }).value("ok", false), "req create");
    const char * req_js =
        "var http = require(\"http\");\n"
        "var extra = require(\"./missing.js\");\n"
        "var server = http.createServer(function (req, res) { res.end(\"ok\\n\"); });\n"
        "server.listen(8080);\n";
    expect(call(root, { { "tool", "write" }, { "name", "req" }, { "path", "server.js" }, { "text", req_js } }).value("ok", false), "req write");
    const auto req = call(root, { { "tool", "review" }, { "name", "req" } });
    expect(!req.value("ok", true), "missing require fails");
    bool needs = false;
    if (req["errors"].is_array()) {
        for (const auto & err : req["errors"]) {
            if (err.is_string() && err.get<std::string>().find("needs missing.js") != std::string::npos) {
                needs = true;
            }
        }
    }
    expect(needs, "missing require message");
}

void test_remove(const std::string & root) {
    expect(call(root, { { "tool", "create" }, { "name", "rm" }, { "kind", "python" } }).value("ok", false), "rm create");
    expect(call(root, { { "tool", "write" }, { "name", "rm" }, { "path", "note.txt" }, { "text", "gone" } }).value("ok", false), "rm write");
    expect(call(root, { { "tool", "remove" }, { "name", "rm" }, { "path", "note.txt" } }).value("ok", false), "rm file");
    expect(has_error(call(root, { { "tool", "read" }, { "name", "rm" }, { "path", "note.txt" } })), "rm gone");
    expect(has_error(call(root, { { "tool", "remove" }, { "name", "rm" }, { "path", ".artemis.json" } })), "rm marker");
    expect(has_error(call(root, { { "tool", "remove" }, { "name", "rm" }, { "path", "../x.py" } })), "rm escape");
    const auto listed = call(root, { { "tool", "list" }, { "name", "rm" } });
    expect(listed["files"].is_array() && !listed["files"].empty(), "rm project remains");
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/artemis-projects-XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::printf("projects_test: mkdtemp failed\n");
        return 1;
    }
    const std::string root = tmpl;
    test_rejects(root);
    test_kinds(root);
    test_review_errors(root);
    test_round_trip(root);
    test_build(root);
    test_remove(root);
    std::filesystem::remove_all(root);
    if (fails) {
        std::printf("projects_test: %d failed\n", fails);
        return 1;
    }
    std::printf("projects_test: ok\n");
    return 0;
}
