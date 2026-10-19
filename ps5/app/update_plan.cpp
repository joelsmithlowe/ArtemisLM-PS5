// Reading a GitHub release JSON object and unpacking the title zip.
// Network and the console restart live in update.cpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update_plan.hpp"

extern "C" {
#include "puff.h"
#include "sha256.h"
}

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

namespace ps5lm {

namespace {

constexpr uint64_t kMaxZip    = 80ull << 20;
constexpr uint64_t kMaxMember = 120ull << 20;
constexpr uint64_t kMaxTotal  = 160ull << 20;
constexpr int      kMaxFiles  = 256;

void set_error(std::string * error, const char * text) {
    if (error) {
        *error = text;
    }
}

uint16_t ru16(const unsigned char * p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

uint32_t ru32(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

uint32_t crc32_bytes(const unsigned char * data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

// Three numeric parts. A leading v is ignored. Missing parts are 0.
bool parse_version(const std::string & in, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t i = 0;
    if (!in.empty() && (in[0] == 'v' || in[0] == 'V')) {
        i = 1;
    }
    if (i >= in.size() || !std::isdigit(static_cast<unsigned char>(in[i]))) {
        return false;
    }
    int parts = 0;
    while (i < in.size() && parts < 3) {
        if (!std::isdigit(static_cast<unsigned char>(in[i]))) {
            return false;
        }
        long value = 0;
        while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) {
            value = value * 10 + (in[i] - '0');
            if (value > 1000000) {
                return false;
            }
            ++i;
        }
        out[parts++] = (int) value;
        if (i == in.size()) {
            return true;
        }
        if (in[i] != '.') {
            return false;
        }
        ++i;
        if (i == in.size()) {
            return false;
        }
    }
    return i == in.size();
}

std::string hex_lower(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    for (char & c : text) {
        c = (char) std::tolower(static_cast<unsigned char>(c));
    }
    const std::string prefix = "sha256:";
    if (text.compare(0, prefix.size(), prefix) == 0) {
        text.erase(0, prefix.size());
    }
    if (text.size() != 64) {
        return {};
    }
    for (char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return {};
        }
    }
    return text;
}

bool url_ok(const std::string & url, const std::string & asset) {
    const std::string prefix = "https://github.com/joelsmithlowe/ArtemisLM-PS5/releases/download/";
    const std::string tail = "/" + asset;
    if (url.size() <= prefix.size() + tail.size()) {
        return false;
    }
    if (url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (url.compare(url.size() - tail.size(), tail.size(), tail) != 0) {
        return false;
    }
    if (url.find("..") != std::string::npos || url.find(' ') != std::string::npos ||
        url.find('\\') != std::string::npos || url.find('\n') != std::string::npos ||
        url.find('\r') != std::string::npos || url.find('?') != std::string::npos ||
        url.find('#') != std::string::npos) {
        return false;
    }
    const std::string mid = url.substr(prefix.size(), url.size() - prefix.size() - tail.size());
    return !mid.empty() && mid.find('/') == std::string::npos;
}

// A member under PPSA98010/. `rel` is the path inside the title folder.
// `ignore` is a file that is not part of the title. `unsafe` fails the zip.
bool title_path(const std::string & name, std::string * rel, bool * directory, bool * ignore, bool * unsafe) {
    *rel = {};
    *directory = false;
    *ignore = false;
    *unsafe = false;
    const std::string prefix = "PPSA98010/";
    if (name.compare(0, prefix.size(), prefix) != 0) {
        *ignore = true;
        return true;
    }
    for (unsigned char c : name) {
        if (c < 32 || c == 127 || c == '\\') {
            *unsafe = true;
            return false;
        }
    }
    const bool dir = !name.empty() && name.back() == '/';
    std::string rest = name.substr(prefix.size());
    if (!rest.empty() && rest[0] == '/') {
        *unsafe = true;
        return false;
    }
    std::string out;
    size_t i = 0;
    while (i < rest.size()) {
        const size_t slash = rest.find('/', i);
        const std::string part = rest.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        i = slash == std::string::npos ? rest.size() : slash + 1;
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == ".." || part.find(':') != std::string::npos) {
            *unsafe = true;
            return false;
        }
        if (!out.empty()) {
            out += '/';
        }
        out += part;
    }
    if (out.empty() && !dir) {
        *unsafe = true;
        return false;
    }
    *rel = out;
    *directory = dir || out.empty();
    return true;
}

bool stage_dest_ok(const std::string & dest) {
    if (dest.size() < 4 || dest[0] != '/' || dest.find("..") != std::string::npos) {
        return false;
    }
    return dest != "/" && dest != "/data" && dest != "/mnt" && dest != "/tmp" && dest != "/data/homebrew" &&
           dest != "/data/ArtemisLM" && dest != "/data/ArtemisLM/update";
}

void remove_stage(const std::string & dest) {
    if (!stage_dest_ok(dest)) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
}

bool read_at(FILE * f, uint64_t at, void * dst, size_t n) {
    if (fseek(f, (long) at, SEEK_SET) != 0) {
        return false;
    }
    return fread(dst, 1, n, f) == n;
}

struct Member {
    std::string name;
    std::string rel;
    bool        directory = false;
    uint16_t    flags = 0;
    uint16_t    method = 0;
    uint32_t    crc = 0;
    uint32_t    comp = 0;
    uint32_t    plain = 0;
    uint32_t    local = 0;
};

bool load_members(FILE * f, long zip_size, std::vector<Member> * out, std::string * error) {
    const long tail_n = zip_size < 65557 ? zip_size : 65557;
    std::vector<unsigned char> tail((size_t) tail_n);
    if (!read_at(f, (uint64_t) (zip_size - tail_n), tail.data(), tail.size())) {
        set_error(error, "The zip could not be read");
        return false;
    }
    long eocd = -1;
    for (long i = tail_n - 22; i >= 0; --i) {
        if (ru32(&tail[(size_t) i]) != 0x06054b50u) {
            continue;
        }
        const uint16_t comment = ru16(&tail[(size_t) i + 20]);
        if ((long) i + 22 + comment == tail_n) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        set_error(error, "The zip could not be read");
        return false;
    }
    const unsigned char * e = &tail[(size_t) eocd];
    if (ru16(e + 4) != 0 || ru16(e + 6) != 0) {
        set_error(error, "The zip is not a plain title archive");
        return false;
    }
    const uint32_t cd_size = ru32(e + 12);
    const uint32_t cd_off = ru32(e + 16);
    const int entries = ru16(e + 10);
    if (cd_off == 0xffffffffu || cd_size == 0xffffffffu || entries > kMaxFiles) {
        set_error(error, "The zip is not a plain title archive");
        return false;
    }
    if ((uint64_t) cd_off + cd_size > (uint64_t) zip_size) {
        set_error(error, "The zip could not be read");
        return false;
    }
    std::vector<unsigned char> cd(cd_size);
    if (cd_size > 0 && !read_at(f, cd_off, cd.data(), cd.size())) {
        set_error(error, "The zip could not be read");
        return false;
    }
    size_t at = 0;
    uint64_t total = 0;
    for (int n = 0; n < entries; ++n) {
        if (at + 46 > cd.size() || ru32(&cd[at]) != 0x02014b50u) {
            set_error(error, "The zip could not be read");
            return false;
        }
        const uint16_t flags = ru16(&cd[at + 8]);
        const uint16_t method = ru16(&cd[at + 10]);
        const uint32_t crc = ru32(&cd[at + 16]);
        const uint32_t comp = ru32(&cd[at + 20]);
        const uint32_t plain = ru32(&cd[at + 24]);
        const uint16_t name_n = ru16(&cd[at + 28]);
        const uint16_t extra_n = ru16(&cd[at + 30]);
        const uint16_t comment_n = ru16(&cd[at + 32]);
        const uint32_t local = ru32(&cd[at + 42]);
        if (at + 46 + name_n + extra_n + comment_n > cd.size()) {
            set_error(error, "The zip could not be read");
            return false;
        }
        std::string name(reinterpret_cast<const char *>(&cd[at + 46]), name_n);
        at += 46u + name_n + extra_n + comment_n;
        std::string rel;
        bool directory = false, ignore = false, unsafe = false;
        if (!title_path(name, &rel, &directory, &ignore, &unsafe)) {
            set_error(error, "The zip has an unsafe path");
            return false;
        }
        if (ignore) {
            continue;
        }
        if ((flags & 0x0001) || (flags & 0x0040) || comp == 0xffffffffu || plain == 0xffffffffu || local == 0xffffffffu) {
            set_error(error, (flags & 0x0041) ? "The zip is encrypted" : "The zip is not a plain title archive");
            return false;
        }
        if (!directory && (plain > kMaxMember || comp > kMaxZip)) {
            set_error(error, "The zip is too large");
            return false;
        }
        if (!directory && total > kMaxTotal - plain) {
            set_error(error, "The zip is too large");
            return false;
        }
        if (!directory && method != 0 && method != 8) {
            set_error(error, "The zip could not be unpacked");
            return false;
        }
        if (!directory && method == 0 && comp != plain) {
            set_error(error, "The zip could not be unpacked");
            return false;
        }
        total += directory ? 0 : plain;
        Member m;
        m.name = std::move(name);
        m.rel = std::move(rel);
        m.directory = directory;
        m.flags = flags;
        m.method = method;
        m.crc = crc;
        m.comp = comp;
        m.plain = plain;
        m.local = local;
        out->push_back(std::move(m));
    }
    if (at != cd.size()) {
        set_error(error, "The zip could not be read");
        return false;
    }
    return true;
}

bool write_member(FILE * f, long zip_size, const Member & m, const std::string & dest, std::string * error) {
    if (m.directory || m.rel.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(m.rel.empty() ? dest : dest + "/" + m.rel, ec);
        if (ec) {
            set_error(error, "The zip could not be unpacked");
            return false;
        }
        return true;
    }
    unsigned char local[30];
    if ((uint64_t) m.local + 30 > (uint64_t) zip_size || !read_at(f, m.local, local, 30) || ru32(local) != 0x04034b50u) {
        set_error(error, "The zip could not be read");
        return false;
    }
    if (ru16(local + 6) & 0x0001) {
        set_error(error, "The zip is encrypted");
        return false;
    }
    const uint32_t data_at = m.local + 30u + ru16(local + 26) + ru16(local + 28);
    if ((uint64_t) data_at + m.comp > (uint64_t) zip_size) {
        set_error(error, "The zip could not be read");
        return false;
    }
    std::vector<unsigned char> comp(m.comp);
    if (m.comp > 0 && !read_at(f, data_at, comp.data(), comp.size())) {
        set_error(error, "The zip could not be read");
        return false;
    }
    std::vector<unsigned char> plain;
    if (m.method == 0) {
        plain.swap(comp);
    } else if (m.plain == 0) {
        if (m.comp != 0) {
            set_error(error, "The zip could not be unpacked");
            return false;
        }
    } else {
        try {
            plain.resize(m.plain);
        } catch (const std::bad_alloc &) {
            set_error(error, "Not enough memory. Unload the model and try again");
            return false;
        }
        unsigned long destlen = m.plain;
        unsigned long sourcelen = m.comp;
        const unsigned char * source = comp.empty() ? (const unsigned char *) "" : comp.data();
        if (puff(plain.data(), &destlen, source, &sourcelen) != 0 || destlen != m.plain) {
            set_error(error, "The zip could not be unpacked");
            return false;
        }
    }
    if (crc32_bytes(plain.data(), plain.size()) != m.crc) {
        set_error(error, "A file in the zip is damaged");
        return false;
    }
    const std::string full = dest + "/" + m.rel;
    const auto slash = full.rfind('/');
    if (slash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(full.substr(0, slash), ec);
        if (ec) {
            set_error(error, "The zip could not be unpacked");
            return false;
        }
    }
    FILE * out = std::fopen(full.c_str(), "wb");
    if (!out) {
        set_error(error, "The zip could not be unpacked");
        return false;
    }
    const bool wrote = plain.empty() || std::fwrite(plain.data(), 1, plain.size(), out) == plain.size();
    const bool closed = std::fclose(out) == 0;
    if (!wrote || !closed) {
        std::filesystem::remove(full);
        set_error(error, "The zip could not be unpacked");
        return false;
    }
    return true;
}

bool copy_over(const std::filesystem::path & from, const std::filesystem::path & to, std::string * error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto bytes = fs::file_size(from, ec);
    if (ec) {
        std::printf("artemislm-ps5: update: copy failed\n");
        set_error(error, "The install did not finish");
        return false;
    }
    fs::create_directories(to.parent_path(), ec);
    const fs::path tmp = to.string() + ".new";
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec || fs::file_size(tmp, ec) != bytes) {
        fs::remove(tmp);
        std::printf("artemislm-ps5: update: copy failed\n");
        set_error(error, "The install did not finish");
        return false;
    }
    ec.clear();
    fs::rename(tmp, to, ec);
    if (ec) {
        ec.clear();
        fs::remove(to, ec);
        ec.clear();
        fs::rename(tmp, to, ec);
    }
    if (ec) {
        fs::remove(tmp);
        std::printf("artemislm-ps5: update: copy failed\n");
        set_error(error, "The install did not finish");
        return false;
    }
    return true;
}

}  // namespace

int version_cmp(const std::string & a, const std::string & b) {
    int av[3], bv[3];
    const bool a_ok = parse_version(a, av);
    const bool b_ok = parse_version(b, bv);
    if (!a_ok && !b_ok) {
        return 0;
    }
    if (!a_ok) {
        return -1;
    }
    if (!b_ok) {
        return 1;
    }
    for (int i = 0; i < 3; ++i) {
        if (av[i] != bv[i]) {
            return av[i] < bv[i] ? -1 : 1;
        }
    }
    return 0;
}

bool parse_release(const std::string & body, ReleaseInfo * out, std::string * error) {
    if (!out) {
        set_error(error, "Could not read the release");
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) {
        set_error(error, "Could not read the release");
        return false;
    }
    if (!j.contains("tag_name") || !j["tag_name"].is_string()) {
        set_error(error, "Could not read the release");
        return false;
    }
    std::string tag = j["tag_name"].get<std::string>();
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) {
        tag.erase(tag.begin());
    }
    int parts[3];
    if (!parse_version(tag, parts)) {
        set_error(error, "Could not read the release");
        return false;
    }
    if (!j.contains("assets") || !j["assets"].is_array()) {
        set_error(error, "The release has no app zip");
        return false;
    }
    const std::string want = "artemislm-ps5-app-v" + tag + ".zip";
    const nlohmann::json * asset = nullptr;
    for (const auto & one : j["assets"]) {
        if (!one.is_object() || !one.contains("name") || !one["name"].is_string()) {
            continue;
        }
        if (one["name"].get<std::string>() == want) {
            asset = &one;
        }
    }
    if (!asset) {
        set_error(error, "The release has no app zip");
        return false;
    }
    const auto text_of = [](const nlohmann::json & object, const char * key) {
        if (!object.contains(key) || !object[key].is_string()) {
            return std::string();
        }
        return object[key].get<std::string>();
    };
    const std::string url = text_of(*asset, "browser_download_url");
    if (!url_ok(url, want)) {
        set_error(error, "The download address is not this repository");
        return false;
    }
    const std::string sum = hex_lower(text_of(*asset, "digest"));
    if (sum.empty()) {
        set_error(error, "The release has no checksum");
        return false;
    }
    out->version = tag;
    out->url = url;
    out->sha256 = sum;
    out->name = want;
    return true;
}

