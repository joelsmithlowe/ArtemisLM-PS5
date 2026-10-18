// In-process reference for the model. No Python, no shell, no extra library.
// SHA-256 and base64 are the standard algorithms, written here so the title
// does not gain a dependency. The day count is Howard Hinnant's civil calendar.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "reference.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ps5lm {

namespace {

constexpr int kTextCap = 8000;

std::string fail(const std::string & message) {
    return nlohmann::json{ { "error", message } }.dump();
}

std::string ok_text(const std::string & text) {
    return nlohmann::json{ { "text", text } }.dump();
}

std::string lower_copy(const std::string & in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char) in[i];
        if (c == 0xC2 && i + 1 < in.size() && (unsigned char) in[i + 1] == 0xB5) {
            out.push_back('u');
            i += 2;
            continue;
        }
        if (c == 0xCE && i + 1 < in.size() && (unsigned char) in[i + 1] == 0xBC) {
            out.push_back('u');
            i += 2;
            continue;
        }
        if (c == 0xC2 && i + 1 < in.size() && (unsigned char) in[i + 1] == 0xB0) {
            i += 2;
            continue;
        }
        if (c < 128) {
            out.push_back((char) std::tolower(c));
            i += 1;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < in.size()) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < in.size()) {
            i += 4;
        } else {
            i += 1;
        }
    }
    return out;
}

bool word_char(unsigned char c) { return std::isalnum(c) != 0; }

