// Corpus tests: compare libsseq against the files Cakewalk Apprentice 1.04 saved for the
// corpus songs (tests/data, see tests/data/README.md).
//
//   sseq_corpus_tests <tests/data dir>
//
// For every song in tests/corpus/songs.cpp:
//   src/NAME.WRK      written by libsseq (gencorpus); must equal what the library writes now
//   app/NAME.WRK      the same song after Apprentice loaded and saved it: byte-identical round
//                     trip, idempotent normalization, and the model must match the song
//   app/NAME.MID      Apprentice's MIDI export of it: our export must have the same content
//   mid2wrk/NAME.WRK  Apprentice's import of that MIDI file: our import must match its model
#include "check.hpp"
#include "corpus/songs.hpp"
#include "sseq/sseq.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace sseq;

namespace {

std::vector<std::uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

long firstDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    const auto [ia, ib] = std::ranges::mismatch(a, b);
    if (ia == a.end() && ib == b.end()) {
        return -1;
    }
    return static_cast<long>(ia - a.begin());
}

// Canonical form of an SMF: per track, the list of (time, bytes) with events at the same tick
// sorted by their bytes, so that the order of simultaneous events does not matter.
using CanonicalEvent = std::pair<Tick, std::vector<std::uint8_t>>;

std::vector<std::vector<CanonicalEvent>> canonicalSmf(const std::vector<std::uint8_t>& b) {
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

/// Apprentice stores a Wave payload in a 32-byte-granular block and saves the whole block.
std::vector<std::uint8_t> padWave(std::vector<std::uint8_t> p) {
    p.resize((p.size() + 31) / 32 * 32, 0);
    return p;
}

bool sameEvent(const Event& x, const Event& y) {
    const bool samePayload =
        x.payload == y.payload ||
        (x.kind == EventKind::Wave && (padWave(x.payload) == y.payload || x.payload == padWave(y.payload)));
    return x.time == y.time && x.kind == y.kind && x.channel == y.channel && x.data1 == y.data1 && x.data2 == y.data2 &&
           x.duration == y.duration && samePayload;
}

/// Events of a track in canonical order: by time, then by a total order on the fields, so that
/// the order of simultaneous events does not matter.
std::vector<Event> canonicalEvents(std::vector<Event> ev) {
    std::ranges::stable_sort(ev, [](const Event& a, const Event& b) {
        return std::tie(a.time, a.kind, a.channel, a.data1, a.data2, a.duration, a.payload) <
               std::tie(b.time, b.kind, b.channel, b.data1, b.data2, b.duration, b.payload);
    });
    return ev;
}

bool sameEvents(const Track& a, const Track& b, const std::string& what) {
    const std::vector<Event> ea = canonicalEvents(a.events);
    const std::vector<Event> eb = canonicalEvents(b.events);
    if (ea.size() != eb.size()) {
        std::printf("FAIL %s: track %d has %zu events, expected %zu\n", what.c_str(), a.number + 1, ea.size(),
                    eb.size());
        return false;
    }
    for (std::size_t i = 0; i < ea.size(); ++i) {
        if (!sameEvent(ea[i], eb[i])) {
            std::printf("FAIL %s: track %d event %zu differs (t=%lld kind=%d d1=%d d2=%d dur=%lld vs t=%lld kind=%d "
                        "d1=%d d2=%d dur=%lld)\n",
                        what.c_str(), a.number + 1, i, static_cast<long long>(ea[i].time), static_cast<int>(ea[i].kind),
                        ea[i].data1, ea[i].data2, static_cast<long long>(ea[i].duration),
                        static_cast<long long>(eb[i].time), static_cast<int>(eb[i].kind), eb[i].data1, eb[i].data2,
                        static_cast<long long>(eb[i].duration));
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): needs #field for the message
#define CHECK_FIELD(what, a, b, field)                                                                                 \
    do {                                                                                                               \
        ++test::checks;                                                                                                \
        if (!((a).field == (b).field)) {                                                                               \
            ++test::failures;                                                                                          \
            std::printf("FAIL %s: %s differs (%s vs %s)\n", (what).c_str(), #field, test::str((a).field).c_str(),      \
                        test::str((b).field).c_str());                                                                 \
        }                                                                                                              \
    } while (0)

std::string opt(const std::optional<int>& v) {
    return v ? std::to_string(*v) : "none";
}
std::string opt(const std::optional<std::uint8_t>& v) {
    return v ? std::to_string(*v) : "none";
}

/// Compares the song Apprentice saved (`got`) with the song it was given (`want`).
void compareWithSource(const std::string& what, const Song& got, const Song& want, const bool fromApprentice) {
    CHECK_FIELD(what, got, want, ppq);
    CHECK_FIELD(what, got, want, tracks.size());
    for (std::size_t i = 0; i < std::min(got.tracks.size(), want.tracks.size()); ++i) {
        const Track& g = got.tracks[i];
        const Track& w = want.tracks[i];
        const std::string where = what + " track " + std::to_string(w.number + 1);
        CHECK_FIELD(where, g, w, number);
        CHECK_FIELD(where, g, w, name);
        CHECK_FIELD(where, g, w, port);
        CHECK_FIELD(where, g, w, keyPlus);
        CHECK_FIELD(where, g, w, velPlus);
        CHECK_FIELD(where, g, w, muted);
        CHECK_FIELD(where, g, w, loop);
        ++test::checks;
        if (opt(g.channel) != opt(w.channel) || opt(g.patch) != opt(w.patch) || opt(g.volume) != opt(w.volume) ||
            opt(g.pan) != opt(w.pan) || opt(g.reps) != opt(w.reps) || opt(g.timePlus) != opt(w.timePlus)) {
            ++test::failures;
            std::printf(
                "FAIL %s: parameters differ (chan %s/%s patch %s/%s vol %s/%s pan %s/%s reps %s/%s time+ %s/%s)\n",
                where.c_str(), opt(g.channel).c_str(), opt(w.channel).c_str(), opt(g.patch).c_str(),
                opt(w.patch).c_str(), opt(g.volume).c_str(), opt(w.volume).c_str(), opt(g.pan).c_str(),
                opt(w.pan).c_str(), opt(g.reps).c_str(), opt(w.reps).c_str(), opt(g.timePlus).c_str(),
                opt(w.timePlus).c_str());
        }
        ++test::checks;
        if (!sameEvents(g, w, where)) {
            ++test::failures;
        }
    }
    CHECK_FIELD(what, got, want, tempoMap.entries.size());
    for (std::size_t i = 0; i < std::min(got.tempoMap.entries.size(), want.tempoMap.entries.size()); ++i) {
        CHECK_FIELD(what, got.tempoMap.entries[i], want.tempoMap.entries[i], time);
        CHECK_FIELD(what, got.tempoMap.entries[i], want.tempoMap.entries[i], bpm100);
    }
    CHECK_FIELD(what, got, want, meterMap.entries.size());
    for (std::size_t i = 0; i < std::min(got.meterMap.entries.size(), want.meterMap.entries.size()); ++i) {
        CHECK_FIELD(what, got.meterMap.entries[i], want.meterMap.entries[i], measure);
        CHECK_FIELD(what, got.meterMap.entries[i], want.meterMap.entries[i], numerator);
        CHECK_FIELD(what, got.meterMap.entries[i], want.meterMap.entries[i], denominatorLog2);
    }
    CHECK_FIELD(what, got, want, keyMap.size());
    for (std::size_t i = 0; i < std::min(got.keyMap.size(), want.keyMap.size()); ++i) {
        CHECK_FIELD(what, got.keyMap[i], want.keyMap[i], measure);
        CHECK_FIELD(what, got.keyMap[i], want.keyMap[i], alterations);
    }
    CHECK_FIELD(what, got, want, markers.size());
    for (std::size_t i = 0; i < std::min(got.markers.size(), want.markers.size()); ++i) {
        if (fromApprentice && want.markers[i].smpteLocked) {
            // Apprentice converts a locked marker to whole frames and back, so its tick moves to
            // the nearest frame boundary.
            const TempoMap& tm = want.tempoMap;
            const Timecode& tc = want.timecode;
            const double fps = tc.dropFrame() ? 30000.0 / 1001.0 : tc.nominalFps();
            const std::int64_t frames = std::llround(tm.tickToSeconds(want.markers[i].time) * fps);
            const Tick expected = tm.secondsToTick(tc.framesToSeconds(frames));
            ++test::checks;
            if (std::llabs(got.markers[i].time - expected) > 1) {
                ++test::failures;
                std::printf("FAIL %s: locked marker %zu at %lld, expected %lld (from %lld)\n", what.c_str(), i,
                            static_cast<long long>(got.markers[i].time), static_cast<long long>(expected),
                            static_cast<long long>(want.markers[i].time));
            }
        } else {
            CHECK_FIELD(what, got.markers[i], want.markers[i], time);
        }
        CHECK_FIELD(what, got.markers[i], want.markers[i], name);
        CHECK_FIELD(what, got.markers[i], want.markers[i], smpteLocked);
    }
    CHECK_FIELD(what, got, want, timecode.offsetFrames);
    CHECK_FIELD(what, got, want, timecode.nominalFps());
    CHECK_FIELD(what, got, want, metronome.port);
    CHECK_FIELD(what, got, want, metronome.channel);
    CHECK_FIELD(what, got, want, metronome.key);
    CHECK_FIELD(what, got, want, metronome.velocity);
    CHECK_FIELD(what, got, want, metronome.duration);
    CHECK_FIELD(what, got, want, thru.mode);
    CHECK_FIELD(what, got, want, thru.port);
    CHECK_FIELD(what, got, want, thru.keyPlus);
    CHECK_FIELD(what, got, want, thru.velPlus);
    CHECK_FIELD(what, got, want, commentsText());
    CHECK_FIELD(what, got, want, sysxBanks.size());
    for (std::size_t i = 0; i < std::min(got.sysxBanks.size(), want.sysxBanks.size()); ++i) {
        CHECK_FIELD(what, got.sysxBanks[i], want.sysxBanks[i], name);
        CHECK_FIELD(what, got.sysxBanks[i], want.sysxBanks[i], data);
    }
    // VARS: the known flags; Apprentice rewrites some other bytes (Now, end-all time, ...).
    CHECK_FIELD(what, got, want, vars.zeroControllers());
    CHECK_FIELD(what, got, want, vars.sendSpp());
    CHECK_FIELD(what, got, want, vars.useContinue());
    CHECK_FIELD(what, got, want, vars.patchSearchBack());
    CHECK_FIELD(what, got, want, vars.metronomePlayback());
    CHECK_FIELD(what, got, want, vars.metronomeRecording());
    CHECK_FIELD(what, got, want, vars.metronomeAccent());
    CHECK_FIELD(what, got, want, vars.countIn());
    CHECK_FIELD(what, got, want, vars.sppLocateDelay());
    CHECK_FIELD(what, got, want, vars.transmitMidiSync());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sseq_corpus_tests <tests/data dir>\n");
        return 2;
    }
    const fs::path data = argv[1];
    const fs::path src = data / "src";
    const fs::path app = data / "app";
    const fs::path mid2wrk = data / "mid2wrk";
    int songs = 0;

    for (const corpus::CorpusSong& c : corpus::allSongs()) {
        const std::string name = c.name;
        const fs::path srcFile = src / (name + ".WRK");
        const fs::path appFile = app / (name + ".WRK");
        const fs::path midFile = app / (name + ".MID");
        const fs::path importFile = mid2wrk / (name + ".WRK");
        if (!fs::exists(srcFile)) {
            std::printf("skip %s: no source file\n", name.c_str());
            continue;
        }
        ++songs;

        // 1. The generator output is reproducible.
        ++test::checks;
        if (wrk::write(c.song, wrk::WriteMode::Normalize) != slurp(srcFile)) {
            ++test::failures;
            std::printf("FAIL %s: src file differs from what the library writes now (regenerate with gencorpus)\n",
                        name.c_str());
        }
        // The source file reads back as the song it was written from.
        compareWithSource(name + " (src)", wrk::readFile(srcFile.string()), c.song, false);

        if (!fs::exists(appFile)) {
            std::printf("ok   %s (source only; no Apprentice files)\n", name.c_str());
            continue;
        }

        // 2. Apprentice's save of the song: byte-identical round trip, idempotent normalization,
        //    and the same model as the source song.
        const std::vector<std::uint8_t> appBytes = slurp(appFile);
        Song appSong;
        try {
            appSong = wrk::read(appBytes);
        } catch (const Error& e) {
            ++test::checks;
            ++test::failures;
            std::printf("FAIL %s: cannot read app file: %s\n", name.c_str(), e.what());
            continue;
        }
        const long d = firstDifference(appBytes, wrk::write(appSong, wrk::WriteMode::Preserve));
        ++test::checks;
        if (d >= 0) {
            ++test::failures;
            std::printf("FAIL %s: round trip of app file differs at byte %ld\n", name.c_str(), d);
        }
        const std::vector<std::uint8_t> n1 = wrk::write(appSong, wrk::WriteMode::Normalize);
        ++test::checks;
        if (wrk::write(wrk::read(n1), wrk::WriteMode::Normalize) != n1) {
            ++test::failures;
            std::printf("FAIL %s: normalize is not idempotent\n", name.c_str());
        }
        compareWithSource(name + " (app)", appSong, c.song, true);

        // 3. Apprentice's MIDI export has the same content as ours.
        if (fs::exists(midFile)) {
            ++test::checks;
            if (canonicalSmf(smf::write(appSong)) != canonicalSmf(slurp(midFile))) {
                ++test::failures;
                std::printf("FAIL %s: SMF export differs from Apprentice's\n", name.c_str());
            }
        }

        // 4. Our import of Apprentice's MIDI file equals Apprentice's own import of it.
        if (fs::exists(midFile) && fs::exists(importFile)) {
            const Song ours = smf::readFile(midFile.string());
            const Song theirs = wrk::readFile(importFile.string());
            const std::string what = name + " (mid2wrk)";
            CHECK_FIELD(what, ours, theirs, tracks.size());
            for (std::size_t i = 0; i < std::min(ours.tracks.size(), theirs.tracks.size()); ++i) {
                const Track& a = ours.tracks[i];
                const Track& b = theirs.tracks[i];
                const std::string where = what + " track " + std::to_string(b.number + 1);
                CHECK_FIELD(where, a, b, number);
                CHECK_FIELD(where, a, b, name);
                CHECK_FIELD(where, a, b, port);
                ++test::checks;
                if (opt(a.channel) != opt(b.channel) || opt(a.patch) != opt(b.patch) ||
                    opt(a.volume) != opt(b.volume) || opt(a.pan) != opt(b.pan)) {
                    ++test::failures;
                    std::printf("FAIL %s: imported parameters differ (chan %s/%s patch %s/%s vol %s/%s pan %s/%s)\n",
                                where.c_str(), opt(a.channel).c_str(), opt(b.channel).c_str(), opt(a.patch).c_str(),
                                opt(b.patch).c_str(), opt(a.volume).c_str(), opt(b.volume).c_str(), opt(a.pan).c_str(),
                                opt(b.pan).c_str());
                }
                ++test::checks;
                if (!sameEvents(a, b, where)) {
                    ++test::failures;
                }
            }
            CHECK_FIELD(what, ours, theirs, markers.size());
            CHECK_FIELD(what, ours, theirs, tempoMap.entries.size());
            CHECK_FIELD(what, ours, theirs, meterMap.entries.size());
            CHECK_FIELD(what, ours, theirs, keyMap.size());
        }
        std::printf("ok   %s (%zu bytes, %zu tracks%s%s)\n", name.c_str(), appBytes.size(), appSong.tracks.size(),
                    fs::exists(midFile) ? ", mid" : "", fs::exists(importFile) ? ", mid2wrk" : "");
    }
    if (songs == 0) {
        std::printf("no corpus files under %s\n", data.string().c_str());
    }
    return test::report("corpus tests");
}
