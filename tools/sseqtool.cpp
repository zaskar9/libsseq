// sseqtool: command-line front end for libsseq.
//
//   sseqtool dump FILE            chunk / track structure of a .wrk or .mid file
//   sseqtool info FILE            song summary (tracks, tempo, meter, markers)
//   sseqtool roundtrip FILE...    read each .wrk, write it back, report byte differences
//   sseqtool convert IN OUT       .wrk <-> .mid by extension (.wrk written normalized)
//   sseqtool convert --plain IN OUT   same, but a .mid without the Cakewalk port meta event
//   sseqtool normalize IN OUT     rewrite a .wrk in Apprentice order
#include "sseq/sseq.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace sseq;

namespace {

[[nodiscard]] std::vector<std::uint8_t> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error("cannot open " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool endsWith(const std::string& s, const std::string_view suffix) {
    std::string a = s;
    std::ranges::transform(a, a.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.size() >= suffix.size() && a.compare(a.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] bool isMidiFile(const std::string& path) {
    return endsWith(path, ".mid");
}

/// Index of the first differing byte, or -1 when the two vectors are equal.
[[nodiscard]] long firstDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    const auto [ia, ib] = std::ranges::mismatch(a, b);
    if (ia == a.end() && ib == b.end()) {
        return -1;
    }
    return static_cast<long>(ia - a.begin());
}

void info(const Song& s) {
    std::printf("WRK %d.%d, %d ppq, %zu tracks, end %lld ticks (%s)\n", s.versionMajor, s.versionMinor, s.ppq,
                s.tracks.size(), static_cast<long long>(s.endTime()),
                formatMBT(s.meterMap.tickToMBT(s.endTime())).c_str());
    std::printf("tempo:");
    for (const TempoEntry& e : s.tempoMap.entries) {
        std::printf(" %lld:%.2f", static_cast<long long>(e.time), e.bpm());
    }
    std::printf("\n");
    std::printf("meter:");
    for (const MeterEntry& m : s.meterMap.entries) {
        std::printf(" m%d:%d/%d", m.measure, m.numerator, m.denominator());
    }
    std::printf("\n");
    std::printf("timecode: %d fps, offset %lld frames (%s)\n", s.timecode.nominalFps(),
                static_cast<long long>(s.timecode.offsetFrames),
                formatHMSF(s.timecode.framesToHMSF(s.timecode.offsetFrames)).c_str());
    std::printf("metronome: port %d channel %d key %d vel %d dur %d; thru mode %d port %d chan %d\n",
                s.metronome.port + 1, s.metronome.channel + 1, s.metronome.key, s.metronome.velocity,
                s.metronome.duration, s.thru.mode, s.thru.port + 1, s.thru.channel + 1);
    for (const Marker& m : s.markers) {
        std::printf("marker %lld%s '%s'\n", static_cast<long long>(m.time), m.smpteLocked ? " (SMPTE)" : "",
                    m.name.c_str());
    }
    for (const Track& t : s.tracks) {
        std::printf("track %2d '%s' chan %s port %d key%+d vel%+d%s%s", t.number + 1, t.name.c_str(),
                    t.channel ? std::to_string(*t.channel + 1).c_str() : "-", t.port + 1, t.keyPlus, t.velPlus,
                    t.muted ? " muted" : "", t.loop ? " loop" : "");
        if (t.patch) {
            std::printf(" patch %d", *t.patch);
        }
        if (t.volume) {
            std::printf(" vol %d", *t.volume);
        }
        if (t.pan) {
            std::printf(" pan %d", *t.pan);
        }
        if (t.reps) {
            std::printf(" reps %d", *t.reps);
        }
        if (t.timePlus) {
            std::printf(" time%+d", *t.timePlus);
        }
        std::printf(": %zu events, end %s\n", t.events.size(), formatMBT(s.meterMap.tickToMBT(t.endTime())).c_str());
    }
    const std::string c = s.commentsText();
    if (!c.empty()) {
        std::printf("comments: %zu chars\n", c.size());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::fprintf(stderr, "usage: sseqtool dump|info|roundtrip|convert|normalize FILE [OUT]\n");
            return 2;
        }
        const std::string cmd = argv[1];
        if (cmd == "dump") {
            const std::vector<std::uint8_t> b = slurp(argv[2]);
            std::cout << (isMidiFile(argv[2]) ? smf::dump(b) : wrk::dump(b));
        } else if (cmd == "info") {
            info(isMidiFile(argv[2]) ? smf::readFile(argv[2]) : wrk::readFile(argv[2]));
        } else if (cmd == "roundtrip") {
            int failures = 0;
            for (int i = 2; i < argc; ++i) {
                const std::vector<std::uint8_t> in = slurp(argv[i]);
                const std::vector<std::uint8_t> out = wrk::write(wrk::read(in), wrk::WriteMode::Preserve);
                const long d = firstDifference(in, out);
                if (d < 0) {
                    std::printf("ok        %s (%zu bytes)\n", argv[i], in.size());
                } else {
                    std::printf("DIFFERS   %s at byte %ld (in %zu, out %zu)\n", argv[i], d, in.size(), out.size());
                    ++failures;
                }
            }
            return failures != 0 ? 1 : 0;
        } else if (cmd == "convert" || cmd == "normalize") {
            smf::WriteOptions midiOptions;
            int arg = 2;
            if (std::string(argv[arg]) == "--plain") {
                midiOptions.portMeta = false;
                ++arg;
            }
            if (argc < arg + 2) {
                std::fprintf(stderr, "need IN and OUT\n");
                return 2;
            }
            const std::string in = argv[arg];
            const std::string out = argv[arg + 1];
            const Song s = isMidiFile(in) ? smf::readFile(in) : wrk::readFile(in);
            if (isMidiFile(out)) {
                smf::writeFile(s, out, midiOptions);
            } else {
                const bool normalize = cmd == "normalize" || isMidiFile(in);
                wrk::writeFile(s, out, normalize ? wrk::WriteMode::Normalize : wrk::WriteMode::Preserve);
            }
            std::printf("wrote %s\n", out.c_str());
        } else {
            std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
