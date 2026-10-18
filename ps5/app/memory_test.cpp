// Host check for hashed-word notes:
//   c++ -std=c++20 -O1 -I ps5/app -I third_party/llama.cpp/vendor ps5/app/memory_test.cpp ps5/app/memory.cpp -o build/memory_test
// The test uses a temporary directory. It does not touch /data.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    const std::string raw = ps5lm::memory_call(root, body.dump());
    const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) {
        std::printf("FAIL bad json: %s\n", raw.c_str());
        ++fails;
        return nlohmann::json::object();
    }
    return parsed;
}

bool has_error(const nlohmann::json & j) { return j.contains("error") && j["error"].is_string(); }

}  // namespace

int main() {
    char tmpl[] = "/tmp/artemis-memory-XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::printf("memory_test: mkdtemp failed\n");
        return 1;
    }
    const std::string root = tmpl;
    const std::string note = "The router password is on the sticker under the box";
    const auto saved = call(root, { { "tool", "save" }, { "text", note } });
    expect(saved.value("ok", false) && saved.value("text", std::string()) == note, "save");
    expect(saved.value("id", std::string()).size() == 8, "id length");
    const auto again = call(root, { { "tool", "save" }, { "text", note } });
    expect(again.value("id", std::string()) == saved.value("id", std::string()), "duplicate id");
    const auto listed = call(root, { { "tool", "list" } });
    expect(listed["notes"].is_array() && listed["notes"].size() == 1, "one note");

    const auto hit = call(root, { { "tool", "find" }, { "text", "where is the router password" } });
    const bool found = hit["hits"].is_array() && hit["hits"].size() == 1 && hit["hits"][0].value("text", std::string()) == note;
    if (!found) {
        std::printf("FAIL find router: %s\n", hit.dump().c_str());
        ++fails;
    }
    expect(hit.dump().find("\"v\"") == std::string::npos, "find hides vector");

    std::ifstream in(root + "/notes.json");
    const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(file.find("\"v\"") != std::string::npos, "stored vector");

    const auto math = call(root, { { "tool", "find" }, { "text", "what is 17 times 23" } });
    expect(math["hits"].is_array() && math["hits"].empty(), "math misses");
    const auto weather = call(root, { { "tool", "find" }, { "text", "what is the weather today" } });
    expect(weather["hits"].is_array() && weather["hits"].empty(), "weather misses");

    expect(has_error(call(root, { { "tool", "forget" }, { "text", "what is 17 times 23" } })), "forget math");
    const auto forgot = call(root, { { "tool", "forget" }, { "text", "router password sticker" } });
    if (!forgot.value("ok", false)) {
        std::printf("FAIL forget close: %s\n", forgot.dump().c_str());
        ++fails;
    }
    const auto after = call(root, { { "tool", "list" } });
    expect(after["notes"].is_array() && after["notes"].empty(), "forgotten");

    expect(has_error(call(root, { { "tool", "save" }, { "text", "" } })), "empty");
    expect(has_error(call(root, { { "tool", "save" }, { "text", std::string(401, 'a') } })), "too long");
    expect(has_error(call(root, { { "tool", "nope" } })), "tool");

    bool filled = true;
    for (int i = 0; i < 81; ++i) {
        const auto one = call(root, { { "tool", "save" }, { "text", "alpha item " + std::to_string(i) + " kept" } });
        if (!one.value("ok", false)) {
            filled = false;
        }
    }
    expect(filled, "fill");
    const auto capped = call(root, { { "tool", "list" } });
    expect(capped["notes"].is_array() && capped["notes"].size() == 80, "cap 80");
    bool dropped = true;
    if (capped["notes"].is_array()) {
        for (const auto & one : capped["notes"]) {
            if (one.value("text", std::string()) == "alpha item 0 kept") {
                dropped = false;
            }
        }
    }
    expect(dropped, "oldest dropped");

    std::filesystem::remove_all(root);
    if (fails) {
        std::printf("memory_test: %d failed\n", fails);
        return 1;
    }
    std::printf("memory_test: ok\n");
    return 0;
}