bool has_word(const std::string & q, const char * word) {
    const size_t n = std::strlen(word);
    if (n == 0) {
        return false;
    }
    for (size_t at = 0; (at = q.find(word, at)) != std::string::npos; at += n) {
        const bool left = at > 0 && word_char((unsigned char) q[at - 1]);
        const bool right = at + n < q.size() && word_char((unsigned char) q[at + n]);
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

// Weather, prices and rates stay on the search path. A plain conversion
// can still match, because "5 km to miles" is not a price.
bool live_topic(const std::string & q) {
    static const char * words[] = { "weather", "price", "rate", "exchange", "news", "usd", "aud", "eur", "gbp",
                                     "jpy", "cad", "nzd", "cny", "stock", "score", "forecast", "population",
                                     "bitcoin", "research", "scrape" };
    return any_word(q, words, sizeof(words) / sizeof(words[0]));
}

std::string format_num(double v) {
    if (!std::isfinite(v)) {
        return "not a number";
    }
    const double rounded = std::round(v);
    if (std::fabs(v - rounded) <= 1e-8 * std::max(1.0, std::fabs(v)) && std::fabs(rounded) < 1e15) {
        return std::to_string((long long) rounded);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string json_string(const nlohmann::json & j, const char * key) {
    if (!j.is_object() || !j.contains(key)) {
        return {};
    }
    if (j[key].is_string()) {
        return j[key].get<std::string>();
    }
    if (j[key].is_number() && !j[key].is_boolean()) {
        return j[key].dump();
    }
    return {};
}

bool json_number(const nlohmann::json & j, const char * key, double * out) {
    if (!j.is_object() || !j.contains(key) || !out) {
        return false;
    }
    if (j[key].is_number() && !j[key].is_boolean()) {
        *out = j[key].get<double>();
        return std::isfinite(*out);
    }
    if (!j[key].is_string()) {
        return false;
    }
    const std::string text = j[key].get<std::string>();
    if (text.empty()) {
        return false;
    }
    char * end = nullptr;
    *out = std::strtod(text.c_str(), &end);
    return end && end != text.c_str() && *end == '\0' && std::isfinite(*out);
}

// --- units -----------------------------------------------------------------

enum { kLength, kMass, kTemp, kVolume, kArea, kSpeed, kData, kTime, kPressure, kEnergy };

struct Unit {
    const char * key;
    int          dim;
    int          mode;  // 0 scale, 1 celsius, 2 fahrenheit, 3 kelvin
    int          bytes; // 0 none, 1 decimal, 2 binary, 3 a single byte
    double       to_si;
    const char * label;
};

const Unit * unit_table(size_t * count) {
    static const Unit table[] = {
        { "m", kLength, 0, 0, 1, "m" },
        { "meter", kLength, 0, 0, 1, "m" },
        { "meters", kLength, 0, 0, 1, "m" },
        { "metre", kLength, 0, 0, 1, "m" },
        { "metres", kLength, 0, 0, 1, "m" },
        { "km", kLength, 0, 0, 1000, "km" },
        { "kilometer", kLength, 0, 0, 1000, "km" },
        { "kilometers", kLength, 0, 0, 1000, "km" },
        { "kilometre", kLength, 0, 0, 1000, "km" },
        { "kilometres", kLength, 0, 0, 1000, "km" },
        { "cm", kLength, 0, 0, 0.01, "cm" },
        { "centimeter", kLength, 0, 0, 0.01, "cm" },
        { "centimeters", kLength, 0, 0, 0.01, "cm" },
        { "centimetre", kLength, 0, 0, 0.01, "cm" },
        { "centimetres", kLength, 0, 0, 0.01, "cm" },
        { "mm", kLength, 0, 0, 0.001, "mm" },
        { "millimeter", kLength, 0, 0, 0.001, "mm" },
        { "millimeters", kLength, 0, 0, 0.001, "mm" },
        { "millimetre", kLength, 0, 0, 0.001, "mm" },
        { "millimetres", kLength, 0, 0, 0.001, "mm" },
        { "um", kLength, 0, 0, 1e-6, "um" },
        { "micrometre", kLength, 0, 0, 1e-6, "um" },
        { "micrometres", kLength, 0, 0, 1e-6, "um" },
        { "micrometer", kLength, 0, 0, 1e-6, "um" },
        { "micrometers", kLength, 0, 0, 1e-6, "um" },
        { "micron", kLength, 0, 0, 1e-6, "um" },
        { "microns", kLength, 0, 0, 1e-6, "um" },
        { "nm", kLength, 0, 0, 1e-9, "nm" },
        { "nanometre", kLength, 0, 0, 1e-9, "nm" },
        { "nanometres", kLength, 0, 0, 1e-9, "nm" },
        { "nanometer", kLength, 0, 0, 1e-9, "nm" },
        { "nanometers", kLength, 0, 0, 1e-9, "nm" },
        { "in", kLength, 0, 0, 0.0254, "in" },
        { "inch", kLength, 0, 0, 0.0254, "in" },
        { "inches", kLength, 0, 0, 0.0254, "in" },
        { "ft", kLength, 0, 0, 0.3048, "ft" },
        { "foot", kLength, 0, 0, 0.3048, "ft" },
        { "feet", kLength, 0, 0, 0.3048, "ft" },
        { "yd", kLength, 0, 0, 0.9144, "yd" },
        { "yard", kLength, 0, 0, 0.9144, "yd" },
        { "yards", kLength, 0, 0, 0.9144, "yd" },
        { "mi", kLength, 0, 0, 1609.344, "miles" },
        { "mile", kLength, 0, 0, 1609.344, "miles" },
        { "miles", kLength, 0, 0, 1609.344, "miles" },
        { "nmi", kLength, 0, 0, 1852, "nautical miles" },
        { "nauticalmile", kLength, 0, 0, 1852, "nautical miles" },
        { "nauticalmiles", kLength, 0, 0, 1852, "nautical miles" },
        { "kg", kMass, 0, 0, 1, "kg" },
        { "kilogram", kMass, 0, 0, 1, "kg" },
        { "kilograms", kMass, 0, 0, 1, "kg" },
        { "g", kMass, 0, 0, 0.001, "g" },
        { "gram", kMass, 0, 0, 0.001, "g" },
        { "grams", kMass, 0, 0, 0.001, "g" },
        { "mg", kMass, 0, 0, 1e-6, "mg" },
        { "milligram", kMass, 0, 0, 1e-6, "mg" },
        { "milligrams", kMass, 0, 0, 1e-6, "mg" },
        { "lb", kMass, 0, 0, 0.45359237, "lb" },
        { "lbs", kMass, 0, 0, 0.45359237, "lb" },
        { "pound", kMass, 0, 0, 0.45359237, "lb" },
        { "pounds", kMass, 0, 0, 0.45359237, "lb" },
        { "oz", kMass, 0, 0, 0.028349523125, "oz" },
        { "ounce", kMass, 0, 0, 0.028349523125, "oz" },
        { "ounces", kMass, 0, 0, 0.028349523125, "oz" },
        { "stone", kMass, 0, 0, 6.35029318, "stone" },
        { "stones", kMass, 0, 0, 6.35029318, "stone" },
        { "tonne", kMass, 0, 0, 1000, "tonnes" },
        { "tonnes", kMass, 0, 0, 1000, "tonnes" },
        { "metricton", kMass, 0, 0, 1000, "tonnes" },
        { "metrictons", kMass, 0, 0, 1000, "tonnes" },
        { "shortton", kMass, 0, 0, 907.18474, "US short tons" },
        { "shorttons", kMass, 0, 0, 907.18474, "US short tons" },
        { "uston", kMass, 0, 0, 907.18474, "US short tons" },
        { "ustons", kMass, 0, 0, 907.18474, "US short tons" },
        { "longton", kMass, 0, 0, 1016.0469088, "long tons" },
        { "longtons", kMass, 0, 0, 1016.0469088, "long tons" },
        { "c", kTemp, 1, 0, 0, "°C" },
        { "celsius", kTemp, 1, 0, 0, "°C" },
        { "centigrade", kTemp, 1, 0, 0, "°C" },
        { "f", kTemp, 2, 0, 0, "°F" },
        { "fahrenheit", kTemp, 2, 0, 0, "°F" },
        { "k", kTemp, 3, 0, 0, "K" },
        { "kelvin", kTemp, 3, 0, 0, "K" },
        { "l", kVolume, 0, 0, 1, "L" },
        { "liter", kVolume, 0, 0, 1, "L" },
        { "liters", kVolume, 0, 0, 1, "L" },
        { "litre", kVolume, 0, 0, 1, "L" },
        { "litres", kVolume, 0, 0, 1, "L" },
        { "ml", kVolume, 0, 0, 0.001, "mL" },
        { "milliliter", kVolume, 0, 0, 0.001, "mL" },
        { "milliliters", kVolume, 0, 0, 0.001, "mL" },
        { "millilitre", kVolume, 0, 0, 0.001, "mL" },
        { "millilitres", kVolume, 0, 0, 0.001, "mL" },
        { "m3", kVolume, 0, 0, 1000, "m^3" },
        { "cubicmeter", kVolume, 0, 0, 1000, "m^3" },
        { "cubicmeters", kVolume, 0, 0, 1000, "m^3" },
        { "cubicmetre", kVolume, 0, 0, 1000, "m^3" },
        { "cubicmetres", kVolume, 0, 0, 1000, "m^3" },
        { "gal", kVolume, 0, 0, 3.785411784, "US liquid gallons" },
        { "gallon", kVolume, 0, 0, 3.785411784, "US liquid gallons" },
        { "gallons", kVolume, 0, 0, 3.785411784, "US liquid gallons" },
        { "usgallon", kVolume, 0, 0, 3.785411784, "US liquid gallons" },
        { "usgallons", kVolume, 0, 0, 3.785411784, "US liquid gallons" },
        { "imperialgallon", kVolume, 0, 0, 4.54609, "imperial gallons" },
        { "imperialgallons", kVolume, 0, 0, 4.54609, "imperial gallons" },
        { "ukgallon", kVolume, 0, 0, 4.54609, "imperial gallons" },
        { "ukgallons", kVolume, 0, 0, 4.54609, "imperial gallons" },
        { "qt", kVolume, 0, 0, 0.946352946, "US quarts" },
        { "quart", kVolume, 0, 0, 0.946352946, "US quarts" },
        { "quarts", kVolume, 0, 0, 0.946352946, "US quarts" },
        { "pt", kVolume, 0, 0, 0.473176473, "US pints" },
        { "pint", kVolume, 0, 0, 0.473176473, "US pints" },
        { "pints", kVolume, 0, 0, 0.473176473, "US pints" },
        { "cup", kVolume, 0, 0, 0.2365882365, "US cups" },
        { "cups", kVolume, 0, 0, 0.2365882365, "US cups" },
        { "floz", kVolume, 0, 0, 0.0295735295625, "US fl oz" },
        { "fluidounce", kVolume, 0, 0, 0.0295735295625, "US fl oz" },
        { "fluidounces", kVolume, 0, 0, 0.0295735295625, "US fl oz" },
        { "tbsp", kVolume, 0, 0, 0.01478676478125, "US tablespoons" },
        { "tablespoon", kVolume, 0, 0, 0.01478676478125, "US tablespoons" },
        { "tablespoons", kVolume, 0, 0, 0.01478676478125, "US tablespoons" },
        { "tsp", kVolume, 0, 0, 0.00492892159375, "US teaspoons" },
        { "teaspoon", kVolume, 0, 0, 0.00492892159375, "US teaspoons" },
        { "teaspoons", kVolume, 0, 0, 0.00492892159375, "US teaspoons" },
        { "m2", kArea, 0, 0, 1, "m^2" },
        { "sqm", kArea, 0, 0, 1, "m^2" },
        { "squaremeter", kArea, 0, 0, 1, "m^2" },
        { "squaremeters", kArea, 0, 0, 1, "m^2" },
        { "squaremetre", kArea, 0, 0, 1, "m^2" },
        { "squaremetres", kArea, 0, 0, 1, "m^2" },
        { "km2", kArea, 0, 0, 1e6, "km^2" },
        { "squarekilometer", kArea, 0, 0, 1e6, "km^2" },
        { "squarekilometers", kArea, 0, 0, 1e6, "km^2" },
        { "squarekilometre", kArea, 0, 0, 1e6, "km^2" },
        { "squarekilometres", kArea, 0, 0, 1e6, "km^2" },
        { "ha", kArea, 0, 0, 10000, "hectares" },
        { "hectare", kArea, 0, 0, 10000, "hectares" },
        { "hectares", kArea, 0, 0, 10000, "hectares" },
        { "acre", kArea, 0, 0, 4046.8564224, "acres" },
        { "acres", kArea, 0, 0, 4046.8564224, "acres" },
        { "ft2", kArea, 0, 0, 0.09290304, "ft^2" },
        { "sqft", kArea, 0, 0, 0.09290304, "ft^2" },
        { "squarefoot", kArea, 0, 0, 0.09290304, "ft^2" },
        { "squarefeet", kArea, 0, 0, 0.09290304, "ft^2" },
        { "in2", kArea, 0, 0, 0.00064516, "in^2" },
        { "squareinch", kArea, 0, 0, 0.00064516, "in^2" },
        { "squareinches", kArea, 0, 0, 0.00064516, "in^2" },
        { "mi2", kArea, 0, 0, 2589988.110336, "square miles" },
        { "squaremile", kArea, 0, 0, 2589988.110336, "square miles" },
        { "squaremiles", kArea, 0, 0, 2589988.110336, "square miles" },
        { "m/s", kSpeed, 0, 0, 1, "m/s" },
        { "mps", kSpeed, 0, 0, 1, "m/s" },
        { "meterpersecond", kSpeed, 0, 0, 1, "m/s" },
        { "meterspersecond", kSpeed, 0, 0, 1, "m/s" },
        { "km/h", kSpeed, 0, 0, 1000.0 / 3600.0, "km/h" },
        { "kmh", kSpeed, 0, 0, 1000.0 / 3600.0, "km/h" },
        { "kph", kSpeed, 0, 0, 1000.0 / 3600.0, "km/h" },
        { "mph", kSpeed, 0, 0, 1609.344 / 3600.0, "mph" },
        { "mileperhour", kSpeed, 0, 0, 1609.344 / 3600.0, "mph" },
        { "milesperhour", kSpeed, 0, 0, 1609.344 / 3600.0, "mph" },
        { "knot", kSpeed, 0, 0, 1852.0 / 3600.0, "knots" },
        { "knots", kSpeed, 0, 0, 1852.0 / 3600.0, "knots" },
        { "ft/s", kSpeed, 0, 0, 0.3048, "ft/s" },
        { "fps", kSpeed, 0, 0, 0.3048, "ft/s" },
        { "b", kData, 0, 3, 1, "bytes" },
        { "byte", kData, 0, 3, 1, "bytes" },
        { "bytes", kData, 0, 3, 1, "bytes" },
        { "kb", kData, 0, 1, 1000, "KB" },
        { "kilobyte", kData, 0, 1, 1000, "KB" },
        { "kilobytes", kData, 0, 1, 1000, "KB" },
        { "mb", kData, 0, 1, 1e6, "MB" },
        { "megabyte", kData, 0, 1, 1e6, "MB" },
        { "megabytes", kData, 0, 1, 1e6, "MB" },
        { "gb", kData, 0, 1, 1e9, "GB" },
        { "gigabyte", kData, 0, 1, 1e9, "GB" },
        { "gigabytes", kData, 0, 1, 1e9, "GB" },
        { "tb", kData, 0, 1, 1e12, "TB" },
        { "terabyte", kData, 0, 1, 1e12, "TB" },
        { "terabytes", kData, 0, 1, 1e12, "TB" },
        { "kib", kData, 0, 2, 1024, "KiB" },
        { "kibibyte", kData, 0, 2, 1024, "KiB" },
        { "kibibytes", kData, 0, 2, 1024, "KiB" },
        { "mib", kData, 0, 2, 1048576, "MiB" },
        { "mebibyte", kData, 0, 2, 1048576, "MiB" },
        { "mebibytes", kData, 0, 2, 1048576, "MiB" },
        { "gib", kData, 0, 2, 1073741824.0, "GiB" },
        { "gibibyte", kData, 0, 2, 1073741824.0, "GiB" },
        { "gibibytes", kData, 0, 2, 1073741824.0, "GiB" },
        { "tib", kData, 0, 2, 1099511627776.0, "TiB" },
        { "tebibyte", kData, 0, 2, 1099511627776.0, "TiB" },
        { "tebibytes", kData, 0, 2, 1099511627776.0, "TiB" },
        { "s", kTime, 0, 0, 1, "seconds" },
        { "sec", kTime, 0, 0, 1, "seconds" },
        { "second", kTime, 0, 0, 1, "seconds" },
        { "seconds", kTime, 0, 0, 1, "seconds" },
        { "min", kTime, 0, 0, 60, "minutes" },
        { "minute", kTime, 0, 0, 60, "minutes" },
        { "minutes", kTime, 0, 0, 60, "minutes" },
        { "h", kTime, 0, 0, 3600, "hours" },
        { "hr", kTime, 0, 0, 3600, "hours" },
        { "hour", kTime, 0, 0, 3600, "hours" },
        { "hours", kTime, 0, 0, 3600, "hours" },
        { "day", kTime, 0, 0, 86400, "days" },
        { "days", kTime, 0, 0, 86400, "days" },
        { "week", kTime, 0, 0, 604800, "weeks" },
        { "weeks", kTime, 0, 0, 604800, "weeks" },
        { "pa", kPressure, 0, 0, 1, "Pa" },
        { "pascal", kPressure, 0, 0, 1, "Pa" },
        { "pascals", kPressure, 0, 0, 1, "Pa" },
        { "kpa", kPressure, 0, 0, 1000, "kPa" },
        { "kilopascal", kPressure, 0, 0, 1000, "kPa" },
        { "kilopascals", kPressure, 0, 0, 1000, "kPa" },
        { "bar", kPressure, 0, 0, 100000, "bar" },
        { "bars", kPressure, 0, 0, 100000, "bar" },
        { "psi", kPressure, 0, 0, 6894.757293168, "psi" },
        { "atm", kPressure, 0, 0, 101325, "atm" },
        { "atmosphere", kPressure, 0, 0, 101325, "atm" },
        { "atmospheres", kPressure, 0, 0, 101325, "atm" },
        { "j", kEnergy, 0, 0, 1, "J" },
        { "joule", kEnergy, 0, 0, 1, "J" },
        { "joules", kEnergy, 0, 0, 1, "J" },
        { "kj", kEnergy, 0, 0, 1000, "kJ" },
        { "kilojoule", kEnergy, 0, 0, 1000, "kJ" },
        { "kilojoules", kEnergy, 0, 0, 1000, "kJ" },
        { "cal", kEnergy, 0, 0, 4.184, "cal" },
        { "calorie", kEnergy, 0, 0, 4.184, "cal" },
        { "calories", kEnergy, 0, 0, 4.184, "cal" },
        { "kcal", kEnergy, 0, 0, 4184, "kcal" },
        { "kilocalorie", kEnergy, 0, 0, 4184, "kcal" },
        { "kilocalories", kEnergy, 0, 0, 4184, "kcal" },
        { "wh", kEnergy, 0, 0, 3600, "Wh" },
        { "watthour", kEnergy, 0, 0, 3600, "Wh" },
        { "watthours", kEnergy, 0, 0, 3600, "Wh" },
        { "kwh", kEnergy, 0, 0, 3600000, "kWh" },
        { "kilowatthour", kEnergy, 0, 0, 3600000, "kWh" },
        { "kilowatthours", kEnergy, 0, 0, 3600000, "kWh" },
        { "ev", kEnergy, 0, 0, 1.602176634e-19, "eV" },
        { "electronvolt", kEnergy, 0, 0, 1.602176634e-19, "eV" },
        { "electronvolts", kEnergy, 0, 0, 1.602176634e-19, "eV" },
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

std::string unit_key(const std::string & raw) {
    std::string key;
    for (unsigned char c : raw) {
        if (c == ' ' || c == '_' || c == '-' || c == '.') {
            continue;
        }
        key.push_back((char) std::tolower(c));
    }
    return key;
}

const Unit * find_unit(const std::string & raw) {
    const std::string key = unit_key(raw);
    if (key.empty()) {
        return nullptr;
    }
    size_t count = 0;
    const Unit * table = unit_table(&count);
    for (size_t i = 0; i < count; ++i) {
        if (key == table[i].key) {
            return &table[i];
        }
    }
    return nullptr;
}

const char * dim_name(int dim) {
    switch (dim) {
    case kLength: return "length";
    case kMass: return "mass";
    case kTemp: return "temperature";
    case kVolume: return "volume";
    case kArea: return "area";
    case kSpeed: return "speed";
    case kData: return "data size";
    case kTime: return "time";
    case kPressure: return "pressure";
    case kEnergy: return "energy";
    default: return "measure";
    }
}

double to_kelvin(double v, int mode) {
    if (mode == 1) {
        return v + 273.15;
    }
    if (mode == 2) {
        return (v - 32.0) * 5.0 / 9.0 + 273.15;
    }
    return v;
}

double from_kelvin(double k, int mode) {
    if (mode == 1) {
        return k - 273.15;
    }
    if (mode == 2) {
        return (k - 273.15) * 9.0 / 5.0 + 32.0;
    }
    return k;
}

std::string convert_text(double value, const Unit & from, const Unit & to) {
    if (from.dim != to.dim) {
        return {};
    }
    if (std::fabs(value) > 1e15) {
        return {};
    }
    double out = 0;
    if (from.dim == kTemp) {
        const double kelvin = to_kelvin(value, from.mode);
        if (kelvin < 0 || kelvin > 1e7) {
            return {};
        }
        out = from_kelvin(kelvin, to.mode);
    } else if (to.to_si == 0) {
        return {};
    } else {
        out = value * from.to_si / to.to_si;
    }
    std::string text = format_num(value) + " " + from.label + " = " + format_num(out) + " " + to.label + ".";
    if (from.dim == kData && (from.bytes != to.bytes)) {
        text += " KB, MB, GB and TB are powers of 1000. KiB, MiB, GiB and TiB are powers of 1024.";
    }
    if (from.dim == kVolume && (std::strstr(from.label, "US") || std::strstr(to.label, "US") ||
                                 std::strstr(from.label, "imperial") || std::strstr(to.label, "imperial"))) {
        text += " A bare gallon, quart, pint, cup or fluid ounce here is US liquid. An imperial gallon is 4.54609 L.";
    }
    if (from.dim == kEnergy && (std::strcmp(from.key, "cal") == 0 || std::strcmp(to.key, "cal") == 0 ||
                                 std::strcmp(from.key, "kcal") == 0 || std::strcmp(to.key, "kcal") == 0)) {
        text += " A calorie here is the thermochemical calorie, 4.184 J.";
    }
    return text;
}

// --- calendar --------------------------------------------------------------

int days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int) doe - 719468;
}

void civil_from_days(int z, int * y, int * m, int * d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned) (z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int) yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    *y = year;
    *m = (int) month;
    *d = (int) day;
}

bool real_date(int y, int m, int d) {
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    int yy = 0, mm = 0, dd = 0;
    civil_from_days(days_from_civil(y, m, d), &yy, &mm, &dd);
    return yy == y && mm == m && dd == d;
}

int weekday_index(int days) {
    int w = (days + 4) % 7;
    if (w < 0) {
        w += 7;
    }
    return w;
}

const char * weekday_name(int days) {
    static const char * names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    return names[weekday_index(days)];
}

std::string iso_date(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

int month_number(const std::string & name) {
    static const char * full[] = { "january", "february", "march",     "april",   "may",      "june",
                                    "july",    "august",   "september", "october", "november", "december" };
    static const char * shortn[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    for (int i = 0; i < 12; ++i) {
        if (name == full[i] || name == shortn[i] || (i == 8 && name == "sept")) {
            return i + 1;
        }
    }
    return 0;
}

struct DateAt {
    int    y = 0;
    int    m = 0;
    int    d = 0;
    size_t begin = 0;
    size_t end = 0;
    bool   ok = false;
};

bool digits_at(const std::string & s, size_t i, int width, int * value, size_t * next) {
    if (i + (size_t) width > s.size()) {
        return false;
    }
    int v = 0;
    for (int k = 0; k < width; ++k) {
        const char c = s[i + (size_t) k];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *value = v;
    *next = i + (size_t) width;
    return true;
}

DateAt date_at(const std::string & s, size_t i) {
    DateAt got;
    if (i >= s.size()) {
        return got;
    }
    int y = 0, m = 0, d = 0;
    size_t next = i;
    if (digits_at(s, i, 4, &y, &next) && next < s.size() && s[next] == '-') {
        size_t m1 = next + 1;
        int width = 0;
        if (m1 < s.size() && s[m1] >= '0' && s[m1] <= '9') {
            width = (m1 + 1 < s.size() && s[m1 + 1] >= '0' && s[m1 + 1] <= '9') ? 2 : 1;
        }
        size_t after_m = m1;
        if (width && digits_at(s, m1, width, &m, &after_m) && after_m < s.size() && s[after_m] == '-') {
            size_t d1 = after_m + 1;
            int dwidth = 0;
            if (d1 < s.size() && s[d1] >= '0' && s[d1] <= '9') {
                dwidth = (d1 + 1 < s.size() && s[d1 + 1] >= '0' && s[d1 + 1] <= '9') ? 2 : 1;
            }
            size_t after_d = d1;
            if (dwidth && digits_at(s, d1, dwidth, &d, &after_d) && real_date(y, m, d)) {
                const bool left = i > 0 && word_char((unsigned char) s[i - 1]);
                const bool right = after_d < s.size() && word_char((unsigned char) s[after_d]);
                if (!left && !right) {
                    got = { y, m, d, i, after_d, true };
                    return got;
                }
            }
        }
    }
    if (std::isdigit((unsigned char) s[i])) {
        int day_width = 1;
        if (i + 1 < s.size() && std::isdigit((unsigned char) s[i + 1])) {
            day_width = 2;
        }
        int day = 0;
        size_t after_day = i;
        if (digits_at(s, i, day_width, &day, &after_day)) {
            size_t p = after_day;
            while (p < s.size() && (s[p] == ' ' || s[p] == ',')) {
                p += 1;
            }
            size_t month_end = p;
            while (month_end < s.size() && std::isalpha((unsigned char) s[month_end])) {
                month_end += 1;
            }
            const int month = p < month_end ? month_number(s.substr(p, month_end - p)) : 0;
            size_t q = month_end;
            while (q < s.size() && (s[q] == ' ' || s[q] == ',')) {
                q += 1;
            }
            int year = 0;
            size_t after_year = q;
            if (month && digits_at(s, q, 4, &year, &after_year) && real_date(year, month, day)) {
                const bool left = i > 0 && word_char((unsigned char) s[i - 1]);
                const bool right = after_year < s.size() && word_char((unsigned char) s[after_year]);
                if (!left && !right) {
                    got = { year, month, day, i, after_year, true };
                    return got;
                }
            }
        }
    }
    size_t end = i;
    while (end < s.size() && std::isalpha((unsigned char) s[end])) {
        end += 1;
    }
    if (end == i) {
        return got;
    }
    const int month = month_number(s.substr(i, end - i));
    if (!month) {
        return got;
    }
    size_t p = end;
    while (p < s.size() && (s[p] == ' ' || s[p] == ',')) {
        p += 1;
    }
    int day = 0;
    size_t after_day = p;
    int day_width = 0;
    if (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        day_width = (p + 1 < s.size() && s[p + 1] >= '0' && s[p + 1] <= '9') ? 2 : 1;
    }
    if (!day_width || !digits_at(s, p, day_width, &day, &after_day)) {
        return got;
    }
    size_t q = after_day;
    while (q < s.size() && (s[q] == ' ' || s[q] == ',')) {
        q += 1;
    }
    int year = 0;
    size_t after_year = q;
    if (!digits_at(s, q, 4, &year, &after_year) || !real_date(year, month, day)) {
        return got;
    }
    const bool left = i > 0 && word_char((unsigned char) s[i - 1]);
    const bool right = after_year < s.size() && word_char((unsigned char) s[after_year]);
    if (left || right) {
        return got;
    }
    got = { year, month, day, i, after_year, true };
    return got;
}

std::vector<DateAt> find_dates(const std::string & s) {
    std::vector<DateAt> dates;
    for (size_t i = 0; i < s.size();) {
        const DateAt one = date_at(s, i);
        if (one.ok) {
            dates.push_back(one);
            i = one.end;
            continue;
        }
        i += 1;
    }
    return dates;
}

bool inside_date(const std::vector<DateAt> & dates, size_t at) {
    for (const auto & one : dates) {
        if (at >= one.begin && at < one.end) {
            return true;
        }
    }
    return false;
}

std::string weekday_text(int y, int m, int d) {
    const int days = days_from_civil(y, m, d);
    return iso_date(y, m, d) + " is a " + weekday_name(days) + ".";
}

std::string between_text(int y1, int m1, int d1, int y2, int m2, int d2) {
    const int a = days_from_civil(y1, m1, d1);
    const int b = days_from_civil(y2, m2, d2);
    const int span = std::abs(b - a);
    const std::string left = iso_date(y1, m1, d1);
    const std::string right = iso_date(y2, m2, d2);
    if (span == 1) {
        return "There is 1 day from " + left + " to " + right + ".";
    }
    return "There are " + std::to_string(span) + " days from " + left + " to " + right + ".";
}

std::string add_text(int y, int m, int d, int delta) {
    if (delta > 365000 || delta < -365000) {
        return {};
    }
    const int days = days_from_civil(y, m, d);
    if (days > std::numeric_limits<int>::max() - 400000 || days < std::numeric_limits<int>::min() + 400000) {
        return {};
    }
    int yy = 0, mm = 0, dd = 0;
    civil_from_days(days + delta, &yy, &mm, &dd);
    if (!real_date(yy, mm, dd)) {
        return {};
    }
    const std::string when = delta < 0 ? std::to_string(-delta) + " days before " : std::to_string(delta) + " days after ";
    return when + iso_date(y, m, d) + " is " + iso_date(yy, mm, dd) + ", a " + weekday_name(days + delta) + ".";
}

// --- constants, elements, HTTP, prefixes ----------------------------------

struct Constant {
    const char * phrase;
    const char * text;
};

const Constant * constants(size_t * count) {
    static const Constant table[] = {
        { "pi", "pi is 3.141592653589793." },
        { "golden ratio", "The golden ratio phi is 1.618033988749895." },
        { "euler", "Euler's number e is 2.718281828459045." },
        { "square root of 2", "The square root of 2 is 1.414213562373095." },
        { "speed of light", "The speed of light is 299792458 m/s. That value is exact." },
        { "standard gravity", "Standard gravity is 9.80665 m/s^2. That value is exact." },
        { "gravitational constant", "The gravitational constant G is 6.67430e-11 m^3/(kg s^2). That is a measured value, not an exact one." },
        { "planck constant", "Planck's constant h is 6.62607015e-34 J s. That value is exact." },
        { "boltzmann", "Boltzmann's constant k is 1.380649e-23 J/K. That value is exact." },
        { "avogadro", "Avogadro's number is 6.02214076e23 per mole. That value is exact." },
        { "elementary charge", "The elementary charge is 1.602176634e-19 C. That value is exact." },
        { "gas constant", "The gas constant R is 8.314462618 J/(mol K)." },
        { "stefan-boltzmann", "The Stefan-Boltzmann constant is 5.670374419e-8 W/(m^2 K^4)." },
        { "stefan boltzmann", "The Stefan-Boltzmann constant is 5.670374419e-8 W/(m^2 K^4)." },
        { "astronomical unit", "An astronomical unit is 149597870700 m. That value is exact." },
        { "light year", "A light year is 9460730472580800 m. That uses a Julian year of 365.25 days." },
        { "light-year", "A light year is 9460730472580800 m. That uses a Julian year of 365.25 days." },
        { "parsec", "A parsec is about 3.085677581e16 m." },
        { "earth radius", "The mean Earth radius used here is 6371000 m." },
        { "radius of the earth", "The mean Earth radius used here is 6371000 m." },
        { "mass of the earth", "The mass of the Earth used here is 5.9722e24 kg." },
        { "earth mass", "The mass of the Earth used here is 5.9722e24 kg." },
        { "mass of the sun", "The mass of the Sun used here is 1.9885e30 kg." },
        { "sun mass", "The mass of the Sun used here is 1.9885e30 kg." },
        { "absolute zero", "Absolute zero is -273.15 °C, which is 0 K." },
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

struct Element {
    int         z;
    const char * symbol;
    const char * name;
    const char * mass;
};

const Element * elements(size_t * count) {
    static const Element table[] = {
        { 1, "H", "hydrogen", "1.008" },
        { 2, "He", "helium", "4.0026" },
        { 3, "Li", "lithium", "6.94" },
        { 4, "Be", "beryllium", "9.0122" },
        { 5, "B", "boron", "10.81" },
        { 6, "C", "carbon", "12.011" },
        { 7, "N", "nitrogen", "14.007" },
        { 8, "O", "oxygen", "15.999" },
        { 9, "F", "fluorine", "18.998" },
        { 10, "Ne", "neon", "20.180" },
        { 11, "Na", "sodium", "22.990" },
        { 12, "Mg", "magnesium", "24.305" },
        { 13, "Al", "aluminum", "26.982" },
        { 14, "Si", "silicon", "28.085" },
        { 15, "P", "phosphorus", "30.974" },
        { 16, "S", "sulfur", "32.06" },
        { 17, "Cl", "chlorine", "35.45" },
        { 18, "Ar", "argon", "39.948" },
        { 19, "K", "potassium", "39.098" },
        { 20, "Ca", "calcium", "40.078" },
        { 21, "Sc", "scandium", "44.956" },
        { 22, "Ti", "titanium", "47.867" },
        { 23, "V", "vanadium", "50.942" },
        { 24, "Cr", "chromium", "51.996" },
        { 25, "Mn", "manganese", "54.938" },
        { 26, "Fe", "iron", "55.845" },
        { 27, "Co", "cobalt", "58.933" },
        { 28, "Ni", "nickel", "58.693" },
        { 29, "Cu", "copper", "63.546" },
        { 30, "Zn", "zinc", "65.38" },
        { 31, "Ga", "gallium", "69.723" },
        { 32, "Ge", "germanium", "72.630" },
        { 33, "As", "arsenic", "74.922" },
        { 34, "Se", "selenium", "78.971" },
        { 35, "Br", "bromine", "79.904" },
        { 36, "Kr", "krypton", "83.798" },
        { 37, "Rb", "rubidium", "85.468" },
        { 38, "Sr", "strontium", "87.62" },
        { 39, "Y", "yttrium", "88.906" },
        { 40, "Zr", "zirconium", "91.224" },
        { 41, "Nb", "niobium", "92.906" },
        { 42, "Mo", "molybdenum", "95.95" },
        { 43, "Tc", "technetium", nullptr },
        { 44, "Ru", "ruthenium", "101.07" },
        { 45, "Rh", "rhodium", "102.91" },
        { 46, "Pd", "palladium", "106.42" },
        { 47, "Ag", "silver", "107.87" },
        { 48, "Cd", "cadmium", "112.41" },
        { 49, "In", "indium", "114.82" },
        { 50, "Sn", "tin", "118.71" },
        { 51, "Sb", "antimony", "121.76" },
        { 52, "Te", "tellurium", "127.60" },
        { 53, "I", "iodine", "126.90" },
        { 54, "Xe", "xenon", "131.29" },
        { 55, "Cs", "cesium", "132.91" },
        { 56, "Ba", "barium", "137.33" },
        { 57, "La", "lanthanum", "138.91" },
        { 58, "Ce", "cerium", "140.12" },
        { 59, "Pr", "praseodymium", "140.91" },
        { 60, "Nd", "neodymium", "144.24" },
        { 61, "Pm", "promethium", nullptr },
        { 62, "Sm", "samarium", "150.36" },
        { 63, "Eu", "europium", "151.96" },
        { 64, "Gd", "gadolinium", "157.25" },
        { 65, "Tb", "terbium", "158.93" },
        { 66, "Dy", "dysprosium", "162.50" },
        { 67, "Ho", "holmium", "164.93" },
        { 68, "Er", "erbium", "167.26" },
        { 69, "Tm", "thulium", "168.93" },
        { 70, "Yb", "ytterbium", "173.05" },
        { 71, "Lu", "lutetium", "174.97" },
        { 72, "Hf", "hafnium", "178.49" },
        { 73, "Ta", "tantalum", "180.95" },
        { 74, "W", "tungsten", "183.84" },
        { 75, "Re", "rhenium", "186.21" },
        { 76, "Os", "osmium", "190.23" },
        { 77, "Ir", "iridium", "192.22" },
        { 78, "Pt", "platinum", "195.08" },
        { 79, "Au", "gold", "196.97" },
        { 80, "Hg", "mercury", "200.59" },
        { 81, "Tl", "thallium", "204.38" },
        { 82, "Pb", "lead", "207.2" },
        { 83, "Bi", "bismuth", "208.98" },
        { 84, "Po", "polonium", nullptr },
        { 85, "At", "astatine", nullptr },
        { 86, "Rn", "radon", nullptr },
        { 87, "Fr", "francium", nullptr },
        { 88, "Ra", "radium", nullptr },
        { 89, "Ac", "actinium", nullptr },
        { 90, "Th", "thorium", "232.04" },
        { 91, "Pa", "protactinium", "231.04" },
        { 92, "U", "uranium", "238.03" },
        { 93, "Np", "neptunium", nullptr },
        { 94, "Pu", "plutonium", nullptr },
        { 95, "Am", "americium", nullptr },
        { 96, "Cm", "curium", nullptr },
        { 97, "Bk", "berkelium", nullptr },
        { 98, "Cf", "californium", nullptr },
        { 99, "Es", "einsteinium", nullptr },
        { 100, "Fm", "fermium", nullptr },
        { 101, "Md", "mendelevium", nullptr },
        { 102, "No", "nobelium", nullptr },
        { 103, "Lr", "lawrencium", nullptr },
        { 104, "Rf", "rutherfordium", nullptr },
        { 105, "Db", "dubnium", nullptr },
        { 106, "Sg", "seaborgium", nullptr },
        { 107, "Bh", "bohrium", nullptr },
        { 108, "Hs", "hassium", nullptr },
        { 109, "Mt", "meitnerium", nullptr },
        { 110, "Ds", "darmstadtium", nullptr },
        { 111, "Rg", "roentgenium", nullptr },
        { 112, "Cn", "copernicium", nullptr },
        { 113, "Nh", "nihonium", nullptr },
        { 114, "Fl", "flerovium", nullptr },
        { 115, "Mc", "moscovium", nullptr },
        { 116, "Lv", "livermorium", nullptr },
        { 117, "Ts", "tennessine", nullptr },
        { 118, "Og", "oganesson", nullptr },
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

std::string element_name_key(const std::string & name) {
    if (name == "aluminium") {
        return "aluminum";
    }
    if (name == "caesium") {
        return "cesium";
    }
    if (name == "sulphur") {
        return "sulfur";
    }
    return name;
}

std::string element_text(const Element & one) {
    std::string text = std::string(one.name);
    text[0] = (char) std::toupper((unsigned char) text[0]);
    text += " is ";
    text += one.symbol;
    text += ", element ";
    text += std::to_string(one.z);
    text += ".";
    if (one.mass && one.mass[0]) {
        text += " The conventional atomic weight stored here is ";
        text += one.mass;
        text += ".";
    } else {
        text += " This table has no standard atomic weight for it.";
    }
    return text;
}

const Element * find_element(const std::string & raw) {
    const std::string name = element_name_key(unit_key(raw));
    if (name.empty()) {
        return nullptr;
    }
    size_t count = 0;
    const Element * table = elements(&count);
    for (size_t i = 0; i < count; ++i) {
        if (name == table[i].name) {
            return &table[i];
        }
        std::string symbol = table[i].symbol;
        for (char & c : symbol) {
            c = (char) std::tolower((unsigned char) c);
        }
        if (name == symbol) {
            return &table[i];
        }
    }
    char * end = nullptr;
    const long z = std::strtol(name.c_str(), &end, 10);
    if (end && *end == '\0' && z >= 1 && z <= (long) count) {
        return &table[z - 1];
    }
    return nullptr;
}

struct Status {
    int         code;
    const char * text;
};

const Status * statuses(size_t * count) {
    static const Status table[] = {
        { 100, "Continue" },
        { 101, "Switching Protocols" },
        { 103, "Early Hints" },
        { 200, "OK" },
        { 201, "Created" },
        { 202, "Accepted" },
        { 204, "No Content" },
        { 206, "Partial Content" },
        { 301, "Moved Permanently" },
        { 302, "Found" },
        { 303, "See Other" },
        { 304, "Not Modified" },
        { 307, "Temporary Redirect" },
        { 308, "Permanent Redirect" },
        { 400, "Bad Request" },
        { 401, "Unauthorized" },
        { 403, "Forbidden" },
        { 404, "Not Found" },
        { 405, "Method Not Allowed" },
        { 408, "Request Timeout" },
        { 409, "Conflict" },
        { 410, "Gone" },
        { 413, "Content Too Large" },
        { 414, "URI Too Long" },
        { 415, "Unsupported Media Type" },
        { 418, "I'm a teapot" },
        { 422, "Unprocessable Content" },
        { 429, "Too Many Requests" },
        { 431, "Request Header Fields Too Large" },
        { 451, "Unavailable For Legal Reasons" },
        { 500, "Internal Server Error" },
        { 501, "Not Implemented" },
        { 502, "Bad Gateway" },
        { 503, "Service Unavailable" },
        { 504, "Gateway Timeout" },
        { 505, "HTTP Version Not Supported" },
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

const Status * find_status(int code) {
    size_t count = 0;
    const Status * table = statuses(&count);
    for (size_t i = 0; i < count; ++i) {
        if (table[i].code == code) {
            return &table[i];
        }
    }
    return nullptr;
}

std::string status_text(const Status & one) {
    return "HTTP " + std::to_string(one.code) + " is " + one.text + ".";
}

struct Prefix {
    const char * name;
    const char * symbol;
    int          exp10;
};

const Prefix * prefixes(size_t * count) {
    static const Prefix table[] = {
        { "quetta", "Q", 30 }, { "ronna", "R", 27 }, { "yotta", "Y", 24 }, { "zetta", "Z", 21 },
        { "exa", "E", 18 },    { "peta", "P", 15 },  { "tera", "T", 12 },   { "giga", "G", 9 },
        { "mega", "M", 6 },    { "kilo", "k", 3 },   { "hecto", "h", 2 },   { "deca", "da", 1 },
        { "deci", "d", -1 },   { "centi", "c", -2 }, { "milli", "m", -3 },  { "micro", "u", -6 },
        { "nano", "n", -9 },   { "pico", "p", -12 }, { "femto", "f", -15 }, { "atto", "a", -18 },
        { "zepto", "z", -21 }, { "yocto", "y", -24 }, { "ronto", "r", -27 }, { "quecto", "q", -30 },
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

const Prefix * find_prefix(const std::string & raw) {
    const std::string key = unit_key(raw);
    size_t count = 0;
    const Prefix * table = prefixes(&count);
    for (size_t i = 0; i < count; ++i) {
        if (key == table[i].name) {
            return &table[i];
        }
    }
    return nullptr;
}

std::string prefix_text(const Prefix & one) {
    std::string text = std::string(one.name) + " is 10^";
    text += std::to_string(one.exp10);
    text += ", symbol ";
    text += one.symbol;
    text += ".";
    return text;
}

// --- text codecs -----------------------------------------------------------

std::string base64_encode(const std::string & in) {
    static const char * alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    unsigned value = 0;
    int bits = 0;
    for (unsigned char c : in) {
        value = (value << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(value >> bits) & 63]);
        }
    }
    if (bits > 0) {
        out.push_back(alphabet[(value << (6 - bits)) & 63]);
    }
    while (out.size() % 4) {
        out.push_back('=');
    }
    return out;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool base64_decode(const std::string & in, std::string * out) {
    std::string text;
    text.reserve(in.size());
    for (unsigned char c : in) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        text.push_back((char) c);
    }
    if (text.size() % 4 != 0) {
        return false;
    }
    out->clear();
    for (size_t i = 0; i < text.size(); i += 4) {
        int pad = 0;
        int values[4];
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + (size_t) k];
            if (c == '=') {
                values[k] = 0;
                pad += 1;
                if (k < 2) {
                    return false;
                }
            } else {
                if (pad) {
                    return false;
                }
                values[k] = b64_value(c);
                if (values[k] < 0) {
                    return false;
                }
            }
        }
        const unsigned triple = ((unsigned) values[0] << 18) | ((unsigned) values[1] << 12) | ((unsigned) values[2] << 6) |
                                (unsigned) values[3];
        out->push_back((char) ((triple >> 16) & 0xFF));
        if (pad < 2) {
            out->push_back((char) ((triple >> 8) & 0xFF));
        }
        if (pad < 1) {
            out->push_back((char) (triple & 0xFF));
        }
    }
    return true;
}

std::string hex_encode(const std::string & in) {
    static const char * hex = "0123456789abcdef";
    std::string out;
    out.resize(in.size() * 2);
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = (unsigned char) in[i];
        out[i * 2] = hex[c >> 4];
        out[i * 2 + 1] = hex[c & 15];
    }
    return out;
}

uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

std::string sha256_hex(const std::string & in) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    std::vector<unsigned char> buf(in.begin(), in.end());
    buf.push_back(0x80);
    while ((buf.size() % 64) != 56) {
        buf.push_back(0);
    }
    const uint64_t bits = (uint64_t) in.size() * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back((unsigned char) ((bits >> shift) & 0xFF));
    }
    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const size_t at = off + (size_t) i * 4;
            w[i] = ((uint32_t) buf[at] << 24) | ((uint32_t) buf[at + 1] << 16) | ((uint32_t) buf[at + 2] << 8) |
                   (uint32_t) buf[at + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }
    const uint32_t words[] = { h0, h1, h2, h3, h4, h5, h6, h7 };
    std::string hex;
    hex.resize(64);
    static const char * digits = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            const unsigned char byte = (unsigned char) ((words[i] >> (24 - 8 * b)) & 0xFF);
            hex[(size_t) i * 8 + (size_t) b * 2] = digits[byte >> 4];
            hex[(size_t) i * 8 + (size_t) b * 2 + 1] = digits[byte & 15];
        }
    }
    return hex;
}

size_t code_points(const std::string & text) {
    size_t n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            n += 1;
        }
    }
    return n;
}

std::string count_text(const std::string & text) {
    size_t words = 0;
    size_t lines = text.empty() ? 0 : 1;
    bool in_word = false;
    for (unsigned char c : text) {
        if (c == '\n') {
            lines += 1;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words += 1;
        }
    }
    return std::to_string(code_points(text)) + " characters, " + std::to_string(text.size()) + " bytes, " +
           std::to_string(words) + " words, " + std::to_string(lines) + " lines.";
}

bool text_ok(const std::string & text, std::string * error) {
    if (text.size() > kTextCap) {
        *error = "That text is longer than 8000 characters.";
        return false;
    }
    return true;
}

// --- question notes --------------------------------------------------------

struct Tok {
    bool        number = false;
    double      value = 0;
    std::string text;
    size_t      begin = 0;
};

std::vector<Tok> tokens_of(const std::string & q) {
    std::vector<Tok> out;
    for (size_t i = 0; i < q.size();) {
        const unsigned char c = (unsigned char) q[i];
        if (std::isdigit(c) || (c == '.' && i + 1 < q.size() && std::isdigit((unsigned char) q[i + 1]))) {
            const size_t begin = i;
            char * end = nullptr;
            const double value = std::strtod(q.c_str() + i, &end);
            if (end && end != q.c_str() + i) {
                Tok tok;
                tok.number = true;
                tok.value = value;
                tok.begin = begin;
                tok.text = q.substr(begin, (size_t) (end - (q.c_str() + i)));
                out.push_back(tok);
                i = (size_t) (end - q.c_str());
                continue;
            }
        }
        if (std::isalpha(c)) {
            const size_t begin = i;
            size_t end = i + 1;
            while (end < q.size()) {
                const unsigned char n = (unsigned char) q[end];
                if (std::isalpha(n) || n == '/') {
                    end += 1;
                    continue;
                }
                break;
            }
            Tok tok;
            tok.begin = begin;
            tok.text = q.substr(begin, end - begin);
            out.push_back(tok);
            i = end;
            continue;
        }
        i += 1;
    }
    return out;
}

bool short_temp_key(const std::string & key) { return key == "c" || key == "f" || key == "k"; }

bool glue_key(const std::string & key) {
    return key == "fl" || key == "fluid" || key == "square" || key == "cubic" || key == "short" || key == "long" ||
           key == "metric" || key == "us" || key == "imperial" || key == "uk" || key == "nautical";
}

const Unit * unit_from_tokens(const std::vector<Tok> & toks, size_t index, size_t * used) {
    if (index >= toks.size() || toks[index].number) {
        return nullptr;
    }
    const std::string key = unit_key(toks[index].text);
    if (glue_key(key) && index + 1 < toks.size() && !toks[index + 1].number) {
        const Unit * both = find_unit(key + toks[index + 1].text);
        if (both) {
            *used = 2;
            return both;
        }
    }
    const Unit * one = find_unit(key);
    if (!one) {
        return nullptr;
    }
    *used = 1;
    return one;
}

bool temp_pair_ok(const Unit & from, const Unit & to) {
    if (from.dim != kTemp) {
        return true;
    }
    const bool from_short = short_temp_key(from.key);
    const bool to_short = short_temp_key(to.key);
    if (!from_short && !to_short) {
        return true;
    }
    return from.dim == to.dim;
}

std::string conversion_note(const std::string & q) {
    const std::vector<Tok> toks = tokens_of(q);
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!toks[i].number || !std::isfinite(toks[i].value)) {
            continue;
        }
        size_t at = i + 1;
        if (at < toks.size() && (toks[at].text == "degree" || toks[at].text == "degrees")) {
            at += 1;
        }
        size_t used = 0;
        const Unit * from = unit_from_tokens(toks, at, &used);
        if (!from) {
            continue;
        }
        at += used;
        if (at < toks.size() && (toks[at].text == "to" || toks[at].text == "into" || toks[at].text == "in")) {
            at += 1;
            if (at < toks.size() && (toks[at].text == "degree" || toks[at].text == "degrees")) {
                at += 1;
            }
            size_t used_to = 0;
            const Unit * to = unit_from_tokens(toks, at, &used_to);
            if (to && temp_pair_ok(*from, *to)) {
                const std::string text = convert_text(toks[i].value, *from, *to);
                if (!text.empty()) {
                    return text;
                }
            }
        }
    }
    size_t target_at = std::string::npos;
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if ((toks[i].text == "how" || toks[i].text == "many") && toks[i + 1].text == "many") {
            continue;
        }
        if (toks[i].text == "many" || toks[i].text == "much") {
            target_at = i + 1;
            break;
        }
    }
    if (target_at == std::string::npos) {
        return {};
    }
    if (target_at < toks.size() && (toks[target_at].text == "degree" || toks[target_at].text == "degrees")) {
        target_at += 1;
    }
    size_t used_to = 0;
    const Unit * to = unit_from_tokens(toks, target_at, &used_to);
    if (!to) {
        return {};
    }
    for (size_t i = target_at + used_to; i < toks.size(); ++i) {
        if (!toks[i].number) {
            continue;
        }
        size_t at = i + 1;
        if (at < toks.size() && (toks[at].text == "degree" || toks[at].text == "degrees")) {
            at += 1;
        }
        size_t used = 0;
        const Unit * from = unit_from_tokens(toks, at, &used);
        if (from && temp_pair_ok(*from, *to)) {
            const std::string text = convert_text(toks[i].value, *from, *to);
            if (!text.empty()) {
                return text;
            }
        }
    }
    return {};
}

