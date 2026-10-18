// Web search and page fetch for the chat's tools.
//
// A title cannot start a process, so there is no system Python, curl or
// shell to hand this to. The request goes out through libSceHttp2, the same
// stack the model downloads use, and the page is read in this process.
// llama-server calls ps5lm_web_search and ps5lm_web_fetch (patches/0008);
// any other build leaves those symbols unused.
//
// ARTEMISLM_SEARCH_URL, or the older PS5LM_SEARCH_URL, one line in
// /data/ArtemisLM/env.txt, points the search at a SearXNG (or compatible)
// JSON endpoint instead of DuckDuckGo and Wikipedia.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "search_parse.hpp"
#include "fetch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace {

std::mutex g_search;
int        g_template = -2;

int search_template() {
    if (g_template == -2) {
        // A browser agent: the lite search page answers a script's agent
        // with a challenge instead of results.
        g_template = ps5lm::https_template(
            "Mozilla/5.0 (PlayStation; PS5) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15");
    }
    return g_template;
}

// The body, or empty with the reason in error. Stops after max_bytes: a
// search page is small, and a long read holds the only HTTP context.
std::string get_text(const std::string & url, size_t max_bytes, std::string * error) {
    const int tmpl = search_template();
    if (tmpl < 0) {
        *error = "HTTPS is not available";
        return {};
    }
    std::string body;
    body.reserve(std::min(max_bytes, (size_t) 65536));
    const ps5lm::FetchResult r = ps5lm::https_get(url, "", [&](const char * p, size_t n) {
        if (body.size() >= max_bytes) {
            return false;
        }
        const size_t room = max_bytes - body.size();
        body.append(p, std::min(n, room));
        return body.size() < max_bytes;
    }, tmpl, 64u << 10);
    if (r.status != 200) {
        *error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        return {};
    }
    return body;
}

bool http_url(const std::string & url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return false;
    }
    // A credential in the URL would be sent and then shown to the model.
    const size_t scheme = url.find("://");
    const size_t slash = url.find('/', scheme + 3);
    const std::string host = url.substr(scheme + 3, slash == std::string::npos ? std::string::npos : slash - (scheme + 3));
    return host.find('@') == std::string::npos && host.find(' ') == std::string::npos && !host.empty();
}

// A question about today often arrives without a year. The chat page is
// supposed to read the clock first and put the date in the query. This
// still adds it when a client searches "today" and leaves the year out,
// and it leaves a query that already names a year alone.
bool has_year(const std::string & q) {
    for (size_t i = 0; i + 4 <= q.size(); ++i) {
        if (!std::isdigit((unsigned char) q[i])) {
            continue;
        }
        int n = 0;
        size_t j = i;
        while (j < q.size() && std::isdigit((unsigned char) q[j]) && n < 4) {
            ++n;
            ++j;
        }
        if (n == 4 && (j == q.size() || !std::isdigit((unsigned char) q[j]))) {
            const int year = std::atoi(q.substr(i, 4).c_str());
            if (year >= 1990 && year <= 2100) {
                return true;
            }
        }
    }
    return false;
}

// A whole word, so "now" does not match inside "unknown" and "date" does not
// match inside "update". q is already lower case.
bool has_word(const std::string & q, const char * w) {
    const size_t n = std::strlen(w);
    if (n == 0) {
        return false;
    }
    for (size_t at = 0; (at = q.find(w, at)) != std::string::npos; at += n) {
        const bool left = at > 0 && std::isalnum((unsigned char) q[at - 1]);
        const bool right = at + n < q.size() && std::isalnum((unsigned char) q[at + n]);
        if (!left && !right) {
            return true;
        }
    }
    return false;
}

bool any_word(const std::string & q, const char * const * words, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (has_word(q, words[i])) {
            return true;
        }
    }
    return false;
}

// Words that mean the question is about the clock itself, not a topic.
bool skip_token(const std::string & w) {
    static const char * skip[] = {
        "what", "whats", "is", "the", "a", "an", "of", "to", "in", "on", "for", "me", "tell", "please",
        "right", "this", "it", "its", "and", "or", "do", "you", "can", "how", "about", "there", "here",
        "from", "with", "that", "my", "your", "our", "who", "when", "where", "why", "which",
        "today", "tonight", "now", "current", "latest", "date", "time", "day", "weather", "price",
        "rate", "exchange", "news", "console", "playstation", "ps5", "artemis", "artemislm", "phone", "clock",
    };
    for (const char * s : skip) {
        if (w == s) {
            return true;
        }
    }
    return false;
}

// Something besides the clock words is left, such as "president" after "current".
bool has_topic(const std::string & q) {
    std::string w;
    for (size_t i = 0; i <= q.size(); ++i) {
        const unsigned char c = i < q.size() ? (unsigned char) q[i] : ' ';
        if (std::isalnum(c)) {
            w.push_back((char) c);
            continue;
        }
        if (w.size() >= 3 && !skip_token(w)) {
            return true;
        }
        w.clear();
    }
    return false;
}

