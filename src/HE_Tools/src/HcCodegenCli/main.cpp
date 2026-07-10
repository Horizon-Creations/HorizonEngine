// hc_codegen — the HorizonCode → C++ generation CLI (plan §10.2).
//
//   hc_codegen --fixtures --out DIR          generate the parity-test fixtures
//   hc_codegen --out DIR file.hcode.json...  generate from graph JSON files
//
// Used at build time to compile the test fixtures into he_tests, and handy for
// eyeballing emission during development. The editor export worker calls the
// HE::hccg library in-process instead.

#include <HorizonScene/HcCodegen.h>
#include "fixtures.h"   // tests/fixtures/hcodegen (target include dir)
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool writeIfChanged(const fs::path& path, const std::string& contents)
{
    // Leave up-to-date files untouched so downstream builds stay incremental.
    {
        std::ifstream in(path, std::ios::binary);
        if (in)
        {
            std::stringstream ss;
            ss << in.rdbuf();
            if (ss.str() == contents) return true;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << contents;
    return (bool)out;
}

int main(int argc, char** argv)
{
    std::string outDir;
    bool fixtures = false;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--fixtures") fixtures = true;
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else inputs.push_back(a);
    }
    if (outDir.empty() || (!fixtures && inputs.empty()))
    {
        std::fprintf(stderr, "usage: hc_codegen [--fixtures] --out DIR [graph.json...]\n");
        return 2;
    }

    std::vector<HE::hccg::ClassSource> sources;
    if (fixtures)
        sources = hcfix::all();
    for (const std::string& file : inputs)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) { std::fprintf(stderr, "hc_codegen: cannot read %s\n", file.c_str()); return 1; }
        std::stringstream ss;
        ss << in.rdbuf();
        HorizonCode::Graph g;
        if (!HorizonCode::fromJson(ss.str(), g))
        { std::fprintf(stderr, "hc_codegen: invalid graph json %s\n", file.c_str()); return 1; }
        const std::string stem = fs::path(file).stem().string();
        sources.push_back({ stem, fs::path(file).filename().string(), std::move(g) });
    }

    HE::hccg::Options opt;
    opt.engineVersion = "dev";
    const HE::hccg::Result res = HE::hccg::generate(sources, opt);
    for (const auto& w : res.warnings)
        std::fprintf(stderr, "hc_codegen: warning: %s\n", w.c_str());
    for (const auto& fb : res.fallbacks)
        std::fprintf(stderr, "hc_codegen: fallback: %s: %s\n", fb.key.c_str(), fb.reason.c_str());
    if (!res.ok) { std::fprintf(stderr, "hc_codegen: generation failed\n"); return 1; }
    // Fixtures must all compile — a fallback there is a build error by design.
    if (fixtures && !res.fallbacks.empty()) return 1;

    std::error_code ec;
    fs::create_directories(outDir, ec);
    for (const auto& f : res.files)
        if (!writeIfChanged(fs::path(outDir) / f.name, f.contents))
        { std::fprintf(stderr, "hc_codegen: cannot write %s\n", f.name.c_str()); return 1; }
    return 0;
}