bool extract_title_zip(const std::string & zip_path, const std::string & dest, std::string * error) {
    FILE * f = std::fopen(zip_path.c_str(), "rb");
    if (!f) {
        set_error(error, "The zip could not be read");
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        set_error(error, "The zip could not be read");
        return false;
    }
    const long zip_size = std::ftell(f);
    if (zip_size < 22 || (uint64_t) zip_size > kMaxZip) {
        std::fclose(f);
        set_error(error, zip_size > 22 ? "The zip is too large" : "The zip could not be read");
        return false;
    }
    if (!stage_dest_ok(dest)) {
        std::fclose(f);
        set_error(error, "The zip could not be unpacked");
        return false;
    }
    std::vector<Member> members;
    if (!load_members(f, zip_size, &members, error)) {
        std::fclose(f);
        remove_stage(dest);
        return false;
    }
    remove_stage(dest);
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        std::fclose(f);
        set_error(error, "The zip could not be unpacked");
        return false;
    }
    bool wrote_file = false;
    for (const Member & m : members) {
        if (!write_member(f, zip_size, m, dest, error)) {
            std::fclose(f);
            remove_stage(dest);
            return false;
        }
        wrote_file = wrote_file || !m.directory;
    }
    std::fclose(f);
    if (!wrote_file) {
        remove_stage(dest);
        set_error(error, "The zip has no app");
        return false;
    }
    return true;
}

