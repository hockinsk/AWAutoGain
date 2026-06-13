// airwrap.cpp
// -----------------------------------------------------------------------------
// Generator. Points at a folder of Airwindows VST2 plugins and produces, for
// each one, a "<Name> AG" wrapper (a copy of the shim template) plus a sidecar
// .cfg telling that copy which real plugin to load.
//
//   airwrap --shim <template> --in <airwindows dir> --out <output dir> [--copy]
//
// Searches subfolders. By default the cfg points at the original plugin by
// absolute path, so the output folder contains ONLY wrappers (add just that
// folder to your host's VST scan path). With --copy the originals are bundled
// into <out>/_airwin so the output folder is fully self-contained.
// -----------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

static uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}
static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}
static std::string arg(int argc, char** argv, const std::string& key, const std::string& def = "") {
    for (int i = 1; i < argc - 1; ++i) if (key == argv[i]) return argv[i + 1];
    return def;
}
static bool flag(int argc, char** argv, const std::string& key) {
    for (int i = 1; i < argc; ++i) if (key == argv[i]) return true;
    return false;
}

int main(int argc, char** argv) {
    std::string shim = arg(argc, argv, "--shim");
    std::string in   = arg(argc, argv, "--in");
    std::string out  = arg(argc, argv, "--out");
    bool copyInner   = flag(argc, argv, "--copy");

    if (shim.empty() || in.empty() || out.empty()) {
        std::printf("usage: airwrap --shim <template> --in <dir> --out <dir> [--copy]\n");
        return 1;
    }
    if (!fs::exists(shim)) { std::printf("ERROR: shim template not found: %s\n", shim.c_str()); return 1; }
    if (!fs::exists(in) || !fs::is_directory(in)) {
        std::printf("ERROR: --in folder does not exist or is not a directory:\n  %s\n", in.c_str());
        return 1;
    }

    std::string ext = lower(fs::path(shim).extension().string());   // .dll / .so / .vst
    fs::create_directories(out);
    fs::path outAbs   = fs::absolute(out);
    fs::path innerDir = fs::path(out) / "_airwin";
    if (copyInner) fs::create_directories(innerDir);

    int seen = 0, made = 0;
    std::error_code scanEc;
    for (auto it = fs::recursive_directory_iterator(in, scanEc);
         it != fs::recursive_directory_iterator(); it.increment(scanEc)) {
        if (scanEc) { std::printf("  (skipping unreadable entry: %s)\n", scanEc.message().c_str()); scanEc.clear(); continue; }
        const auto& e = *it;
        if (!e.is_regular_file()) continue;
        if (lower(e.path().extension().string()) != ext) continue;

        // don't recurse into / re-wrap our own output
        if (fs::absolute(e.path().parent_path()) == outAbs) continue;
        std::string name = e.path().stem().string();              // e.g. "ToTape6"
        if (name.size() >= 3 && name.substr(name.size() - 3) == " AG") continue; // already a wrapper

        ++seen;
        std::string wrapName = name + " AG";
        fs::path wrapDll = fs::path(out) / (wrapName + ext);
        fs::path cfgPath = fs::path(out) / (wrapName + ".cfg");

        std::error_code ec;
        fs::copy_file(shim, wrapDll, fs::copy_options::overwrite_existing, ec);
        if (ec) { std::printf("  skip %s (%s)\n", name.c_str(), ec.message().c_str()); continue; }

        std::string target;
        if (copyInner) {
            fs::path dst = innerDir / e.path().filename();
            fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);
            target = "_airwin/" + e.path().filename().string();
        } else {
            target = fs::absolute(e.path()).string();
        }

        std::ofstream cfg(cfgPath, std::ios::binary);
        cfg << "target=" << target << "\n"
            << "name="   << wrapName << "\n"
            << "id=0x"   << std::hex << (fnv1a(wrapName) | 0x1u) << "\n";
        cfg.close();

        ++made;
        std::printf("  wrapped %s\n", name.c_str());
    }

    std::printf("\nfound %d plugin(s) under %s\n", seen, in.c_str());
    std::printf("done: %d wrapper(s) written to %s\n", made, out.c_str());
    if (seen == 0)
        std::printf("NOTE: no '%s' files found. Point --in at the folder that actually\n"
                    "      contains the Airwindows VST2 .dll files.\n", ext.c_str());
    return 0;
}
