// Host check for the release parser and the title zip:
//   c++ -std=c++20 -O2 ps5/app/update_test.cpp ps5/app/update_plan.cpp ps5/app/puff.c
//      third_party/llama.cpp/vendor/hash/sha256/sha256.c -I ps5/app
//      -I third_party/llama.cpp/vendor -I third_party/llama.cpp/vendor/hash/sha256
// A deflated zip, when passed as argv[1], is unpacked as well.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update_plan.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}

void expect_is(bool ok, const char * what, const std::string & detail) {
    if (!ok) {
        std::printf("FAIL %s (%s)\n", what, detail.c_str());
        ++fails;
    }
}

void add16(std::string & o, uint16_t v) {
    o.push_back((char) (v & 0xff));
    o.push_back((char) ((v >> 8) & 0xff));
}

void add32(std::string & o, uint32_t v) {
    o.push_back((char) (v & 0xff));
    o.push_back((char) ((v >> 8) & 0xff));
    o.push_back((char) ((v >> 16) & 0xff));
    o.push_back((char) ((v >> 24) & 0xff));
}

uint32_t crc32_bytes(const std::string & s) {
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    uint32_t c = 0xffffffffu;
    for (unsigned char ch : s) {
        c = table[(c ^ ch) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

struct ZFile {
    std::string name;
    std::string data;
    uint16_t flags = 0;
    uint16_t method = 0;
    bool lie = false;
    uint32_t crc = 0;
    uint32_t comp = 0;
    uint32_t plain = 0;
};

std::string make_zip(const std::vector<ZFile> & files) {
    std::string local;
    std::string central;
    for (const ZFile & file : files) {
        const uint32_t offset = (uint32_t) local.size();
        const uint32_t crc = file.lie ? file.crc : crc32_bytes(file.data);
        const uint32_t plain = file.lie ? file.plain : (uint32_t) file.data.size();
        const uint32_t comp = file.lie ? file.comp : (uint32_t) file.data.size();
        add32(local, 0x04034b50u);
        add16(local, 20);
        add16(local, file.flags);
        add16(local, file.method);
        add16(local, 0);
        add16(local, 0);
        add32(local, crc);
        add32(local, comp);
        add32(local, plain);
        add16(local, (uint16_t) file.name.size());
        add16(local, 0);
        local += file.name;
        local += file.data;
        add32(central, 0x02014b50u);
        add16(central, 20);
        add16(central, 20);
        add16(central, file.flags);
        add16(central, file.method);
        add16(central, 0);
        add16(central, 0);
        add32(central, crc);
        add32(central, comp);
        add32(central, plain);
        add16(central, (uint16_t) file.name.size());
        add16(central, 0);
        add16(central, 0);
        add16(central, 0);
        add16(central, 0);
        add32(central, 0);
        add32(central, offset);
        central += file.name;
    }
    const uint32_t cd_off = (uint32_t) local.size();
    std::string out = local + central;
    add32(out, 0x06054b50u);
    add16(out, 0);
    add16(out, 0);
    add16(out, (uint16_t) files.size());
    add16(out, (uint16_t) files.size());
    add32(out, (uint32_t) central.size());
    add32(out, cd_off);
    add16(out, 0);
    return out;
}

const char * kParam = "{\"titleId\":\"PPSA98010\",\"titleName\":\"ArtemisLM PS5\"}";

void write_bytes(const std::string & path, const std::string & body) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
}

std::string read_bytes(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ZFile title_file(const std::string & name, const std::string & data) {
    ZFile f;
    f.name = name;
    f.data = data;
    return f;
}

void check_versions() {
    using ps5lm::version_cmp;
    expect(version_cmp("0.9", "0.9.0") == 0, "0.9 equals 0.9.0");
    expect(version_cmp("v0.9.0", "0.9.0") == 0, "leading v");
    expect(version_cmp("V1.2.3", "1.2.3") == 0, "leading V");
    expect(version_cmp("0.9.0", "0.9.1") < 0, "patch");
    expect(version_cmp("0.9.1", "0.10.0") < 0, "minor");
    expect(version_cmp("0.10.0", "1.0.0") < 0, "major");
    expect(version_cmp("1.0.0", "0.9.9") > 0, "newer major");
    expect(version_cmp("nope", "0.0.1") < 0, "invalid is older");
    expect(version_cmp("0.9.0", "0.9.0-rc1") > 0, "suffix is older");
    expect(version_cmp("nope", "also") == 0, "two invalids");
    expect(version_cmp("", "0.0.0") < 0, "empty");
}

void check_parse() {
    using ps5lm::parse_release;
    ps5lm::ReleaseInfo info;
    std::string error;
    const char * ok =
        "{"
        "\"tag_name\":\"v0.9.1\","
        "\"assets\":["
        "{\"name\":\"notes.txt\",\"browser_download_url\":\"https://example.com/x\",\"digest\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
        "{\"name\":\"artemislm-ps5-app-v0.9.1.zip\","
        "\"browser_download_url\":\"https://github.com/joelsmithlowe/ArtemisLM-PS5/releases/download/v0.9.1/artemislm-ps5-app-v0.9.1.zip\","
        "\"digest\":\"SHA256:0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\"}"
        "]}";
    expect(parse_release(ok, &info, &error), "parse good release");
    expect(info.version == "0.9.1", "version");
    expect(info.name == "artemislm-ps5-app-v0.9.1.zip", "asset name");
    expect(info.sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "digest lower");
    expect(info.url.find("github.com/joelsmithlowe/ArtemisLM-PS5/releases/download/") != std::string::npos, "url host");

    expect(!parse_release("{", &info, &error) && error == "Could not read the release", "bad json");
    const char * wrong_name =
        "{\"tag_name\":\"v0.9.1\",\"assets\":[{\"name\":\"artemislm-ps5-app-v0.9.0.zip\","
        "\"browser_download_url\":\"https://github.com/joelsmithlowe/ArtemisLM-PS5/releases/download/v0.9.1/artemislm-ps5-app-v0.9.0.zip\","
        "\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}]}";
    expect(!parse_release(wrong_name, &info, &error) && error == "The release has no app zip", "wrong asset");
    const char * no_sum =
        "{\"tag_name\":\"v0.9.1\",\"assets\":[{\"name\":\"artemislm-ps5-app-v0.9.1.zip\","
        "\"browser_download_url\":\"https://github.com/joelsmithlowe/ArtemisLM-PS5/releases/download/v0.9.1/artemislm-ps5-app-v0.9.1.zip\"}]}";
    expect(!parse_release(no_sum, &info, &error) && error == "The release has no checksum", "no digest");
    const char * evil_host =
        "{\"tag_name\":\"v0.9.1\",\"assets\":[{\"name\":\"artemislm-ps5-app-v0.9.1.zip\","
        "\"browser_download_url\":\"https://evil.example/artemislm-ps5-app-v0.9.1.zip\","
        "\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}]}";
    expect(!parse_release(evil_host, &info, &error) && error == "The download address is not this repository", "wrong host");
    const char * dots =
        "{\"tag_name\":\"v0.9.1\",\"assets\":[{\"name\":\"artemislm-ps5-app-v0.9.1.zip\","
        "\"browser_download_url\":\"https://github.com/joelsmithlowe/ArtemisLM-PS5/releases/download/../v0.9.1/artemislm-ps5-app-v0.9.1.zip\","
        "\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}]}";
    expect(!parse_release(dots, &info, &error), "dotdot url");
}

void check_zip(const std::string & root) {
    using ps5lm::extract_title_zip;
    using ps5lm::install_title;
    using ps5lm::sha256_file;
    using ps5lm::stage_ok;
    const std::string dir = root + "/work";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_bytes(dir + "/abc", "abc");
    expect(sha256_file(dir + "/abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    expect(sha256_file(dir + "/missing").empty(), "sha256 missing");

    const std::string good = make_zip({
        title_file("PPSA98010/", ""),
        title_file("PPSA98010/sce_sys/", ""),
        title_file("README.txt", "ignore me"),
        title_file("PPSA98010/eboot.bin", "eboot-bytes"),
        title_file("PPSA98010/sce_sys/param.json", kParam),
    });
    write_bytes(dir + "/good.zip", good);
    const std::string stage = dir + "/stage";
    write_bytes(stage + "/old", "gone");
    std::string error;
    expect_is(extract_title_zip(dir + "/good.zip", stage, &error), "extract good", error);
    expect(read_bytes(stage + "/eboot.bin") == "eboot-bytes", "eboot bytes");
    expect(!std::filesystem::exists(stage + "/README.txt"), "ignored member");
    expect(!std::filesystem::exists(stage + "/old"), "old stage removed");
    expect_is(stage_ok(stage, &error), "stage ok", error);

    std::string bad_param = std::string(kParam);
    const auto at = bad_param.find("ArtemisLM");
    bad_param.replace(at, 9, "OtherName");
    write_bytes(dir + "/wrong.zip", make_zip({
        title_file("PPSA98010/eboot.bin", "eboot-bytes"),
        title_file("PPSA98010/sce_sys/param.json", bad_param),
    }));
    expect(extract_title_zip(dir + "/wrong.zip", dir + "/wrong", &error), "extract wrong title");
    expect(!stage_ok(dir + "/wrong", &error) && error == "The zip is not ArtemisLM PS5", "rejects other title");

    const std::string home = dir + "/homebrew";
    std::filesystem::create_directories(home);
    write_bytes(home + "/PPSA98010/eboot.bin", "OLD-EBOOT");
    write_bytes(home + "/PPSA98010/keep.txt", "stay");
    expect_is(install_title(stage, home + "/PPSA98010", &error), "install", error);
    expect(read_bytes(home + "/PPSA98010/eboot.bin") == "eboot-bytes", "replaced eboot");
    expect(read_bytes(home + "/PPSA98010/sce_sys/param.json").find("ArtemisLM") != std::string::npos, "installed param");
    expect(read_bytes(home + "/PPSA98010/keep.txt") == "stay", "left extra file");
    expect(!std::filesystem::exists(home + "/PPSA98010/eboot.bin.new"), "temp name removed");
    expect(!install_title(stage, dir + "/missing/child/PPSA98010", &error), "missing parent");

    write_bytes(dir + "/evil.zip", make_zip({
        title_file("PPSA98010/eboot.bin", "eboot-bytes"),
        title_file("PPSA98010/../../artemis-evil/pwned", "pwned-marker"),
    }));
    write_bytes(dir + "/evil-stage/marker", "here");
    expect(!extract_title_zip(dir + "/evil.zip", dir + "/evil-stage", &error), "rejects dotdot");
    expect(error == "The zip has an unsafe path", "dotdot message");
    expect(!std::filesystem::exists(dir + "/evil-stage"), "stage removed");
    expect(!std::filesystem::exists(dir + "/artemis-evil/pwned"), "no escape beside stage");
    expect(!std::filesystem::exists(root + "/artemis-evil/pwned"), "no escape above work");
    expect(read_bytes("/tmp/artemis-evil/pwned") != "pwned-marker", "no escape in tmp");

    write_bytes(dir + "/slash.zip", make_zip({ title_file("PPSA98010/eboot.bin\\x", "nope") }));
    expect(!extract_title_zip(dir + "/slash.zip", dir + "/slash-stage", &error), "rejects backslash");
    expect(!std::filesystem::exists(dir + "/slash-stage"), "backslash stage removed");

    write_bytes(dir + "/abs.zip", make_zip({ title_file("PPSA98010//tmp/evil", "nope") }));
    expect(!extract_title_zip(dir + "/abs.zip", dir + "/abs-stage", &error), "rejects absolute");

    ZFile locked = title_file("PPSA98010/eboot.bin", "secret");
    locked.flags = 1;
    write_bytes(dir + "/crypt.zip", make_zip({ locked }));
    expect(!extract_title_zip(dir + "/crypt.zip", dir + "/crypt-stage", &error) && error == "The zip is encrypted", "encrypted");

    ZFile huge = title_file("PPSA98010/eboot.bin", "x");
    huge.lie = true;
    huge.plain = 121u << 20;
    huge.comp = 1;
    huge.crc = 0;
    write_bytes(dir + "/huge.zip", make_zip({ huge }));
    expect(!extract_title_zip(dir + "/huge.zip", dir + "/huge-stage", &error) && error == "The zip is too large", "member cap");

    ZFile zip64 = title_file("PPSA98010/eboot.bin", "x");
    zip64.lie = true;
    zip64.plain = 0xffffffffu;
    zip64.comp = 0xffffffffu;
    write_bytes(dir + "/zip64.zip", make_zip({ zip64 }));
    expect(!extract_title_zip(dir + "/zip64.zip", dir + "/zip64-stage", &error) && error == "The zip is not a plain title archive", "zip64");

    ZFile damaged = title_file("PPSA98010/eboot.bin", "hello");
    damaged.lie = true;
    damaged.crc = 1;
    damaged.comp = 5;
    damaged.plain = 5;
    write_bytes(dir + "/bad.zip", make_zip({ damaged, title_file("PPSA98010/sce_sys/param.json", kParam) }));
    write_bytes(dir + "/bad-stage/marker", "here");
    expect(!extract_title_zip(dir + "/bad.zip", dir + "/bad-stage", &error) && error == "A file in the zip is damaged", "bad crc");
    expect(!std::filesystem::exists(dir + "/bad-stage"), "damaged stage removed");

    ZFile odd = title_file("PPSA98010/eboot.bin", "x");
    odd.method = 99;
    write_bytes(dir + "/odd.zip", make_zip({ odd }));
    expect(!extract_title_zip(dir + "/odd.zip", dir + "/odd-stage", &error), "unknown method");

    write_bytes(dir + "/notes.zip", make_zip({ title_file("README.txt", "only") }));
    expect(!extract_title_zip(dir + "/notes.zip", dir + "/notes-stage", &error) && error == "The zip has no app", "no title");
}

void check_deflate(const std::string & zip, const std::string & root) {
    const std::string stage = root + "/deflate-stage";
    std::filesystem::remove_all(stage);
    std::string error;
    expect_is(ps5lm::extract_title_zip(zip, stage, &error), "deflate extract", error);
    expect(read_bytes(stage + "/eboot.bin") == "deflate-ok", "deflate bytes");
    expect(!std::filesystem::exists(stage + "/NOTES.txt"), "deflate ignored");
    expect_is(ps5lm::stage_ok(stage, &error), "deflate stage", error);
}

}  // namespace

int main(int argc, char ** argv) {
    check_versions();
    check_parse();
    const std::string root = "/tmp/artemis-upd";
    std::filesystem::remove_all(root);
    std::filesystem::remove_all("/tmp/artemis-evil");
    check_zip(root);
    if (argc > 1) {
        check_deflate(argv[1], root);
    }
    if (fails) {
        std::printf("update_test: %d failed\n", fails);
        return 1;
    }
    std::printf("update_test: ok\n");
    return 0;
}
