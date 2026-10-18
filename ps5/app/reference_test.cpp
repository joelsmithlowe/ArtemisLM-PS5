// Host check for the on-console reference tables.
//   c++ -std=c++20 -Wall -Wextra -I ps5/app -I <llama vendor> ps5/app/reference_test.cpp ps5/app/reference.cpp -o /tmp/reference_test
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "reference.hpp"

#include <nlohmann/json.hpp>

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

nlohmann::json call(const nlohmann::json & body) {
    const std::string raw = ps5lm::reference_call(body.dump());
    const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        std::fprintf(stderr, "FAIL bad json: %s\n", raw.c_str());
        ++fails;
        return nlohmann::json::object();
    }
    return parsed;
}

bool has_error(const nlohmann::json & j) { return j.contains("error") && j["error"].is_string(); }

}  // namespace

int main() {
    const auto km = call({ { "tool", "convert" }, { "value", 5 }, { "from", "km" }, { "to", "miles" } });
    expect(!has_error(km) && km.value("text", "").find("3.106855961") != std::string::npos, "5 km to miles");

    const auto freeze = call({ { "tool", "convert" }, { "value", 32 }, { "from", "fahrenheit" }, { "to", "celsius" } });
    expect(!has_error(freeze) && freeze.value("text", "").find("= 0 ") != std::string::npos, "32 F to C");

    const auto boil = call({ { "tool", "convert" }, { "value", 100 }, { "from", "celsius" }, { "to", "fahrenheit" } });
    expect(!has_error(boil) && boil.value("text", "").find("212") != std::string::npos, "100 C to F");

    const auto same = call({ { "tool", "convert" }, { "value", -40 }, { "from", "C" }, { "to", "F" } });
    expect(!has_error(same) && same.value("text", "").find("-40") != std::string::npos, "-40 C to F");

    const auto kelvin = call({ { "tool", "convert" }, { "value", 0 }, { "from", "celsius" }, { "to", "kelvin" } });
    expect(!has_error(kelvin) && kelvin.value("text", "").find("273.15") != std::string::npos, "0 C to K");

    const auto kib = call({ { "tool", "convert" }, { "value", 1 }, { "from", "KiB" }, { "to", "bytes" } });
    expect(!has_error(kib) && kib.value("text", "").find("1024") != std::string::npos, "1 KiB");

    const auto kb = call({ { "tool", "convert" }, { "value", 1 }, { "from", "KB" }, { "to", "bytes" } });
    expect(!has_error(kb) && kb.value("text", "").find("= 1000 ") != std::string::npos, "1 KB");

    const auto hours = call({ { "tool", "convert" }, { "value", 7 }, { "from", "days" }, { "to", "hours" } });
    expect(!has_error(hours) && hours.value("text", "").find("168") != std::string::npos, "7 days in hours");

    const auto mixed = call({ { "tool", "convert" }, { "value", 5 }, { "from", "km" }, { "to", "pounds" } });
    expect(has_error(mixed), "length is not mass");

    const auto sunday = call({ { "tool", "calendar" }, { "op", "weekday" }, { "date", "2026-10-18" } });
    expect(!has_error(sunday) && sunday.value("text", "").find("Sunday") != std::string::npos, "2026-10-18 Sunday");

    const auto epoch = call({ { "tool", "calendar" }, { "op", "weekday" }, { "date", "1970-01-01" } });
    expect(!has_error(epoch) && epoch.value("text", "").find("Thursday") != std::string::npos, "epoch Thursday");

    const auto y2k = call({ { "tool", "calendar" }, { "op", "weekday" }, { "date", "1 January 2000" } });
    expect(!has_error(y2k) && y2k.value("text", "").find("Saturday") != std::string::npos, "2000-01-01 Saturday");

    const auto leap = call({ { "tool", "calendar" }, { "op", "between" }, { "date", "2024-02-28" }, { "end", "2024-03-01" } });
    expect(!has_error(leap) && leap.value("text", "").find("2 days") != std::string::npos, "leap span");

    const auto later = call({ { "tool", "calendar" }, { "op", "add" }, { "date", "2026-10-18" }, { "days", 10 } });
    expect(!has_error(later) && later.value("text", "").find("2026-10-28") != std::string::npos, "plus 10 days");

    const auto bad_day = call({ { "tool", "calendar" }, { "op", "weekday" }, { "date", "2026-02-31" } });
    expect(has_error(bad_day), "reject 31 February");

    const auto light = call({ { "tool", "lookup" }, { "kind", "constant" }, { "name", "speed of light" } });
    expect(!has_error(light) && light.value("text", "").find("299792458") != std::string::npos, "speed of light");

    const auto iron = call({ { "tool", "lookup" }, { "kind", "element" }, { "name", "Fe" } });
    expect(!has_error(iron) && iron.value("text", "").find("element 26") != std::string::npos, "iron");
    expect(iron.value("text", "").find("55.845") != std::string::npos, "iron weight");

    const auto gold = call({ { "tool", "lookup" }, { "kind", "element" }, { "name", "gold" } });
    expect(!has_error(gold) && gold.value("text", "").find("Au") != std::string::npos, "gold");

    const auto missing = call({ { "tool", "lookup" }, { "kind", "http" }, { "code", 404 } });
    expect(!has_error(missing) && missing.value("text", "").find("Not Found") != std::string::npos, "http 404");

    const auto nano = call({ { "tool", "lookup" }, { "kind", "prefix" }, { "name", "nano" } });
    expect(!has_error(nano) && nano.value("text", "").find("10^-9") != std::string::npos, "nano");

    const auto hi = call({ { "tool", "text" }, { "op", "base64" }, { "text", "hi" } });
    expect(!has_error(hi) && hi.value("text", "") == "aGk=", "base64 hi");

    const auto back = call({ { "tool", "text" }, { "op", "unbase64" }, { "text", "aGk=" } });
    expect(!has_error(back) && back.value("text", "") == "hi", "unbase64 hi");

    const auto abc = call({ { "tool", "text" }, { "op", "sha256" }, { "text", "abc" } });
    expect(abc.value("text", "") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");

    const auto empty_hash = call({ { "tool", "text" }, { "op", "sha256" }, { "text", "" } });
    expect(empty_hash.value("text", "") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");

    const auto counted = call({ { "tool", "text" }, { "op", "count" }, { "text", "one two" } });
    expect(counted.value("text", "").find("2 words") != std::string::npos, "count words");

    const auto huge = call({ { "tool", "text" }, { "op", "count" }, { "text", std::string(8001, 'a') } });
    expect(has_error(huge), "text cap");

    const std::string miles = ps5lm::reference_note("convert 5 km to miles");
    expect(miles.find("3.106855961") != std::string::npos, "note km");
    expect(ps5lm::reference_note("how many miles in 5 km").find("miles") != std::string::npos, "note how many");
    expect(ps5lm::reference_note("What day of the week is 2026-10-18?").find("Sunday") != std::string::npos, "note weekday");
    expect(ps5lm::reference_note("how many days between 2024-02-28 and 2024-03-01").find("2 days") != std::string::npos, "note between");
    expect(ps5lm::reference_note("10 days after 2026-10-18").find("2026-10-28") != std::string::npos, "note after");
    expect(ps5lm::reference_note("what is the speed of light").find("299792458") != std::string::npos, "note light");
    expect(ps5lm::reference_note("atomic number of iron").find("26") != std::string::npos, "note iron");
    expect(ps5lm::reference_note("what is http status 404").find("Not Found") != std::string::npos, "note 404");
    expect(ps5lm::reference_note("sha256 of abc").find("ba7816bf") != std::string::npos, "note sha");
    expect(ps5lm::reference_note("What is 17 times 23?").empty(), "not math");
    expect(ps5lm::reference_note("usd to aud today").empty(), "not a rate");
    expect(ps5lm::reference_note("weather tomorrow").empty(), "not weather");
    expect(ps5lm::reference_note("what is the price of gold").empty(), "not a gold price");
    expect(ps5lm::reference_note("there is no element called wood").empty(), "no is not nobelium");

    const auto none = call({ { "tool", "note" }, { "text", "hello" } });
    expect(!has_error(none) && none.value("text", "").find("Nothing in this table") != std::string::npos, "note miss");

    if (fails) {
        std::fprintf(stderr, "reference_test: %d failed\n", fails);
        return 1;
    }
    std::printf("reference_test: ok\n");
    return 0;
}