std::string calendar_note(const std::string & q) {
    const std::vector<DateAt> dates = find_dates(q);
    if (dates.empty()) {
        return {};
    }
    if (dates.size() >= 2) {
        return between_text(dates[0].y, dates[0].m, dates[0].d, dates[1].y, dates[1].m, dates[1].d);
    }
    int delta = 0;
    bool saw = false;
    bool negative = has_word(q, "before") || has_word(q, "ago");
    for (size_t i = 0; i < q.size();) {
        if (inside_date(dates, i) || !std::isdigit((unsigned char) q[i])) {
            i += 1;
            continue;
        }
        char * end = nullptr;
        const long value = std::strtol(q.c_str() + i, &end, 10);
        if (!end || end == q.c_str() + i) {
            i += 1;
            continue;
        }
        const size_t at = i;
        i = (size_t) (end - q.c_str());
        if (value > 365000) {
            continue;
        }
        const bool day_word = has_word(q, "day") || has_word(q, "days");
        if (!day_word) {
            continue;
        }
        if (!saw) {
            delta = (int) value;
            saw = true;
            (void) at;
        }
    }
    if (saw && dates.size() == 1 && (negative || has_word(q, "after") || has_word(q, "later") || has_word(q, "plus"))) {
        return add_text(dates[0].y, dates[0].m, dates[0].d, negative ? -delta : delta);
    }
    if (dates.size() == 1 && (has_word(q, "weekday") || has_word(q, "week") || has_word(q, "day"))) {
        return weekday_text(dates[0].y, dates[0].m, dates[0].d);
    }
    return {};
}

