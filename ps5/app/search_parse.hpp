// Turn a search page into titles, links and snippets.
// No regular expressions: std::regex recurses per character and a long page
// overflows the stack, which is why the decision path stopped using it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace ps5lm {

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

std::string url_encode(const std::string & text);
std::string url_decode(const std::string & text);
std::string html_unescape(std::string text);

// DuckDuckGo's lite and html result pages. A bot wall yields nothing.
std::vector<SearchHit> parse_duckduckgo(const std::string & html, int limit);

// Wikipedia's opensearch JSON: [query, [titles], [descriptions], [urls]].
std::vector<SearchHit> parse_wikipedia_opensearch(const std::string & json, int limit);

// SearXNG's JSON: {"results":[{"title","url","content"}, ...]}.
std::vector<SearchHit> parse_searxng(const std::string & json, int limit);

// Visible text of an HTML page, scripts and styles dropped, whitespace
// collapsed. Already-plain text is returned trimmed.
std::string html_to_text(const std::string & html, size_t max_chars);

// http and https links in the page, scripts and styles skipped.
std::vector<std::string> html_links(const std::string & html, int limit);

// Visible text, then up to a few links. Used when a page is opened for research.
std::string html_page(const std::string & html, size_t max_chars);

std::string format_hits(const std::vector<SearchHit> & hits);

// A question that is only arithmetic, such as "What is 17 times 23?".
// shown is "17 * 23 = 391" when it parses. Letters that are not the
// words times, plus, minus, divided or over make it fail.
bool eval_math(const std::string & text, double * value, std::string * shown);

// The console clock, and a web search when the question is about today.
// Implemented in search.cpp. clock_on and web_on follow Settings.
struct LiveFacts {
    bool used_clock = false;
    bool used_search = false;
    std::string day;
    std::string clock;
    std::string search;
    bool used_calc = false;
    std::string calc;
};

LiveFacts live_facts(const std::string & question, bool clock_on, bool web_on);

}  // namespace ps5lm