// search: read the clock and search. clock: the clock is the whole answer.
// none: leave the question alone. "hello" is none. A rate today is search.
enum class LiveKind { none, clock, search };

LiveKind classify(std::string q) {
    for (char & c : q) {
        c = (char) std::tolower((unsigned char) c);
    }
    static const char * search_words[] = { "weather", "price", "rate", "exchange", "news", "usd", "aud", "eur",
                                            "gbp", "jpy", "cad", "nzd", "cny", "stock", "score", "forecast",
                                            "population", "bitcoin", "research", "scrape" };
    static const char * clock_words[] = { "today", "tonight", "now", "current", "latest", "date", "time", "day",
                                           "weather", "price", "rate", "exchange" };
    const bool topical = any_word(q, search_words, sizeof(search_words) / sizeof(search_words[0]));
    const bool clockish = topical || any_word(q, clock_words, sizeof(clock_words) / sizeof(clock_words[0]));
    if (!clockish) {
        return LiveKind::none;
    }
    if (topical || has_topic(q)) {
        return LiveKind::search;
    }
    return LiveKind::clock;
}

std::string with_today(std::string q) {
    if (has_year(q) || classify(q) == LiveKind::none) {
        return q;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    if (!localtime_r(&now, &tm)) {
        return q;
    }
    char day[16];
    if (!std::strftime(day, sizeof(day), "%Y-%m-%d", &tm)) {
        return q;
    }
    return q + " " + day;
}

// Two currency codes in the question, in the order they are written.
bool currency_pair(const std::string & query, std::string * from, std::string * to) {
    static const char * codes[] = { "usd", "aud", "eur", "gbp", "jpy", "cad", "nzd", "cny", "chf", "sek", "nok",
                                    "dkk", "inr", "mxn", "sgd", "hkd" };
    std::string q = query;
    for (char & c : q) {
        c = (char) std::tolower((unsigned char) c);
    }
    size_t at_from = std::string::npos, at_to = std::string::npos;
    std::string first, second;
    for (const char * code : codes) {
        if (!has_word(q, code)) {
            continue;
        }
        const size_t n = std::strlen(code);
        size_t at = std::string::npos;
        for (size_t i = 0; (i = q.find(code, i)) != std::string::npos; i += n) {
            const bool left = i > 0 && std::isalnum((unsigned char) q[i - 1]);
            const bool right = i + n < q.size() && std::isalnum((unsigned char) q[i + n]);
            if (!left && !right) {
                at = i;
                break;
            }
        }
        if (at == std::string::npos) {
            continue;
        }
        if (at < at_from) {
            at_to = at_from;
            second = first;
            at_from = at;
            first = code;
        } else if (at < at_to) {
            at_to = at;
            second = code;
        }
    }
    if (second.empty()) {
        return false;
    }
    for (char & c : first) {
        c = (char) std::toupper((unsigned char) c);
    }
    for (char & c : second) {
        c = (char) std::toupper((unsigned char) c);
    }
    *from = first;
    *to = second;
    return true;
}

// Frankfurter serves the ECB reference rate. No key. A failure leaves the
// web search to answer on its own.
std::string currency_note(const std::string & query) {
    std::string from, to;
    if (!currency_pair(query, &from, &to)) {
        return {};
    }
    std::string error;
    const std::string body = get_text("https://api.frankfurter.app/latest?from=" + from + "&to=" + to, 16u << 10, &error);
    const std::string key = "\"" + to + "\":";
    const size_t at = body.find(key);
    if (at == std::string::npos) {
        return {};
    }
    const double rate = std::atof(body.c_str() + at + key.size());
    if (rate <= 0.0) {
        return {};
    }
    std::string day;
    const size_t date = body.find("\"date\":\"");
    if (date != std::string::npos && date + 18 <= body.size()) {
        day = body.substr(date + 8, 10);
    }
    char line[160];
    std::snprintf(line, sizeof(line), "Exchange rate (European Central Bank): 1 %s = %.4f %s%s%s.", from.c_str(), rate,
                  to.c_str(), day.empty() ? "" : " on ", day.c_str());
    return line;
}

void finish(char * out, size_t out_cap, const std::string & text) {
    if (!out || out_cap == 0) {
        return;
    }
    const size_t n = std::min(text.size(), out_cap - 1);
    text.copy(out, n);
    out[n] = '\0';
}

}  // namespace

