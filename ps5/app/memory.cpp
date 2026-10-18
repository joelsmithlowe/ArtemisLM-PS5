// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ps5lm {

namespace {

constexpr int kMaxNotes = 80;
constexpr int kMaxText = 400;
constexpr int kDims = 32;
constexpr int kHitScore = 45;
constexpr int kForgetScore = 70;

std::mutex g_mu;

std::string fail(const std::string & message) {
    return nlohmann::json{ { "error", message } }.dump();
}

std::string trim_copy(std::string text) {
    size_t a = 0;
    while (a < text.size() && std::isspace((unsigned char) text[a])) {
        ++a;
    }
    size_t b = text.size();
    while (b > a && std::isspace((unsigned char) text[b - 1])) {
        --b;
    }
    return text.substr(a, b - a);
}

uint32_t fnv1a(const std::string & text) {
    uint32_t h = 2166136261u;
    for (unsigned char c : text) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

bool stopword(const std::string & word) {
    static const char * words[] = {
        "the",  "and",    "for",     "that",   "this",    "with",    "from",     "what",    "where",   "when",
        "who",  "why",    "how",     "you",    "your",    "our",     "are",      "was",     "were",    "have",
        "has",  "not",    "but",     "can",    "about",   "into",    "please",   "remember","today",   "tonight",
        "its",  "then",   "than",    "them",   "they",    "just",    "also",     "does",    "did",
    };
    for (const char * one : words) {
        if (word == one) {
            return true;
        }
    }
    return false;
}

// One count per hash bucket. A token adds +1 or -1. Words that share a
// bucket land in the same place, which is the match.
std::vector<int> embed(const std::string & text) {
    std::vector<int> v(kDims, 0);
    std::string word;
    std::string lower = text;
    for (char & c : lower) {
        c = (char) std::tolower((unsigned char) c);
    }
    auto take = [&]() {
        if (word.size() >= 3 && !stopword(word)) {
            const uint32_t h = fnv1a(word);
            const int dim = (int) (h % (uint32_t) kDims);
            const int sign = (h & 0x80000000u) ? -1 : 1;
            int next = v[(size_t) dim] + sign;
            if (next > 1000) {
                next = 1000;
            }
            if (next < -1000) {
                next = -1000;
            }
            v[(size_t) dim] = next;
        }
        word.clear();
    };
    for (unsigned char c : lower) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            word.push_back((char) c);
        } else {
            take();
        }
    }
    take();
    return v;
}

int cosine_score(const std::vector<int> & a, const std::vector<int> & b) {
    if (a.size() != (size_t) kDims || b.size() != (size_t) kDims) {
        return 0;
    }
    int64_t dot = 0;
    int64_t na = 0;
    int64_t nb = 0;
    for (int i = 0; i < kDims; ++i) {
        dot += (int64_t) a[(size_t) i] * b[(size_t) i];
        na += (int64_t) a[(size_t) i] * a[(size_t) i];
        nb += (int64_t) b[(size_t) i] * b[(size_t) i];
    }
    if (dot <= 0 || na <= 0 || nb <= 0) {
        return 0;
    }
    const double score = 100.0 * (double) dot / (std::sqrt((double) na) * std::sqrt((double) nb));
    if (score >= 100.0) {
        return 100;
    }
    return (int) std::lround(score);
}

struct Note {
    std::string id;
    std::string text;
    std::vector<int> v;
};

std::string notes_path(const std::string & root) {
    std::string base = root;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    return base + "/notes.json";
}

std::vector<int> read_vec(const nlohmann::json & j) {
    if (!j.is_array() || j.size() != (size_t) kDims) {
        return {};
    }
    std::vector<int> v(kDims, 0);
    for (int i = 0; i < kDims; ++i) {
        if (!j[(size_t) i].is_number_integer()) {
            return {};
        }
        int n = j[(size_t) i].get<int>();
        if (n > 1000) {
            n = 1000;
        }
        if (n < -1000) {
            n = -1000;
        }
        v[(size_t) i] = n;
    }
    return v;
}

std::string load_notes(const std::string & path, std::vector<Note> * notes) {
    std::error_code ec;
    if (std::filesystem::is_symlink(path, ec)) {
        return "That path is not allowed";
    }
    if (!std::filesystem::is_regular_file(path, ec)) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "The notes file could not be read";
    }
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("notes") || !doc["notes"].is_array()) {
        return "The notes file could not be read";
    }
    for (const auto & one : doc["notes"]) {
        if (!one.is_object() || !one.contains("text") || !one["text"].is_string()) {
            continue;
        }
        Note note;
        note.text = one["text"].get<std::string>();
        if (note.text.empty() || note.text.size() > (size_t) kMaxText) {
            continue;
        }
        note.id = one.value("id", std::string());
        if (note.id.size() != 8) {
            char buf[9];
            std::snprintf(buf, sizeof(buf), "%08x", fnv1a(note.text));
            note.id = buf;
        }
        note.v = one.contains("v") ? read_vec(one["v"]) : std::vector<int>();
        if (note.v.empty()) {
            note.v = embed(note.text);
        }
        notes->push_back(std::move(note));
        if ((int) notes->size() >= kMaxNotes) {
            break;
        }
    }
    return {};
}

