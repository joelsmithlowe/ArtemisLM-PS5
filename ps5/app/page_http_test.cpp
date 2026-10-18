// Host check for the idle page routes and the address shown on the television.
//   c++ -std=c++17 ps5/app/page_http_test.cpp -o /tmp/page_http_test
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "page_http.hpp"

#include <cstdio>
#include <string>

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

}  // namespace

int main() {
    using ps5lm::IdleAnswer;
    expect(ps5lm::idle_route("/?x=1") == "/", "query");
    expect(ps5lm::idle_route("/v1/") == "/v1", "trailing slash");
    expect(ps5lm::idle_route("") == "/", "empty path");
    expect(ps5lm::idle_answer("GET", "/") == IdleAnswer::page, "page");
    expect(ps5lm::idle_answer("HEAD", "/index.html") == IdleAnswer::page, "index");
    expect(ps5lm::idle_answer("GET", "/v1") == IdleAnswer::redirect, "api root redirects");
    expect(ps5lm::idle_answer("GET", "/v1/models") == IdleAnswer::models, "models");
    expect(ps5lm::idle_answer("GET", "/health") == IdleAnswer::health, "health");
    expect(ps5lm::idle_answer("GET", "/artemis") == IdleAnswer::info, "info");
    expect(ps5lm::idle_answer("POST", "/") == IdleAnswer::other, "post is not the page");
    expect(ps5lm::idle_answer("GET", "/v1/chat/completions") == IdleAnswer::other, "chat stays an api");
    expect(ps5lm::idle_answer("GET", "/favicon.ico") == IdleAnswer::other, "favicon");

    expect(ps5lm::lan_ipv4("192.168.1.19"), "lan");
    expect(ps5lm::lan_ipv4("10.0.0.2"), "ten");
    expect(!ps5lm::lan_ipv4(""), "empty");
    expect(!ps5lm::lan_ipv4("0.0.0.0"), "any");
    expect(!ps5lm::lan_ipv4("127.0.0.1"), "loopback");
    expect(!ps5lm::lan_ipv4("<console IP>"), "placeholder");
    expect(!ps5lm::lan_ipv4("192.168.1.19:8081"), "port is not an address");
    expect(!ps5lm::lan_ipv4("256.1.1.1"), "octet");
    expect(!ps5lm::lan_ipv4("192.168.1"), "short");

    if (fails) {
        std::fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    std::printf("page http ok\n");
    return 0;
}
