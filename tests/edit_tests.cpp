// Edit tests: the Edit menu commands against what Cakewalk Apprentice 1.04 did to the same
// sequence (tests/data/edit, see tests/data/README.md).
//
//   sseq_edit_tests <tests/data dir>
//
// src/QUANT.WRK is the corpus sequence the oracles start from; edit/QUANT_*.WRK are Cakewalk's
// results after Mark > Select All and Edit > Quantize with the settings listed in `quantizeCases`.
#include "check.hpp"
#include "sseq/sseq.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sseq;

namespace {

/// Compares the events of every stream (time, kind, channel, data, duration).
void checkSameEvents(const Sequence& ours, const Sequence& theirs, const std::string& what) {
    CHECK_EQ(ours.streams.size(), theirs.streams.size());
    for (std::size_t i = 0; i < ours.streams.size() && i < theirs.streams.size(); ++i) {
        const Stream& a = ours.streams[i];
        const Stream& b = theirs.streams[i];
        CHECK_EQ(a.events.size(), b.events.size());
        for (std::size_t j = 0; j < a.events.size() && j < b.events.size(); ++j) {
            const Event& x = a.events[j];
            const Event& y = b.events[j];
            const bool same = x.time == y.time && x.kind == y.kind && x.channel == y.channel && x.data1 == y.data1 &&
                              x.data2 == y.data2 && x.duration == y.duration;
            ++test::checks;
            if (!same) {
                ++test::failures;
                std::printf("FAIL %s: stream %d event %zu: ours t=%lld d=%lld, Cakewalk t=%lld d=%lld\n", what.c_str(),
                            a.number + 1, j, static_cast<long long>(x.time), static_cast<long long>(x.duration),
                            static_cast<long long>(y.time), static_cast<long long>(y.duration));
            }
        }
    }
}

struct QuantizeCase {
    const char* file;
    edit::Resolution resolution;
    bool startTimes;
    bool durations;
    int strength;
    Tick from; // -1: Select All
    Tick thru;
};

// The oracle runs: all four tracks selected, Edit > Quantize, OK, Save As.
const std::vector<QuantizeCase> quantizeCases{
    {"QUANT_16", edit::Resolution::Sixteenth, true, true, 100, -1, -1},
    {"QUANT_32", edit::Resolution::ThirtySecond, true, true, 100, -1, -1},
    {"QUANT_8T", edit::Resolution::EighthTriplet, true, true, 100, -1, -1},
    {"QUANT_ST", edit::Resolution::Sixteenth, true, false, 100, -1, -1},
    {"QUANT_DU", edit::Resolution::Sixteenth, false, true, 100, -1, -1},
    {"QUANT_50", edit::Resolution::Sixteenth, true, true, 50, -1, -1},
    {"QUANT_RG", edit::Resolution::Sixteenth, true, true, 100, 480, 960}, // From 2:01:000 Thru 3:01:000
};

void testQuantizeArithmetic() {
    using edit::quantizeDuration;
    using edit::quantizeTime;
    // Nearest multiple, exactly half rounds up (integer half).
    CHECK_EQ(quantizeTime(0, 30), 0);
    CHECK_EQ(quantizeTime(14, 30), 0);
    CHECK_EQ(quantizeTime(15, 30), 30);
    CHECK_EQ(quantizeTime(29, 30), 30);
    CHECK_EQ(quantizeTime(3615, 30), 3630);
    CHECK_EQ(quantizeTime(1687, 15), 1680); // offset 7 of 15 goes down
    CHECK_EQ(quantizeTime(1928, 15), 1935); // offset 8 goes up
    CHECK_EQ(quantizeTime(4579, 40), 4560);
    CHECK_EQ(quantizeTime(4820, 40), 4840);
    // Strength: that percentage of the distance, truncated toward zero.
    CHECK_EQ(quantizeTime(241, 30, 50), 241);
    CHECK_EQ(quantizeTime(723, 30, 50), 722);
    CHECK_EQ(quantizeTime(1205, 30, 50), 1203);
    CHECK_EQ(quantizeTime(3615, 30, 50), 3622);
    CHECK_EQ(quantizeTime(4097, 30, 50), 4103);
    CHECK_EQ(quantizeTime(3615, 30, 0), 3615);
    // Durations: nearest multiple, at least one, minus one tick.
    CHECK_EQ(quantizeDuration(1, 30), 29);
    CHECK_EQ(quantizeDuration(44, 30), 29);
    CHECK_EQ(quantizeDuration(45, 30), 59);
    CHECK_EQ(quantizeDuration(134, 30), 119);
    CHECK_EQ(quantizeDuration(135, 30), 149);
    CHECK_EQ(quantizeDuration(255, 30), 269);
    CHECK_EQ(quantizeDuration(22, 15), 14);
    CHECK_EQ(quantizeDuration(23, 15), 29);
    CHECK_EQ(quantizeDuration(89, 40), 79);
    CHECK_EQ(quantizeDuration(255, 40), 239);
    // Resolutions at 120 ppq.
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::Whole, 120), 480);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::Quarter, 120), 120);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::EighthTriplet, 120), 40);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::Sixteenth, 120), 30);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::SixteenthTriplet, 120), 20);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::ThirtySecond, 120), 15);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::ThirtySecondTriplet, 120), 10);
    CHECK_EQ(edit::resolutionTicks(edit::Resolution::ThirtySecondTriplet, 480), 40);
}