bool stage_ok(const std::string & dest, std::string * error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path eboot = fs::path(dest) / "eboot.bin";
    if (!fs::is_regular_file(eboot, ec) || fs::file_size(eboot, ec) == 0) {
        set_error(error, "The zip has no app");
        return false;
    }
    const fs::path param = fs::path(dest) / "sce_sys" / "param.json";
    FILE * f = std::fopen(param.string().c_str(), "rb");
    if (!f) {
        set_error(error, "The zip is not ArtemisLM PS5");
        return false;
    }
    char buf[65536];
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    const std::string text(buf, n);
    if (text.find("PPSA98010") == std::string::npos || text.find("ArtemisLM PS5") == std::string::npos) {
        set_error(error, "The zip is not ArtemisLM PS5");
        return false;
    }
    return true;
}

namespace {

// A directory entry name, not an absolute path. ".." inside the name is rejected
// so a joined destination cannot leave the install folder.
bool staged_name_ok(const std::string & name) {
    if (name.empty() || name == "." || name == ".." || name.find("..") != std::string::npos) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || name[i] == '/' || name[i] == '\\' || name[i] == ':') {
            return false;
        }
    }
    return true;
}

std::filesystem::path staged_dest(const std::filesystem::path & install, const std::string & rel) {
    namespace fs = std::filesystem;
    fs::path to = install;
    size_t start = 0;
    while (start < rel.size()) {
        const size_t slash = rel.find('/', start);
        const std::string part = slash == std::string::npos ? rel.substr(start) : rel.substr(start, slash - start);
        if (!part.empty()) {
            to /= part;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return to;
}

}  // namespace

bool install_title(const std::string & stage, const std::string & install_dir, std::string * error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path install(install_dir);
    if (!fs::is_directory(install.parent_path(), ec)) {
        set_error(error, "Copy ArtemisLM PS5 to /data/homebrew once, then updates can install");
        return false;
    }
    if (install_dir.find("..") != std::string::npos || stage.find("..") != std::string::npos) {
        set_error(error, "The install did not finish");
        return false;
    }
    if (!fs::is_directory(install, ec)) {
        fs::create_directory(install, ec);
    }
    if (!fs::is_directory(install)) {
        set_error(error, "The install did not finish");
        return false;
    }
    struct Staged {
        fs::path path;
        std::string rel;
    };
    struct Walk {
        fs::path path;
        std::string rel;
        int depth;
    };
    std::vector<Staged> files;
    std::vector<Walk> stack = { { fs::path(stage), "", 0 } };
    while (!stack.empty()) {
        const Walk dir = stack.back();
        stack.pop_back();
        if (dir.depth > 8) {
            std::printf("artemislm-ps5: update: staged entry is not a file\n");
            set_error(error, "The install did not finish");
            return false;
        }
        ec.clear();
        fs::directory_iterator it(dir.path, ec), end;
        if (ec) {
            set_error(error, "The install did not finish");
            return false;
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                set_error(error, "The install did not finish");
                return false;
            }
            const fs::path from = it->path();
            const std::string name = from.filename().generic_string();
            if (name == "." || name == "..") {
                continue;
            }
            if (!staged_name_ok(name)) {
                std::printf("artemislm-ps5: update: staged name rejected\n");
                set_error(error, "The staged app has an unsafe path");
                return false;
            }
            // lstat can fail for an ordinary file on this console. That is not a symlink.
            std::error_code link_ec;
            if (fs::is_symlink(from, link_ec) && !link_ec) {
                std::printf("artemislm-ps5: update: staged symlink rejected\n");
                set_error(error, "The staged app has an unsafe path");
                return false;
            }
            const std::string rel = dir.rel.empty() ? name : dir.rel + "/" + name;
            std::error_code dir_ec;
            std::error_code file_ec;
            if (fs::is_directory(from, dir_ec) && !dir_ec) {
                stack.push_back({ from, rel, dir.depth + 1 });
            } else if (fs::is_regular_file(from, file_ec) && !file_ec) {
                files.push_back({ from, rel });
            } else {
                std::printf("artemislm-ps5: update: staged entry is not a file\n");
                set_error(error, "The staged app has an unsafe path");
                return false;
            }
            if ((int) files.size() > kMaxFiles) {
                set_error(error, "The install did not finish");
                return false;
            }
        }
    }
    std::vector<std::pair<fs::path, fs::path>> plan;
    fs::path eboot_from, eboot_to;
    bool have_eboot = false;
    for (const Staged & item : files) {
        const fs::path to = staged_dest(install, item.rel);
        if (item.rel == "eboot.bin") {
            eboot_from = item.path;
            eboot_to = to;
            have_eboot = true;
        } else {
            plan.emplace_back(item.path, to);
        }
    }
    for (const auto & item : plan) {
        if (!copy_over(item.first, item.second, error)) {
            return false;
        }
    }
    if (!have_eboot) {
        set_error(error, "The zip has no app");
        return false;
    }
    return copy_over(eboot_from, eboot_to, error);
}

std::string sha256_file(const std::string & path) {
    FILE * f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return {};
    }
    sha256_t ctx;
    sha256_init(&ctx);
    unsigned char buf[1 << 16];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&ctx, buf, n);
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        return {};
    }
    unsigned char dig[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, dig);
    static const char * hex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        out[(size_t) i * 2] = hex[dig[i] >> 4];
        out[(size_t) i * 2 + 1] = hex[dig[i] & 0xf];
    }
    return out;
}

}  // namespace ps5lm
