// Corpus tests: run against the Cakewalk reference files (not part of the repository).
//
//   sseq_corpus_tests <dir>   where <dir> holds expanded/*.WRK and
//                             analysis/wine-exploration/saved-files/*.wrk / *.mid
#include "check.hpp"
#include "sseq/sseq.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sseq;

static std::vector<std::uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static long firstDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    const auto [ia, ib] = std::ranges::mismatch(a, b);
    if (ia == a.end() && ib == b.end()) {
        return -1;
    }
    return static_cast<long>(ia - a.begin());
}

// Canonical form of an SMF: per track, the list of (time, bytes) with events at the same tick
// sorted by their bytes, so that the order of simultaneous events does not matter.
using CanonicalEvent = std::pair<Tick, std::vector<std::uint8_t>>;

static std::vector<std::vector<CanonicalEvent>> canonicalSmf(const std::vector<std::uint8_t>& b) {
    std::vector<std::vector<CanonicalEvent>> out;
    std::size_t o = 14;
    const auto readVlq = [&b](std::size_t& p) {
        std::uint32_t v = 0;
        for (;;) {
            const std::uint8_t c = b[p++];
            v = (v << 7) | (c & 0x7fU);
            if ((c & 0x80U) == 0) {
                return v;
            }
        }
    };
    while (o + 8 <= b.size()) {
        const std::uint32_t len = (std::uint32_t{b[o + 4]} << 24) | (std::uint32_t{b[o + 5]} << 16) |
                                  (std::uint32_t{b[o + 6]} << 8) | std::uint32_t{b[o + 7]};
        std::size_t p = o + 8;
        const std::size_t end = p + len;
        o = end;
        std::vector<CanonicalEvent> ev;
        Tick time = 0;
        int running = -1;
        while (p < end) {
            time += readVlq(p);
            const std::uint8_t c = b[p];
            std::vector<std::uint8_t> bytes;
            if (c == 0xff) {
                std::size_t q = p + 2;
                const std::uint32_t n = readVlq(q);
                bytes.assign(b.begin() + static_cast<std::ptrdiff_t>(p),
                             b.begin() + static_cast<std::ptrdiff_t>(q + n));
                p = q + n;
                running = -1;
            } else {
                std::uint8_t st = 0;
                if ((c & 0x80U) != 0) {
                    st = c;
                    running = c;
                    ++p;
                } else {
                    st = static_cast<std::uint8_t>(running);
                }
                const std::size_t nd = ((st & 0xf0U) == 0xc0 || (st & 0xf0U) == 0xd0) ? 1 : 2;
                bytes.push_back(st);
                bytes.insert(bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(p),
                             b.begin() + static_cast<std::ptrdiff_t>(p + nd));
                p += nd;
            }
            ev.emplace_back(time, std::move(bytes));
        }
        std::ranges::stable_sort(ev);
        out.push_back(std::move(ev));
    }
    return out;
}