std::string store_notes(const std::string & root, const std::string & path, const std::vector<Note> & notes) {
    std::error_code ec;
    if (std::filesystem::is_symlink(root, ec) || std::filesystem::is_symlink(path, ec)) {
        return "That path is not allowed";
    }
    std::filesystem::create_directories(root, ec);
    if (ec || !std::filesystem::is_directory(root, ec)) {
        return "Could not create the notes folder";
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto & note : notes) {
        arr.push_back({ { "id", note.id }, { "text", note.text }, { "v", note.v } });
    }
    const std::string body = nlohmann::json{ { "notes", std::move(arr) } }.dump();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "Could not write the notes file";
        }
        out.write(body.data(), (std::streamsize) body.size());
        if (!out) {
            return "Could not write the notes file";
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return "Could not write the notes file";
    }
    return {};
}

std::string make_id(const std::string & text, const std::vector<Note> & notes) {
    std::string basis = text;
    std::string id;
    for (int n = 0; n < 8; ++n) {
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", fnv1a(basis));
        id = buf;
        bool taken = false;
        for (const auto & note : notes) {
            if (note.id == id && note.text != text) {
                taken = true;
            }
        }
        if (!taken) {
            return id;
        }
        basis.push_back('*');
    }
    return id;
}

std::string field(const nlohmann::json & body, const char * key) {
    if (!body.contains(key) || !body[key].is_string()) {
        return {};
    }
    return body[key].get<std::string>();
}

}  // namespace

std::string memory_call(const std::string & root, const std::string & body) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (root.empty() || root.find("..") != std::string::npos) {
        return fail("The notes folder is not available");
    }
    const nlohmann::json json = nlohmann::json::parse(body.empty() ? "{}" : body, nullptr, false);
    if (!json.is_object()) {
        return fail("The note request is not valid");
    }
    const std::string tool = field(json, "tool");
    const std::string path = notes_path(root);
    std::vector<Note> notes;
    const std::string loaded = load_notes(path, &notes);
    if (!loaded.empty()) {
        return fail(loaded);
    }
    if (tool == "list") {
        nlohmann::json out = nlohmann::json::array();
        for (const auto & note : notes) {
            out.push_back({ { "id", note.id }, { "text", note.text } });
        }
        return nlohmann::json{ { "ok", true }, { "notes", std::move(out) } }.dump();
    }
    if (tool == "find") {
        const std::string text = trim_copy(field(json, "text"));
        const std::vector<int> query = embed(text);
        struct Rank {
            int score;
            size_t at;
        };
        std::vector<Rank> ranks;
        for (size_t i = 0; i < notes.size(); ++i) {
            const int score = cosine_score(query, notes[i].v);
            if (score >= kHitScore) {
                ranks.push_back({ score, i });
            }
        }
        std::sort(ranks.begin(), ranks.end(), [](const Rank & a, const Rank & b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            return a.at < b.at;
        });
        nlohmann::json hits = nlohmann::json::array();
        const size_t n = std::min(ranks.size(), (size_t) 3);
        for (size_t i = 0; i < n; ++i) {
            const Note & note = notes[ranks[i].at];
            hits.push_back({ { "id", note.id }, { "score", ranks[i].score }, { "text", note.text } });
        }
        return nlohmann::json{ { "hits", std::move(hits) }, { "ok", true } }.dump();
    }
    if (tool == "save") {
        const std::string text = trim_copy(field(json, "text"));
        if (text.empty()) {
            return fail("A note needs text");
        }
        if (text.size() > (size_t) kMaxText) {
            return fail("That note is over 400 characters");
        }
        if (text.find('\0') != std::string::npos) {
            return fail("That note contains a zero byte");
        }
        for (auto & note : notes) {
            if (note.text == text) {
                note.v = embed(text);
                const std::string wrote = store_notes(root, path, notes);
                if (!wrote.empty()) {
                    return fail(wrote);
                }
                return nlohmann::json{ { "id", note.id }, { "ok", true }, { "text", note.text } }.dump();
            }
        }
        if ((int) notes.size() >= kMaxNotes) {
            notes.erase(notes.begin());
        }
        Note note;
        note.text = text;
        note.v = embed(text);
        note.id = make_id(text, notes);
        notes.push_back(note);
        const std::string wrote = store_notes(root, path, notes);
        if (!wrote.empty()) {
            return fail(wrote);
        }
        return nlohmann::json{ { "id", note.id }, { "ok", true }, { "text", note.text } }.dump();
    }
    if (tool == "forget") {
        const std::string id = field(json, "id");
        const std::string text = trim_copy(field(json, "text"));
        int drop = -1;
        if (!id.empty()) {
            for (size_t i = 0; i < notes.size(); ++i) {
                if (notes[i].id == id) {
                    drop = (int) i;
                }
            }
            if (drop < 0) {
                return fail("No note has that id");
            }
        } else if (!text.empty()) {
            for (size_t i = 0; i < notes.size(); ++i) {
                if (notes[i].text == text) {
                    drop = (int) i;
                }
            }
            if (drop < 0) {
                const std::vector<int> query = embed(text);
                int best = 0;
                for (size_t i = 0; i < notes.size(); ++i) {
                    const int score = cosine_score(query, notes[i].v);
                    if (score > best) {
                        best = score;
                        drop = (int) i;
                    }
                }
                if (best < kForgetScore) {
                    drop = -1;
                }
            }
            if (drop < 0) {
                return fail("No note is close enough.");
            }
        } else {
            return fail("A note needs an id or text");
        }
        const std::string gone = notes[(size_t) drop].id;
        notes.erase(notes.begin() + drop);
        const std::string wrote = store_notes(root, path, notes);
        if (!wrote.empty()) {
            return fail(wrote);
        }
        return nlohmann::json{ { "id", gone }, { "ok", true } }.dump();
    }
    return fail("Unknown note tool");
}

}  // namespace ps5lm