std::string http_note(const std::string & q) {
    if (!has_word(q, "http") && !has_word(q, "status")) {
        return {};
    }
    for (size_t i = 0; i < q.size();) {
        if (!std::isdigit((unsigned char) q[i])) {
            i += 1;
            continue;
        }
        char * end = nullptr;
        const long code = std::strtol(q.c_str() + i, &end, 10);
        if (!end || end == q.c_str() + i) {
            break;
        }
        const bool left = i > 0 && word_char((unsigned char) q[i - 1]);
        const bool right = word_char((unsigned char) *end);
        i = (size_t) (end - q.c_str());
        if (left || right || code < 100 || code > 599) {
            continue;
        }
        const Status * one = find_status((int) code);
        if (one) {
            return status_text(*one);
        }
        return "HTTP " + std::to_string(code) + " is not in the status table on this console.";
    }
    return {};
}

std::string element_note(const std::string & q) {
    if (!has_word(q, "element") && !has_word(q, "atomic") && !has_word(q, "symbol")) {
        return {};
    }
    size_t count = 0;
    const Element * table = elements(&count);
    for (size_t i = 0; i < count; ++i) {
        if (has_word(q, table[i].name)) {
            return element_text(table[i]);
        }
    }
    if (has_word(q, "aluminium")) {
        return element_text(*find_element("aluminum"));
    }
    if (has_word(q, "caesium")) {
        return element_text(*find_element("cesium"));
    }
    if (has_word(q, "sulphur")) {
        return element_text(*find_element("sulfur"));
    }
    // One-letter symbols, and symbols that are ordinary words, match only
    // when the question says "symbol". "no" would otherwise become nobelium.
    static const char * stop[] = { "he", "be", "as", "at", "in", "no", "am", "al", "i",  "c",  "k", "o",
                                    "u",  "v",  "y",  "w",  "b",  "f",  "h",  "n",  "p",  "s" };
    const bool symbol_asked = has_word(q, "symbol");
    for (size_t i = 0; i < count; ++i) {
        std::string symbol = table[i].symbol;
        for (char & c : symbol) {
            c = (char) std::tolower((unsigned char) c);
        }
        if (!has_word(q, symbol.c_str())) {
            continue;
        }
        bool common = false;
        for (const char * word : stop) {
            if (symbol == word) {
                common = true;
                break;
            }
        }
        if (symbol_asked || !common) {
            return element_text(table[i]);
        }
    }
    for (size_t i = 0; i < q.size();) {
        if (!std::isdigit((unsigned char) q[i])) {
            i += 1;
            continue;
        }
        char * end = nullptr;
        const long z = std::strtol(q.c_str() + i, &end, 10);
        if (end && end != q.c_str() + i && z >= 1 && z <= (long) count) {
            const bool left = i > 0 && word_char((unsigned char) q[i - 1]);
            const bool right = word_char((unsigned char) *end);
            if (!left && !right) {
                return element_text(table[z - 1]);
            }
        }
        i = end ? (size_t) (end - q.c_str()) : i + 1;
    }
    return {};
}