static bool sameEvents(const Track& a, const Track& b) {
    if (a.events.size() != b.events.size())
        return false;
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        const Event &x = a.events[i], &y = b.events[i];
        if (x.time != y.time || x.kind != y.kind || x.channel != y.channel || x.data1 != y.data1 ||
            x.data2 != y.data2 || x.duration != y.duration || x.payload != y.payload)
            return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sseq_corpus_tests <data dir>\n");
        return 2;
    }
    fs::path root = argv[1];
    fs::path expanded = root / "expanded", saved = root / "analysis" / "wine-exploration" / "saved-files";
    if (!fs::exists(expanded)) {
        std::printf("no reference files at %s, skipping\n", expanded.string().c_str());
        return 0;
    }

    std::vector<fs::path> wrks;
    for (const fs::path& dir : {expanded, saved})
        if (fs::exists(dir))
            for (auto& e : fs::directory_iterator(dir))
                if (lower(e.path().extension().string()) == ".wrk")
                    wrks.push_back(e.path());
    std::sort(wrks.begin(), wrks.end());
    CHECK(wrks.size() >= 5);

    // 1. Byte-identical round trip in Preserve mode, idempotent Normalize mode.
    for (const fs::path& p : wrks) {
        std::vector<std::uint8_t> in = slurp(p);
        Song s;
        try {
            s = wrk::read(in);
        } catch (const Error& e) {
            std::printf("FAIL read %s: %s\n", p.filename().string().c_str(), e.what());
            ++test::failures;
            continue;
        }
        std::vector<std::uint8_t> out = wrk::write(s, wrk::WriteMode::Preserve);
        const long d = firstDifference(in, out);
        ++test::checks;
        if (d >= 0) {
            ++test::failures;
            std::printf("FAIL round trip %s differs at byte %ld (in %zu, out %zu)\n", p.filename().string().c_str(), d,
                        in.size(), out.size());
        }
        std::vector<std::uint8_t> n1 = wrk::write(s, wrk::WriteMode::Normalize);
        std::vector<std::uint8_t> n2 = wrk::write(wrk::read(n1), wrk::WriteMode::Normalize);
        ++test::checks;
        if (n1 != n2) {
            ++test::failures;
            std::printf("FAIL normalize not idempotent for %s\n", p.filename().string().c_str());
        }
        std::printf("ok %-14s %6zu bytes, %zu tracks, %zu chunks\n", p.filename().string().c_str(), in.size(),
                    s.tracks.size(), s.layout.size());
    }

    // 2. Known content of the original samples.
    Song cw = wrk::readFile((expanded / "CAKEWALK.WRK").string());
    CHECK_EQ(cw.ppq, 120);
    CHECK_EQ(cw.tracks.size(), 7u);
    CHECK_EQ(cw.tempoMap.entries[0].bpm100, 18000);
    CHECK_EQ(cw.tracks[0].name, "Clarinet");
    CHECK_EQ(*cw.tracks[0].channel, 1);
    CHECK_EQ(*cw.tracks[0].patch, 71);
    CHECK_EQ(cw.tracks[1].muted, true);
    CHECK_EQ(cw.tracks[0].events.size(), 156u);
    CHECK_EQ(cw.tracks[0].events[0].data1, 65);
    CHECK_EQ(cw.tracks[0].events[0].duration, 51);
    CHECK_EQ(formatMBT(cw.meterMap.tickToMBT(cw.endTime())), formatMBT(cw.meterMap.tickToMBT(14878)));
    Song row = wrk::readFile((expanded / "ROWROW.WRK").string());
    CHECK_EQ(row.legacyTempoChunk, true);
    CHECK_EQ(row.tempoMap.entries[0].bpm100, 9000);
    CHECK_EQ(row.unknownChunks.size(), 10u); // MEMRGN
    CHECK_EQ(row.tracks[0].name, "Theme");
    Song demo = wrk::readFile((expanded / "DEMO.WRK").string());
    CHECK_EQ(demo.tempoMap.entries.size(), 5u);
    CHECK_EQ(demo.tracks[0].channel.has_value(), false);

    // 3. Observed settings in the probe files.
    if (fs::exists(saved / "out10.wrk")) {
        Song s = wrk::readFile((saved / "out10.wrk").string());
        CHECK_EQ(s.metronome.port, 1);
        CHECK_EQ(s.metronome.channel, 4);
        CHECK_EQ(s.metronome.key, 60);
        CHECK_EQ(s.metronome.velocity, 99);
        CHECK_EQ(s.metronome.duration, 7);
        CHECK_EQ(s.vars.countIn(), 3);
        CHECK_EQ(s.vars.metronomePlayback(), true);
        CHECK_EQ(s.vars.metronomeAccent(), true);
        CHECK_EQ(s.vars.sppLocateDelay(), 5);
        CHECK_EQ(s.vars.zeroControllers(), true);
        CHECK_EQ(s.vars.useContinue(), false);
        CHECK_EQ(s.vars.patchSearchBack(), false);
        CHECK(s.timecode.rate == FrameRate::Fps25);
        CHECK_EQ(s.timecode.offsetFrames, 93075);
        CHECK_EQ(formatHMSF(s.timecode.framesToHMSF(s.timecode.offsetFrames)), "01:02:03:00");
        CHECK_EQ(s.thru.mode, 1);
        CHECK_EQ(s.thru.channel, 6);
        CHECK_EQ(s.thru.keyPlus, 12);
        CHECK_EQ(s.thru.velPlus, 1);
        CHECK_EQ(s.markers.size(), 2u);
        CHECK_EQ(s.markers[0].name, "Verse");
        CHECK_EQ(s.markers[0].time, 1090);
        CHECK_EQ(s.markers[1].smpteLocked, true);
        CHECK_EQ(s.markers[1].time, 3600);
        const Track& t = s.tracks[0];
        CHECK_EQ(t.keyPlus, 5);
        CHECK_EQ(t.loop, true);
        CHECK_EQ(*t.reps, 3);
        CHECK_EQ(*t.timePlus, 6720);
        CHECK_EQ(*t.volume, 100);
        CHECK_EQ(*t.pan, 9);
        CHECK_EQ(t.hasStringEvents(), true);
        CHECK(t.events[0].kind == EventKind::Sysex);
        CHECK(t.events[1].kind == EventKind::MciCommand);
        CHECK_EQ(t.events[1].text(), "play c:\\wcwlite\\test.wav");
        CHECK(t.events[2].kind == EventKind::Text);
        CHECK_EQ(t.events[2].text(), "Hello text event");

        // 4. SMF export must match what Apprentice wrote for the same song.
        for (const char* name : {"out9", "out10"}) {
            fs::path w = saved / (std::string(name) + ".wrk"), m = saved / (std::string(name) + ".mid");
            if (!fs::exists(w) || !fs::exists(m))
                continue;
            std::vector<std::uint8_t> ours = smf::write(wrk::readFile(w.string())), theirs = slurp(m);
            ++test::checks;
            if (canonicalSmf(ours) != canonicalSmf(theirs)) {
                ++test::failures;
                std::printf("FAIL SMF export of %s.wrk differs from %s.mid in content\n", name, name);
            } else {
                const long d = firstDifference(ours, theirs);
                std::printf("ok SMF export of %s.wrk matches Apprentice's %s.mid (%s)\n", name, name,
                            d < 0 ? "byte-identical" : "same events; only the order of simultaneous note-offs differs");
            }
        }

        // 5. SMF import must give the model Apprentice produced when it imported the same file.
        if (fs::exists(saved / "out10.mid") && fs::exists(saved / "out11.wrk")) {
            Song ours = smf::readFile((saved / "out10.mid").string());
            Song theirs = wrk::readFile((saved / "out11.wrk").string());
            CHECK_EQ(ours.tracks.size(), theirs.tracks.size());
            for (std::size_t i = 0; i < std::min(ours.tracks.size(), theirs.tracks.size()); ++i) {
                const Track &a = ours.tracks[i], &b = theirs.tracks[i];
                CHECK_EQ(a.number, b.number);
                CHECK_EQ(a.name, b.name);
                CHECK_EQ(a.port, b.port);
                CHECK_EQ(a.channel.value_or(255), b.channel.value_or(255));
                CHECK_EQ(a.patch.value_or(-1), b.patch.value_or(-1));
                CHECK_EQ(a.volume.value_or(-1), b.volume.value_or(-1));
                CHECK_EQ(a.pan.value_or(-1), b.pan.value_or(-1));
                ++test::checks;
                if (!sameEvents(a, b)) {
                    ++test::failures;
                    std::printf("FAIL imported events of track %d differ (%zu vs %zu)\n", a.number + 1, a.events.size(),
                                b.events.size());
                }
            }
            CHECK_EQ(ours.markers.size(), theirs.markers.size());
            CHECK_EQ(ours.tempoMap.entries[0].bpm100, theirs.tempoMap.entries[0].bpm100);
            CHECK_EQ(ours.meterMap.entries.size(), theirs.meterMap.entries.size());
        }
    }
    return test::report("corpus tests");
}
