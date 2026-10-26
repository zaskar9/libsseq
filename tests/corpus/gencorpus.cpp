// gencorpus: writes the corpus source sequences as .WRK files (normalized layout) into a directory
// and prints the sequence list in Markdown.
//
//   gencorpus <output dir>            the corpus (tests/data/src)
//   gencorpus <output dir> engine     the engine test sequences (tests/data/engine/src)
#include "sequences.hpp"
#include "sseq/sseq.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gencorpus <output dir> [engine]\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    std::printf("| File | Description |\n|---|---|\n");
    const bool engine = argc > 2 && std::string(argv[2]) == "engine";
    for (const sseq::corpus::CorpusSequence& c :
         engine ? sseq::corpus::engineSequences() : sseq::corpus::allSequences()) {
        const std::filesystem::path path = out / (c.name + ".WRK");
        sseq::wrk::writeFile(c.sequence, path.string(), sseq::wrk::WriteMode::Normalize);
        std::printf("| `%s.WRK` | %s |\n", c.name.c_str(), c.description.c_str());
    }
    return 0;
}