std::string constant_note(const std::string & q) {
    size_t count = 0;
    const Constant * table = constants(&count);
    const Constant * best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!has_word(q, table[i].phrase)) {
            continue;
        }
        const size_t len = std::strlen(table[i].phrase);
        if (len > best_len) {
            best = &table[i];
            best_len = len;
        }
    }
    return best ? best->text : std::string();
}

std::string prefix_note(const std::string & q) {
    if (!has_word(q, "prefix")) {
        return {};
    }
    size_t count = 0;
    const Prefix * table = prefixes(&count);
    for (size_t i = 0; i < count; ++i) {
        if (has_word(q, table[i].name)) {
            return prefix_text(table[i]);
        }
    }
    return {};
}

std::string quoted_or_rest(const std::string & q, const char * marker) {
    const size_t at = q.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    size_t i = at + std::strlen(marker);
    while (i < q.size() && q[i] == ' ') {
        i += 1;
    }
    if (i >= q.size()) {
        return {};
    }
    if (q[i] == '"' || q[i] == '\'') {
        const char quote = q[i];
        const size_t end = q.find(quote, i + 1);
        if (end == std::string::npos) {
            return {};
        }
        return q.substr(i + 1, end - i - 1);
    }
    size_t end = i;
    while (end < q.size() && q[end] != ' ' && q[end] != '?' && q[end] != '.') {
        end += 1;
    }
    return q.substr(i, end - i);
}

