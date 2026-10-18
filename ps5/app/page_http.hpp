// Which idle request on port 47281 is the phone page, and which address is safe to print.
// The page is GET / . GET /v1 is only the API prefix, so a browser that opens it is sent to /.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace ps5lm {

enum class IdleAnswer { page, redirect, models, health, info, other };

inline std::string idle_route(const std::string & path) {
    const size_t q = path.find('?');
    std::string route = q == std::string::npos ? path : path.substr(0, q);
    if (route.size() > 1 && route.back() == '/') {
        route.pop_back();
    }
    if (route.empty()) {
        route = "/";
    }
    return route;
}

inline IdleAnswer idle_answer(const std::string & method, const std::string & route) {
    if (method != "GET" && method != "HEAD") {
        return IdleAnswer::other;
    }
    if (route == "/" || route == "/index.html") {
        return IdleAnswer::page;
    }
    if (route == "/v1") {
        return IdleAnswer::redirect;
    }
    if (route == "/v1/models") {
        return IdleAnswer::models;
    }
    if (route == "/health") {
        return IdleAnswer::health;
    }
    if (route == "/artemis") {
        return IdleAnswer::info;
    }
    return IdleAnswer::other;
}

// A dotted IPv4 another device can open. 0.0.0.0 and loopback are not that address.
inline bool lan_ipv4(const std::string & text) {
    if (text == "0.0.0.0" || text == "127.0.0.1") {
        return false;
    }
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return false;
        }
        int value = 0;
        int digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            if (value > 255 || ++digits > 3) {
                return false;
            }
            ++i;
        }
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') {
                return false;
            }
            ++i;
        }
    }
    return i == text.size();
}

}  // namespace ps5lm
