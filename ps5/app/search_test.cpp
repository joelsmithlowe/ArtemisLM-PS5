// Host check for the search page parser:
//   c++ -std=c++17 -O1 ps5/app/search_test.cpp ps5/app/search_parse.cpp -o build/search_test
//   build/search_test
// SPDX-License-Identifier: GPL-3.0-or-later
#include "search_parse.hpp"

#include <cstdio>
#include <string>

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}

}  // namespace

int main() {
    using namespace ps5lm;
    double value = 0;
    std::string shown;
    expect(eval_math("What is 17 times 23?", &value, &shown) && value == 391.0, "times");
    expect(eval_math("hello", &value, &shown) == false, "not math");
    expect(eval_math("(2 + 3) * 4", &value, &shown) && value == 20.0, "parens");
    expect(url_encode("a b/c") == "a%20b%2Fc", "encode");
    expect(url_decode("a%20b%2Fc+") == "a b/c ", "decode");
    expect(html_unescape("A &amp; B &#39;c&#39;") == "A & B 'c'", "unescape");

    const std::string ddg =
        "<a rel=\"nofollow\" href=\"https://example.com/a\" class=\"result-link\">Example &amp; Co</a>"
        "<td class=\"result-snippet\">A short snippet.</td>"
        "<a rel=\"nofollow\" class=\"result__a\" "
        "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fb&amp;rut=1\">Second</a>"
        "<a class=\"result__snippet\">Second snippet</a>"
        "<div class=\"anomaly-modal\">bots</div>";
    // A challenge page is not a result list, even when it contains links.
    expect(parse_duckduckgo(ddg, 5).empty(), "bot wall");

    const std::string page = ddg.substr(0, ddg.find("<div class=\"anomaly-modal\">"));
    const auto hits = parse_duckduckgo(page, 5);
    expect(hits.size() == 2, "two hits");
    if (hits.size() == 2) {
        expect(hits[0].title == "Example & Co", "title");
        expect(hits[0].url == "https://example.com/a", "url");
        expect(hits[0].snippet == "A short snippet.", "snippet");
        expect(hits[1].url == "https://example.com/b", "uddg");
        expect(hits[1].snippet == "Second snippet", "html snippet");
    }

    const auto wiki = parse_wikipedia_opensearch(
        "[\"cats\",[\"Cat\",\"Catalog\"],[\"A feline.\",\"A list.\"],"
        "[\"https://en.wikipedia.org/wiki/Cat\",\"https://en.wikipedia.org/wiki/Catalog\"]]",
        5);
    expect(wiki.size() == 2 && wiki[0].title == "Cat" && wiki[0].url.find("/Cat") != std::string::npos &&
               wiki[0].snippet == "A feline." && wiki[1].title == "Catalog",
           "wikipedia");

    const auto searx = parse_searxng(
        "{\"results\":[{\"title\":\"Hello\",\"url\":\"https://example.com/h\",\"content\":\"Hi there\"},"
        "{\"url\":\"https://example.com/h\",\"title\":\"Duplicate\"}]}",
        5);
    expect(searx.size() == 1 && searx[0].title == "Hello" && searx[0].snippet == "Hi there", "searx dedup");

    const std::string text = html_to_text("<p>Hello <b>there</b></p><script>secret()</script><p>World</p>", 1000);
    expect(text.find("Hello") != std::string::npos && text.find("there") != std::string::npos &&
               text.find("World") != std::string::npos && text.find("secret") == std::string::npos,
           "html text");
    expect(html_to_text("plain   words", 100) == "plain words", "plain");

    const std::string formatted = format_hits(hits);
    expect(formatted.find("1. Example & Co") == 0 && formatted.find("https://example.com/a") != std::string::npos, "format");

    if (fails) {
        std::printf("search_test: %d failed\n", fails);
        return 1;
    }
    std::puts("search_test: ok");
    return 0;
}