size_t find_ci(const std::string & text, const char * marker) {
    const size_t n = std::strlen(marker);
    if (n == 0 || text.size() < n) {
        return std::string::npos;
    }
    for (size_t i = 0; i + n <= text.size(); ++i) {
        bool same = true;
        for (size_t k = 0; k < n; ++k) {
            const char a = (char) std::tolower((unsigned char) text[i + k]);
            const char b = (char) std::tolower((unsigned char) marker[k]);
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            return i;
        }
    }
    return std::string::npos;
}

std::string payload_after(const std::string & original, const char * marker) {
    const size_t at = find_ci(original, marker);
    if (at == std::string::npos) {
        return {};
    }
    return quoted_or_rest(original, original.substr(at, std::strlen(marker)).c_str());
}

std::string text_note(const std::string & folded, const std::string & original) {
    const bool sha = folded.find("sha-256") != std::string::npos || folded.find("sha256") != std::string::npos;
    if (sha) {
        std::string payload = payload_after(original, "sha-256 of ");
        if (payload.empty()) {
            payload = payload_after(original, "sha256 of ");
        }
        if (payload.empty() || payload.size() > kTextCap) {
            return {};
        }
        return "SHA-256 of that text is " + sha256_hex(payload) + ".";
    }
    if (folded.find("base64") == std::string::npos) {
        return {};
    }
    const bool decode = has_word(folded, "decode") || folded.find("unbase64") != std::string::npos;
    std::string payload = payload_after(original, decode ? "base64 decode " : "base64 encode ");
    if (payload.empty()) {
        payload = payload_after(original, "base64 ");
    }
    if (payload.empty() || payload.size() > kTextCap) {
        return {};
    }
    if (decode) {
        std::string out;
        if (!base64_decode(payload, &out)) {
            return "That is not base64.";
        }
        return "Base64 decoded is " + out + ".";
    }
    return "Base64 of that text is " + base64_encode(payload) + ".";
}

