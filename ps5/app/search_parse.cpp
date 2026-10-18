// SPDX-License-Identifier: GPL-3.0-or-later
#include "search_parse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps5lm {

std::string html_unescape(std::string text);

namespace {

bool is_unreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// The next JSON string at or after i, which must land on the opening quote.
// i moves past the closing quote. False when the quotes do not close.
bool read_json_string(const std::string & s, size_t & i, std::string * out) {
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    out->clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') {
            return true;
        }
        if (c != '\\' || i >= s.size()) {
            out->push_back(c);
            continue;
        }
        const char e = s[i++];
        switch (e) {
            case '"':
            case '\\':
            case '/': out->push_back(e); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u':
                if (i + 4 <= s.size()) {
                    int cp = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        const int h = hex_value(s[i + k]);
                        ok = ok && h >= 0;
                        cp = (cp << 4) | (h < 0 ? 0 : h);
                    }
                    i += 4;
                    if (ok && cp < 0x80) {
                        out->push_back((char) cp);
                    } else if (ok && cp < 0x800) {
                        out->push_back((char) (0xc0 | (cp >> 6)));
                        out->push_back((char) (0x80 | (cp & 0x3f)));
                    } else if (ok) {
                        out->push_back((char) (0xe0 | (cp >> 12)));
                        out->push_back((char) (0x80 | ((cp >> 6) & 0x3f)));
                        out->push_back((char) (0x80 | (cp & 0x3f)));
                    }
                }
                break;
            default: out->push_back(e); break;
        }
    }
    return false;
}

std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char) s[a])) {
        ++a;
    }
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char) s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string strip_tags(const std::string & html) {
    std::string out;
    out.reserve(html.size());
    for (size_t i = 0; i < html.size(); ++i) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string::npos) {
                break;
            }
            i = end;
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(html[i]);
    }
    return trim(html_unescape(out));
}

bool looks_like_url(const std::string & url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// The real target DuckDuckGo hides in its redirect links.
std::string unduck(std::string url) {
    const size_t at = url.find("uddg=");
    if (at == std::string::npos) {
        return url;
    }
    std::string enc = url.substr(at + 5);
    const size_t amp = enc.find('&');
    if (amp != std::string::npos) {
        enc.resize(amp);
    }
    const std::string dec = url_decode(enc);
    return looks_like_url(dec) ? dec : url;
}

std::string attr(const std::string & tag, const char * name) {
    const std::string key = std::string(name) + "=";
    const size_t at = tag.find(key);
    if (at == std::string::npos || at + key.size() >= tag.size()) {
        return {};
    }
    const char q = tag[at + key.size()];
    if (q != '"' && q != '\'') {
        return {};
    }
    const size_t end = tag.find(q, at + key.size() + 1);
    if (end == std::string::npos) {
        return {};
    }
    return tag.substr(at + key.size() + 1, end - (at + key.size() + 1));
}

bool tag_has(const std::string & tag, const char * needle) {
    return tag.find(needle) != std::string::npos;
}

void push_hit(std::vector<SearchHit> & out, SearchHit hit, int limit) {
    hit.title = trim(hit.title);
    hit.url = trim(hit.url);
    hit.snippet = trim(hit.snippet);
    if (hit.title.empty() || !looks_like_url(hit.url) || (int) out.size() >= limit) {
        return;
    }
    for (const auto & have : out) {
        if (have.url == hit.url) {
            return;
        }
    }
    if (hit.snippet.size() > 280) {
        hit.snippet.resize(277);
        hit.snippet += "...";
    }
    out.push_back(std::move(hit));
}

}  // namespace