extern "C" int ps5lm_web_search(const char * query, int num_results, char * out, size_t out_cap) {
    if (!query || !*query || !out || out_cap < 2) {
        finish(out, out_cap, "query is empty");
        return -1;
    }
    num_results = std::max(1, std::min(num_results, 8));
    const std::string q = ps5lm::url_encode(with_today(query));
    std::lock_guard<std::mutex> lock(g_search);
    const std::string rate = currency_note(query);
    std::string error;
    std::vector<ps5lm::SearchHit> hits;
    const char * searx = std::getenv("ARTEMISLM_SEARCH_URL");
    if (!searx || !*searx) {
        searx = std::getenv("PS5LM_SEARCH_URL");
    }
    if (searx && *searx) {
        const std::string url = std::string(searx) + (std::string(searx).find('?') == std::string::npos ? "?" : "&") +
                                "q=" + q + "&format=json";
        const std::string body = get_text(url, 256u << 10, &error);
        hits = ps5lm::parse_searxng(body, num_results);
    } else {
        const std::string body = get_text("https://lite.duckduckgo.com/lite/?q=" + q, 256u << 10, &error);
        hits = ps5lm::parse_duckduckgo(body, num_results);
        if ((int) hits.size() < num_results) {
            std::string wiki_error;
            const std::string wiki = get_text(
                "https://en.wikipedia.org/w/api.php?action=opensearch&limit=5&namespace=0&format=json&search=" + q,
                64u << 10, &wiki_error);
            for (auto & hit : ps5lm::parse_wikipedia_opensearch(wiki, num_results)) {
                if ((int) hits.size() >= num_results) {
                    break;
                }
                bool seen = false;
                for (const auto & have : hits) {
                    seen = seen || have.url == hit.url;
                }
                if (!seen) {
                    hits.push_back(std::move(hit));
                }
            }
            if (hits.empty() && error.empty()) {
                error = wiki_error;
            }
        }
    }
    if (hits.empty()) {
        if (!rate.empty()) {
            finish(out, out_cap, rate);
            return 0;
        }
        finish(out, out_cap, error.empty() ? "no results" : "web search failed: " + error);
        return -1;
    }
    std::string text = rate.empty() ? ps5lm::format_hits(hits) : rate + "\n" + ps5lm::format_hits(hits);
    std::string lower = query;
    for (char & c : lower) {
        c = (char) std::tolower((unsigned char) c);
    }
    // One result page, and only when the question asked for research or a scrape.
    // get_text, not ps5lm_web_fetch: this function already holds g_search.
    if ((has_word(lower, "research") || has_word(lower, "scrape")) && http_url(hits[0].url)) {
        std::string page_error;
        const std::string raw = get_text(hits[0].url, 256u << 10, &page_error);
        if (!raw.empty()) {
            text += "\n\nPage (" + hits[0].url + "):\n" + ps5lm::html_page(raw, 2500);
        }
    }
    finish(out, out_cap, text);
    return 0;
}

extern "C" int ps5lm_web_fetch(const char * url, char * out, size_t out_cap) {
    if (!url || !http_url(url) || !out || out_cap < 2) {
        finish(out, out_cap, "url must be an http or https address, without a password in it");
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_search);
    std::string error;
    const std::string body = get_text(url, 256u << 10, &error);
    if (body.empty()) {
        finish(out, out_cap, error.empty() ? "empty page" : "could not open the page: " + error);
        return -1;
    }
    // Text plus a few links. Long enough to answer from, short enough that
    // one fetch does not fill the context on its own.
    finish(out, out_cap, ps5lm::html_page(body, 6000));
    return 0;
}

namespace ps5lm {

LiveFacts live_facts(const std::string & question, bool clock_on, bool web_on, int search_hits) {
    LiveFacts f;
    const LiveKind kind = classify(question);
    if (kind == LiveKind::none) {
        double value = 0;
        std::string shown;
        if (eval_math(question, &value, &shown)) {
            f.used_calc = true;
            f.calc = shown;
        }
        return f;
    }
    const bool search = kind == LiveKind::search;
    if (clock_on) {
        f.used_clock = true;
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        char iso[16];
        char human[96];
        if (!localtime_r(&now, &tm) || !std::strftime(iso, sizeof(iso), "%Y-%m-%d", &tm)) {
            f.clock = "the clock could not be read";
        } else {
            f.day = iso;
            if (std::strftime(human, sizeof(human), "%A, %d %B %Y, %H:%M", &tm)) {
                f.clock = f.day + " (" + human + ")";
            } else {
                f.clock = f.day;
            }
        }
    }
    if (search && web_on) {
        f.used_search = true;
        std::string query = question;
        if (!f.day.empty() && question.find(f.day) == std::string::npos) {
            query += " " + f.day;
        }
        std::string buf(24 * 1024, '\0');
        const int hits = search_hits == 3 || search_hits == 8 ? search_hits : 5;
        ps5lm_web_search(query.c_str(), hits, buf.data(), buf.size());
        f.search = buf.c_str();
        if (f.search.empty()) {
            f.search = "no results";
        }
    }
    return f;
}

}  // namespace ps5lm