std::string do_convert(const nlohmann::json & j) {
    double value = 0;
    if (!json_number(j, "value", &value)) {
        return fail("Say the number to convert.");
    }
    const Unit * from = find_unit(json_string(j, "from"));
    const Unit * to = find_unit(json_string(j, "to"));
    if (!from || !to) {
        return fail("Name both units. Length, mass, temperature, volume, area, speed, data size, time, pressure and energy are stored here.");
    }
    if (!temp_pair_ok(*from, *to) && from->dim == kTemp) {
        return fail("Use celsius, fahrenheit or kelvin for temperature.");
    }
    if (from->dim != to->dim) {
        return fail(std::string(from->label) + " is " + dim_name(from->dim) + " and " + to->label + " is " +
                    dim_name(to->dim) + ". Those are not the same kind.");
    }
    const std::string text = convert_text(value, *from, *to);
    if (text.empty()) {
        return fail("That number is outside the range this table converts.");
    }
    return ok_text(text);
}

std::string do_calendar(const nlohmann::json & j) {
    std::string op = unit_key(json_string(j, "op"));
    if (op == "week" || op == "day") {
        op = "weekday";
    }
    if (op == "diff" || op == "difference") {
        op = "between";
    }
    if (op == "plus") {
        op = "add";
    }
    const std::string date = lower_copy(json_string(j, "date"));
    const std::vector<DateAt> dates = find_dates(date);
    if (dates.size() != 1 || dates[0].begin != 0) {
        return fail("Use a real date, as YYYY-MM-DD or 18 October 2026.");
    }
    const DateAt from = dates[0];
    if (date.size() != from.end && unit_key(date.substr(from.end)).size() > 0) {
        return fail("Use a real date, as YYYY-MM-DD or 18 October 2026.");
    }
    if (op == "weekday") {
        return ok_text(weekday_text(from.y, from.m, from.d));
    }
    if (op == "between") {
        const std::string end = lower_copy(json_string(j, "end"));
        const std::vector<DateAt> ends = find_dates(end);
        if (ends.size() != 1) {
            return fail("Name the second date.");
        }
        return ok_text(between_text(from.y, from.m, from.d, ends[0].y, ends[0].m, ends[0].d));
    }
    if (op == "add") {
        double days = 0;
        if (!json_number(j, "days", &days) || std::fabs(days - std::round(days)) > 0.001) {
            return fail("Use a whole number of days.");
        }
        const std::string text = add_text(from.y, from.m, from.d, (int) std::round(days));
        if (text.empty()) {
            return fail("That date is outside year 1 to 9999.");
        }
        return ok_text(text);
    }
    return fail("op is weekday, between, or add.");
}