std::string url_encode(const std::string & text) {
    static const char * hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if (is_unreserved(c)) {
            out.push_back((char) c);
        } else if (c == ' ') {
            out += "%20";
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string url_decode(const std::string & text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]), lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char) ((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

std::string html_unescape(std::string text) {
    const auto replace = [&](const char * from, const char * to) {
        const size_t n = std::char_traits<char>::length(from);
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
            text.replace(at, n, to);
            at += std::char_traits<char>::length(to);
        }
    };
    replace("&amp;", "&");
    replace("&lt;", "<");
    replace("&gt;", ">");
    replace("&quot;", "\"");
    replace("&#39;", "'");
    replace("&apos;", "'");
    replace("&nbsp;", " ");
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '&' && i + 3 < text.size() && text[i + 1] == '#') {
            int cp = 0;
            size_t j = i + 2;
            const bool hex = text[j] == 'x' || text[j] == 'X';
            if (hex) {
                ++j;
            }
            const size_t start = j;
            while (j < text.size() && j < start + 6) {
                const int h = hex_value(text[j]);
                if (!hex && (text[j] < '0' || text[j] > '9')) {
                    break;
                }
                if (hex && h < 0) {
                    break;
                }
                cp = hex ? (cp << 4) | h : cp * 10 + (text[j] - '0');
                ++j;
            }
            if (j > start && j < text.size() && text[j] == ';' && cp > 0 && cp < 0x80) {
                out.push_back((char) cp);
                i = j + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

std::vector<SearchHit> parse_duckduckgo(const std::string & html, int limit) {
    std::vector<SearchHit> out;
    if (limit < 1 || html.find("anomaly-modal") != std::string::npos) {
        return out;
    }
    for (size_t i = 0; i < html.size() && (int) out.size() < limit; ++i) {
        if (html[i] != '<') {
            continue;
        }
        const size_t end = html.find('>', i);
        if (end == std::string::npos) {
            break;
        }
        const std::string tag = html.substr(i, end - i);
        const bool link = tag_has(tag, "result-link") || tag_has(tag, "result__a");
        if (!link) {
            i = end;
            continue;
        }
        std::string href = html_unescape(attr(tag, "href"));
        href = unduck(href);
        const size_t close = html.find("</a>", end);
        const std::string title = close == std::string::npos ? std::string() : strip_tags(html.substr(end + 1, close - end - 1));
        std::string snippet;
        const size_t window = html.find("result-snippet", end);
        const size_t window2 = html.find("result__snippet", end);
        size_t snip = std::min(window, window2);
        if (snip != std::string::npos && snip < end + 2000) {
            const size_t snip_end = html.find('>', snip);
            const size_t snip_close = snip_end == std::string::npos ? std::string::npos : html.find('<', snip_end + 1);
            if (snip_end != std::string::npos && snip_close != std::string::npos) {
                snippet = strip_tags(html.substr(snip_end + 1, snip_close - snip_end - 1));
            }
        }
        push_hit(out, { title, href, snippet }, limit);
        i = end;
    }
    return out;
}

std::vector<SearchHit> parse_wikipedia_opensearch(const std::string & json, int limit) {
    std::vector<SearchHit> out;
    std::vector<std::string> strings;
    for (size_t i = 0; i < json.size();) {
        const size_t q = json.find('"', i);
        if (q == std::string::npos) {
            break;
        }
        i = q;
        std::string s;
        if (!read_json_string(json, i, &s)) {
            break;
        }
        strings.push_back(std::move(s));
    }
    // query, then titles, descriptions and urls, the same count of each.
    if (strings.size() < 4 || (strings.size() - 1) % 3 != 0) {
        return out;
    }
    const size_t n = (strings.size() - 1) / 3;
    for (size_t i = 0; i < n && (int) out.size() < limit; ++i) {
        push_hit(out, { strings[1 + i], strings[1 + 2 * n + i], strings[1 + n + i] }, limit);
    }
    return out;
}

std::vector<SearchHit> parse_searxng(const std::string & json, int limit) {
    std::vector<SearchHit> out;
    size_t i = 0;
    while (i < json.size() && (int) out.size() < limit) {
        const size_t at = json.find("\"url\"", i);
        if (at == std::string::npos) {
            break;
        }
        const size_t colon = json.find(':', at + 5);
        if (colon == std::string::npos) {
            break;
        }
        size_t q = json.find('"', colon + 1);
        if (q == std::string::npos) {
            break;
        }
        i = q;
        std::string url;
        if (!read_json_string(json, i, &url)) {
            break;
        }
        const size_t obj_begin = json.rfind('{', at);
        const size_t obj_end = json.find('}', i);
        if (obj_begin == std::string::npos || obj_end == std::string::npos || obj_end < obj_begin) {
            continue;
        }
        const std::string around = json.substr(obj_begin, obj_end - obj_begin);
        const auto field = [&](const char * key) {
            const size_t k = around.find(std::string("\"") + key + "\"");
            if (k == std::string::npos) {
                return std::string();
            }
            const size_t c = around.find(':', k);
            const size_t qq = c == std::string::npos ? std::string::npos : around.find('"', c + 1);
            if (qq == std::string::npos) {
                return std::string();
            }
            size_t p = qq;
            std::string value;
            read_json_string(around, p, &value);
            return value;
        };
        push_hit(out, { field("title"), url, field("content") }, limit);
    }
    return out;
}

std::string html_to_text(const std::string & html, size_t max_chars) {
    std::string out;
    out.reserve(std::min(html.size(), max_chars));
    bool space = true;
    for (size_t i = 0; i < html.size() && out.size() < max_chars;) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string::npos) {
                break;
            }
            const std::string tag = html.substr(i + 1, std::min(end - i - 1, (size_t) 16));
            const auto skip_until = [&](const char * close) {
                const size_t at = html.find(close, end);
                i = at == std::string::npos ? html.size() : at + std::char_traits<char>::length(close);
            };
            if (tag.rfind("script", 0) == 0 || tag.rfind("SCRIPT", 0) == 0) {
                skip_until("</script>");
                if (i == html.size()) {
                    skip_until("</SCRIPT>");
                }
                continue;
            }
            if (tag.rfind("style", 0) == 0 || tag.rfind("STYLE", 0) == 0) {
                skip_until("</style>");
                continue;
            }
            i = end + 1;
            if (!space) {
                out.push_back(' ');
                space = true;
            }
            continue;
        }
        const char c = html[i++];
        if (std::isspace((unsigned char) c)) {
            if (!space) {
                out.push_back(' ');
                space = true;
            }
            continue;
        }
        out.push_back(c);
        space = false;
    }
    return trim(html_unescape(out.substr(0, max_chars)));
}

