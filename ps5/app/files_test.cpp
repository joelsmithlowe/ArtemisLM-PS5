// Host check for folder listing:
//   c++ -std=c++20 -O1 -I ps5/app -I third_party/llama.cpp/vendor ps5/app/files_test.cpp ps5/app/files.cpp -o build/files_test
// The test uses a temporary directory. It does not touch /data.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}

nlohmann::json call(const std::vector<ps5lm::FileRoot> & roots, const nlohmann::json & body) {
    const std::string raw = ps5lm::files_call(roots, body.dump());
    const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) {
        std::printf("FAIL bad json: %s\n", raw.c_str());
        ++fails;
        return nlohmann::json::object();
    }
    return parsed;
}

bool has_error(const nlohmann::json & j) { return j.contains("error") && j["error"].is_string(); }

void write_file(const std::string & path, const std::string & text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/artemis-files-XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::printf("files_test: mkdtemp failed\n");
        return 1;
    }
    const std::string root = tmpl;
    const auto catalog = ps5lm::files_roots();
    bool saw_models = false;
    bool saw_settings = false;
    for (const auto & one : catalog) {
        if (one.id == "models" && one.path == "/data/ArtemisLM/models") {
            saw_models = true;
        }
        if (one.path.find("settings.json") != std::string::npos || one.path.find("..") != std::string::npos) {
            saw_settings = true;
        }
    }
    expect(saw_models, "models root");
    expect(!saw_settings, "roots stay in their folders");

    write_file(root + "/hello.txt", "hello");
    write_file(root + "/model.gguf", "not-text");
    write_file(root + "/.hidden.txt", "secret");
    write_file(root + "/my notes/hello.txt", "spaced");
    write_file(root + "/a/b/c/d.txt", "deep");
    std::error_code ec;
    std::filesystem::create_symlink(root + "/hello.txt", root + "/link.txt", ec);
    expect(!ec, "symlink");

    const std::vector<ps5lm::FileRoot> roots = { { "scratch", root }, { "usb0", root + "/no-such-drive" } };
    const auto listed = call(roots, { { "tool", "list" }, { "root", "scratch" } });
    bool saw_hello = false;
    bool saw_gguf = false;
    bool saw_hidden = false;
    bool saw_link = false;
    bool saw_dir = false;
    if (listed["entries"].is_array()) {
        for (const auto & one : listed["entries"]) {
            const std::string name = one.value("name", std::string());
            if (name == "hello.txt") {
                saw_hello = true;
            }
            if (name == "model.gguf") {
                saw_gguf = true;
            }
            if (name == ".hidden.txt" || (!name.empty() && name[0] == '.')) {
                saw_hidden = true;
            }
            if (name == "link.txt") {
                saw_link = true;
            }
            if (name == "my notes" && one.value("kind", std::string()) == "dir") {
                saw_dir = true;
            }
        }
    } else {
        std::printf("FAIL list: %s\n", listed.dump().c_str());
        ++fails;
    }
    expect(saw_hello && saw_gguf && saw_dir, "listed names");
    expect(!saw_hidden && !saw_link, "skipped hidden and link");

    const auto read = call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "hello.txt" } });
    expect(read.value("text", std::string()) == "hello", "read hello");
    const auto spaced = call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "my notes/hello.txt" } });
    expect(spaced.value("text", std::string()) == "spaced", "read spaced");
    const auto deep = call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "a/b/c/d.txt" } });
    expect(deep.value("text", std::string()) == "deep", "read four");
    expect(has_error(call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "a/b/c/d/e.txt" } })), "five parts");
    expect(has_error(call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "model.gguf" } })), "gguf");
    expect(has_error(call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "../hello.txt" } })), "dotdot");
    expect(has_error(call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", ".hidden.txt" } })), "dotfile");
    expect(has_error(call(roots, { { "tool", "list" }, { "root", "nope" } })), "unknown root");

    const auto missing = call(roots, { { "tool", "list" }, { "root", "usb0" } });
    expect(missing.value("ok", false) && missing["entries"].is_array() && missing["entries"].empty(), "missing drive");
    expect(missing.value("note", std::string()).find("not") != std::string::npos, "missing note");

    write_file(root + "/big.txt", std::string(12001, 'z'));
    expect(has_error(call(roots, { { "tool", "read" }, { "root", "scratch" }, { "path", "big.txt" } })), "too big");

    std::filesystem::remove_all(root);
    if (fails) {
        std::printf("files_test: %d failed\n", fails);
        return 1;
    }
    std::printf("files_test: ok\n");
    return 0;
}