std::string do_lookup(const nlohmann::json & j) {
    const std::string kind = unit_key(json_string(j, "kind"));
    const std::string name = json_string(j, "name");
    if (kind == "constant") {
        const std::string note = constant_note(lower_copy(name));
        if (note.empty()) {
            return fail("That constant is not stored here. Try pi, the speed of light, standard gravity, or Avogadro.");
        }
        return ok_text(note);
    }
    if (kind == "element") {
        const Element * one = find_element(name);
        if (!one) {
            return fail("Name an element, its symbol, or its number from 1 to 118.");
        }
        return ok_text(element_text(*one));
    }
    if (kind == "http" || kind == "status") {
        double code = 0;
        if (!json_number(j, "code", &code)) {
            const std::string note = http_note("http status " + lower_copy(name));
            if (!note.empty()) {
                return ok_text(note);
            }
            return fail("Give the HTTP status code.");
        }
        const Status * one = find_status((int) std::round(code));
        if (!one) {
            return fail("That status code is not in the table on this console.");
        }
        return ok_text(status_text(*one));
    }
    if (kind == "prefix") {
        const Prefix * one = find_prefix(name);
        if (!one) {
            return fail("Name an SI prefix, such as nano or giga.");
        }
        return ok_text(prefix_text(*one));
    }
    return fail("kind is constant, element, http, or prefix.");
}

std::string do_text(const nlohmann::json & j) {
    const std::string op = unit_key(json_string(j, "op"));
    const std::string text = j.is_object() && j.contains("text") && j["text"].is_string() ? j["text"].get<std::string>()
                                                                                            : json_string(j, "text");
    std::string error;
    if (!text_ok(text, &error)) {
        return fail(error);
    }
    if (op == "base64" || op == "encode") {
        return ok_text(base64_encode(text));
    }
    if (op == "unbase64" || op == "base64decode" || op == "decode") {
        std::string out;
        if (!base64_decode(text, &out)) {
            return fail("That is not base64.");
        }
        return ok_text(out);
    }
    if (op == "hex") {
        return ok_text(hex_encode(text));
    }
    if (op == "sha256" || op == "hash") {
        return ok_text(sha256_hex(text));
    }
    if (op == "count") {
        return ok_text(count_text(text));
    }
    return fail("op is base64, unbase64, hex, sha256, or count.");
}

}  // namespace

std::string reference_note(const std::string & question) {
    const std::string q = lower_copy(question);
    if (q.empty() || q.size() > 500) {
        return {};
    }
    const std::string encoded = text_note(q, question);
    if (!encoded.empty()) {
        return encoded;
    }
    const std::string http = http_note(q);
    if (!http.empty()) {
        return http;
    }
    const std::string when = calendar_note(q);
    if (!when.empty()) {
        return when;
    }
    const std::string converted = conversion_note(q);
    if (!converted.empty()) {
        return converted;
    }
    if (live_topic(q)) {
        return {};
    }
    const std::string element = element_note(q);
    if (!element.empty()) {
        return element;
    }
    const std::string constant = constant_note(q);
    if (!constant.empty()) {
        return constant;
    }
    return prefix_note(q);
}

std::string reference_call(const std::string & body) {
    const nlohmann::json j = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return fail("The reference call was not JSON.");
    }
    std::string tool = unit_key(json_string(j, "tool"));
    if (tool == "convertunits") {
        tool = "convert";
    }
    if (tool == "textcodec") {
        tool = "text";
    }
    if (tool.empty() && j.contains("text")) {
        tool = "note";
    }
    if (tool == "note") {
        const std::string text = reference_note(json_string(j, "text"));
        if (text.empty()) {
            return ok_text("Nothing in this table matches that. Ask for a conversion, a date, a constant, an element, an HTTP status, or a short encoding.");
        }
        return ok_text(text);
    }
    if (tool == "convert") {
        return do_convert(j);
    }
    if (tool == "calendar") {
        return do_calendar(j);
    }
    if (tool == "lookup") {
        return do_lookup(j);
    }
    if (tool == "text") {
        return do_text(j);
    }
    return fail("That reference tool is not on this console. Use convert, calendar, lookup, text, or note.");
}

}  // namespace ps5lm
