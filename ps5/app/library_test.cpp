// Host check for the model folders and a move into the home folder:
//   c++ -std=c++17 -O1 ps5/app/library_test.cpp ps5/app/library.cpp ps5/app/model_plan.cpp -o build/library_test
// The filesystem half runs only when /data/.artemislm-libtest exists, so a
// normal run cannot touch a real /data. The runner bind-mounts a temp tree.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.hpp"
#include "stats.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace ps5lm {

bool extra_dir_ok(const std::string & path) {
    if (path.empty()) {
        return true;
    }
    if (path.size() > 180 || path[0] != '/' || path.back() == '/') {
        return false;
    }
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos || path.find(' ') != std::string::npos) {
        return false;
    }
    return path.rfind("/data/", 0) == 0 || path.rfind("/mnt/", 0) == 0;
}

double g_fake_free = -1;

double drive_free_gib(const std::string &) { return g_fake_free; }

}  // namespace ps5lm

namespace {

int fails = 0;

void expect(bool ok, const char * what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}

bool has_dir(const std::vector<std::string> & dirs, const char * path) {
    for (const auto & d : dirs) {
        if (d == path) {
            return true;
        }
    }
    return false;
}

void write_file(const std::string & path, const std::string & body) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
}

std::string read_all(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void check_paths() {
    using namespace ps5lm;
    Settings s;
    auto dirs = library_dirs(s);
    expect(!dirs.empty() && dirs[0] == kHomeModels, "home first");
    expect(has_dir(dirs, "/data/PS5LM/models"), "ps5lm data");
    expect(has_dir(dirs, "/mnt/ext0/PS5LM/models"), "ps5lm m.2");
    expect(has_dir(dirs, "/mnt/usb0/PS5LM/models") && has_dir(dirs, "/mnt/usb1/PS5LM/models"), "ps5lm usb");
    expect(has_dir(dirs, "/mnt/ext0/ArtemisLM/models"), "artemis m.2");
    expect(has_dir(dirs, "/mnt/usb0") && has_dir(dirs, "/mnt/usb1"), "usb root");

    s.scan_ps5lm = false;
    s.scan_usb = false;
    dirs = library_dirs(s);
    expect(dirs.size() == 1, "home only");
    s.extra_dir = "/data/custom/models";
    dirs = library_dirs(s);
    expect(dirs.size() == 2 && dirs[1] == "/data/custom/models", "extra dir");
    s.extra_dir = "/data/custom/../etc";
    expect(library_dirs(s).size() == 1, "reject dotdot");
    s.extra_dir = "/tmp/models";
    expect(library_dirs(s).size() == 1, "reject tmp");
    s.extra_dir = "/data/custom/models/";
    expect(library_dirs(s).size() == 1, "reject slash");

    expect(model_in_home("/data/ArtemisLM/models/a.gguf"), "in home");
    expect(!model_in_home("/data/ArtemisLM/models/nested/a.gguf"), "not nested");
    expect(model_folder("/data/PS5LM/models/a.gguf") == "PS5LM", "folder ps5lm");
    expect(model_folder("/mnt/usb0/PS5LM/models/a.gguf") == "PS5LM", "folder ps5lm usb");
    expect(model_folder("/mnt/ext0/ArtemisLM/models/a.gguf") == "M.2", "folder m.2");
    expect(model_folder("/mnt/usb1/loose.gguf") == "USB", "folder usb");
    expect(model_folder("/data/custom/models/a.gguf") == "Other", "folder other");

    s.scan_ps5lm = true;
    s.scan_usb = true;
    s.extra_dir.clear();
    expect(library_visible(s, "/data/PS5LM/models/a.gguf"), "visible");
    expect(!library_visible(s, "/data/PS5LM/models/../a.gguf"), "hidden dotdot");
    expect(!library_visible(s, "/data/PS5LM/secret/a.gguf"), "hidden other");
    expect(!library_visible(s, "/data/PS5LM/models/a.bin"), "hidden type");
}

void check_files() {
    using namespace ps5lm;
    if (!std::filesystem::exists("/data/.artemislm-libtest")) {
        std::printf("library_test: path checks only\n");
        return;
    }
    const std::string home = std::string(kHomeModels);
    const std::string ps5 = "/data/PS5LM/models";
    const std::string usb = "/mnt/usb0";
    std::filesystem::remove_all(home);
    std::filesystem::remove_all(ps5);
    std::filesystem::remove_all(usb);
    std::filesystem::remove_all("/data/more");

    write_file(ps5 + "/tiny.gguf", "hello-model");
    write_file(ps5 + "/mmproj-tiny.gguf", "projector");
    write_file(ps5 + "/pack-00001-of-00002.gguf", "part-one");
    write_file(ps5 + "/pack-00002-of-00002.gguf", "part-two");
    write_file(usb + "/loose.gguf", "loose-file");
    write_file("/data/more/models/extra.gguf", "extra-file");

    Settings s;
    s.extra_dir = "/data/more/models";
    g_fake_free = -1;
    const std::string note = library_move(s, "", "");
    expect(note.rfind("Moved ", 0) == 0, "moved note");
    expect(read_all(home + "/tiny.gguf") == "hello-model", "tiny copied");
    expect(read_all(home + "/mmproj-tiny.gguf") == "projector", "projector copied");
    expect(read_all(home + "/pack-00002-of-00002.gguf") == "part-two", "split copied");
    expect(read_all(home + "/loose.gguf") == "loose-file", "usb copied");
    expect(read_all(home + "/extra.gguf") == "extra-file", "extra copied");
    expect(!std::filesystem::exists(ps5 + "/tiny.gguf"), "tiny source gone");
    expect(!std::filesystem::exists(ps5 + "/mmproj-tiny.gguf"), "projector source gone");
    expect(!std::filesystem::exists(ps5 + "/pack-00002-of-00002.gguf"), "split source gone");
    expect(!std::filesystem::exists(usb + "/loose.gguf"), "usb source gone");

    write_file(ps5 + "/tiny.gguf", "hello-model");
    g_fake_free = 0.1;
    const std::string again = library_move(s, ps5 + "/tiny.gguf", "");
    expect(again.rfind("Moved ", 0) == 0, "same size with no spare");
    expect(!std::filesystem::exists(ps5 + "/tiny.gguf"), "same size source gone");
    expect(read_all(home + "/tiny.gguf") == "hello-model", "same size dest kept");

    write_file(ps5 + "/tiny.gguf", "different-contents");
    const std::string clash = library_move(s, ps5 + "/tiny.gguf", "");
    expect(clash.find("different size") != std::string::npos, "different size");
    expect(std::filesystem::exists(ps5 + "/tiny.gguf"), "clash source kept");
    expect(read_all(home + "/tiny.gguf") == "hello-model", "clash dest kept");

    write_file(usb + "/other.gguf", "other-model");
    const std::string loaded = library_move(s, usb + "/other.gguf", usb + "/other.gguf");
    expect(loaded.find("loaded") != std::string::npos, "refuse loaded");
    expect(std::filesystem::exists(usb + "/other.gguf"), "loaded source kept");

    g_fake_free = 0.1;
    const std::string tight = library_move(s, usb + "/other.gguf", "");
    expect(tight.find("not enough free space") != std::string::npos, "tight space");
    expect(std::filesystem::exists(usb + "/other.gguf"), "tight source kept");

    g_fake_free = -1;
    expect(library_move(s, home + "/tiny.gguf", "").find("already") != std::string::npos, "already home");
    expect(library_remove(s, home + "/tiny.gguf", home + "/tiny.gguf").find("Unload") != std::string::npos, "refuse delete");
    expect(std::filesystem::exists(home + "/tiny.gguf"), "running kept");
    const std::string gone = library_remove(s, home + "/tiny.gguf", "");
    expect(gone.rfind("Deleted ", 0) == 0, "deleted");
    expect(!std::filesystem::exists(home + "/tiny.gguf"), "model gone");
    expect(!std::filesystem::exists(home + "/mmproj-tiny.gguf"), "projector gone");
}

}  // namespace

int main() {
    check_paths();
    check_files();
    if (fails) {
        std::printf("library_test: %d failed\n", fails);
        return 1;
    }
    std::printf("library_test: ok\n");
    return 0;
}
