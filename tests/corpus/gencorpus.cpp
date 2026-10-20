// gencorpus: writes the corpus source songs as .WRK files (normalized layout) into a directory
// and prints the song list in Markdown.
//
//   gencorpus <output dir>
#include "songs.hpp"
#include "sseq/sseq.hpp"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gencorpus <output dir>\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    std::printf("| File | Description |\n|---|---|\n");
    for (const sseq::corpus::CorpusSong& c : sseq::corpus::allSongs()) {
        const std::filesystem::path path = out / (c.name + ".WRK");
        sseq::wrk::writeFile(c.song, path.string(), sseq::wrk::WriteMode::Normalize);
        std::printf("| `%s.WRK` | %s |\n", c.name.c_str(), c.description.c_str());
    }
    return 0;
}