std::string format_hits(const std::vector<SearchHit> & hits) {
    std::string out;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i) {
            out += "\n\n";
        }
        out += std::to_string(i + 1);
        out += ". ";
        out += hits[i].title;
        out += "\n";
        out += hits[i].url;
        if (!hits[i].snippet.empty()) {
            out += "\n";
            out += hits[i].snippet;
        }
    }
    return out;
}

namespace {

std::string math_source(std::string text) {
    for (char & c : text) {
        c = (char) std::tolower((unsigned char) c);
    }
    const auto swap = [&](const char * from, const char * to) {
        const size_t n = std::strlen(from);
        for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; ) {
            text.replace(at, n, to);
            at += std::strlen(to);
        }
    };
    swap(" divided by ", " / ");
    swap(" times ", " * ");
    swap(" plus ", " + ");
    swap(" minus ", " - ");
    swap(" over ", " / ");
    const char * lead[] = { "what is ", "whats ", "what's ", "calculate ", "compute ", "how much is " };
    for (const char * p : lead) {
        if (text.rfind(p, 0) == 0) {
            text.erase(0, std::strlen(p));
        }
    }
    while (!text.empty() && (text.back() == '?' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    while (!text.empty() && text.front() == ' ') {
        text.erase(text.begin());
    }
    if (text.empty() || text.size() > 80) {
        return {};
    }
    for (unsigned char c : text) {
        if (std::isdigit(c) || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' || c == ' ') {
            continue;
        }
        return {};
    }
    return text;
}

struct Parser {
    const std::string & s;
    size_t i = 0;
    bool bad = false;
    void skip() {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
    }
    double expr();
    double term();
    double factor() {
        skip();
        if (bad || i >= s.size()) {
            bad = true;
            return 0;
        }
        if (s[i] == '(') {
            ++i;
            const double v = expr();
            skip();
            if (i >= s.size() || s[i] != ')') {
                bad = true;
                return 0;
            }
            ++i;
            return v;
        }
        char * end = nullptr;
        const double v = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) {
            bad = true;
            return 0;
        }
        i = (size_t) (end - s.c_str());
        return v;
    }
};

double Parser::term() {
    double v = factor();
    for (;;) {
        skip();
        if (i >= s.size() || (s[i] != '*' && s[i] != '/')) {
            return v;
        }
        const char op = s[i++];
        const double r = factor();
        if (op == '*') {
            v *= r;
        } else if (r == 0.0) {
            bad = true;
            return 0;
        } else {
            v /= r;
        }
    }
}

double Parser::expr() {
    double v = term();
    for (;;) {
        skip();
        if (i >= s.size() || (s[i] != '+' && s[i] != '-')) {
            return v;
        }
        const char op = s[i++];
        const double r = term();
        v = op == '+' ? v + r : v - r;
    }
}

}  // namespace

bool eval_math(const std::string & text, double * value, std::string * shown) {
    const std::string src = math_source(text);
    if (src.empty() || !value || !shown) {
        return false;
    }
    Parser p{ src };
    const double v = p.expr();
    p.skip();
    if (p.bad || p.i != src.size() || !std::isfinite(v)) {
        return false;
    }
    *value = v;
    char num[64];
    std::snprintf(num, sizeof(num), "%.10g", v);
    *shown = src + " = " + num;
    return true;
}

}  // namespace ps5lm