void testQuantizeSelection() {
    Sequence s;
    Stream& t = s.stream(0);
    t.events.push_back(Event::note(29, 0, 60, 100, 10));
    t.events.push_back(Event::note(30, 0, 62, 100, 10));
    t.events.push_back(Event::controller(29, 0, 7, 100));
    Stream& u = s.stream(1);
    u.events.push_back(Event::note(29, 0, 64, 100, 10));
    edit::Selection sel;
    sel.streams = {0};
    sel.from = 0;
    sel.thru = 29;
    edit::QuantizeOptions o;
    o.resolution = 30;
    CHECK_EQ(edit::quantize(s, sel, o), 1);
    CHECK(s.stream(0).events[0].kind == EventKind::Controller); // stayed at 29, now first
    CHECK_EQ(s.stream(0).events[1].time, 30);
    CHECK_EQ(s.stream(0).events[1].data1, 60); // moved note sorted after the controller, before the other note
    CHECK_EQ(s.stream(0).events[2].data1, 62);
    CHECK_EQ(s.stream(0).events[2].duration, 10); // outside Thru
    CHECK_EQ(s.stream(1).events[0].time, 29);     // stream not selected
    // The event filter brings other kinds in; durations stay note-only.
    o.filter = edit::EventFilter::all();
    sel.thru = 30;
    CHECK_EQ(edit::quantize(s, sel, o), 2);
    CHECK_EQ(s.stream(0).events[0].time, 30);
    CHECK(s.stream(0).events[0].kind == EventKind::Controller);
    CHECK_EQ(s.stream(0).events[2].duration, 29);
    // Select All covers every stream with events through the end.
    const edit::Selection all = edit::Selection::all(s);
    CHECK_EQ(all.streams.size(), std::size_t{2});
    CHECK_EQ(all.from, 0);
    CHECK_EQ(all.thru, s.endTime());
}

void testQuantizeOracles(const fs::path& data) {
    const Sequence source = wrk::readFile((data / "src" / "QUANT.WRK").string());
    for (const QuantizeCase& c : quantizeCases) {
        const fs::path oracle = data / "edit" / (std::string(c.file) + ".WRK");
        if (!fs::exists(oracle)) {
            std::printf("  (no oracle %s)\n", oracle.string().c_str());
            continue;
        }
        Sequence ours = source;
        edit::Selection sel = edit::Selection::all(ours);
        if (c.from >= 0) {
            sel.from = c.from;
            sel.thru = c.thru;
        }
        edit::QuantizeOptions o;
        o.resolution = edit::resolutionTicks(c.resolution, ours.ppq);
        o.startTimes = c.startTimes;
        o.durations = c.durations;
        o.strength = c.strength;
        edit::quantize(ours, sel, o);
        checkSameEvents(ours, wrk::readFile(oracle.string()), c.file);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sseq_edit_tests <tests/data dir>\n");
        return 2;
    }
    testQuantizeArithmetic();
    testQuantizeSelection();
    testQuantizeOracles(argv[1]);
    std::printf("%d checks, %d failures\n", test::checks, test::failures);
    return test::failures == 0 ? 0 : 1;
}
