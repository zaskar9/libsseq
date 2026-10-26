// Edit tests: the Edit menu commands against what Cakewalk Apprentice 1.04 did to the same
// sequence (tests/data/edit, see tests/data/README.md).
//
//   sseq_edit_tests <tests/data dir>
//
// src/QUANT.WRK is the corpus sequence the oracles start from; edit/QUANT_*.WRK are Cakewalk's
// results after Mark > Select All and Edit > Quantize with the settings listed in `quantizeCases`.
#include "check.hpp"
#include "sseq/sseq.hpp"

#include <array>
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
            // Only the fields a kind uses: Cakewalk leaves 1 in the duration field of events it
            // inserts (Controller fill), as its MIDI import does (tests/data/README.md).
            const bool usesData2 =
                x.kind == EventKind::Note || x.kind == EventKind::KeyAftertouch || x.kind == EventKind::Controller;
            const bool same = x.time == y.time && x.kind == y.kind && x.channel == y.channel && x.data1 == y.data1 &&
                              (!usesData2 || x.data2 == y.data2) &&
                              (x.kind != EventKind::Note || x.duration == y.duration);
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

struct LengthCase {
    const char* file;
    bool startTimes;
    bool durations;
    int percent;
    Tick from;
    Tick thru;
};

const std::vector<LengthCase> lengthCases{
    {"LEN_50S", true, false, 50, -1, -1},   {"LEN_150B", true, true, 150, -1, -1},  {"LEN_33B", true, true, 33, -1, -1},
    {"LEN_175D", false, true, 175, -1, -1}, {"LEN_50RG", true, true, 50, 480, 960}, // From 2:01:000 Thru 3:01:000
};

struct TransposeCase {
    const char* file;
    int amount;
    bool diatonic;
};

// On XPOSE.WRK: C major, D major from measure 3, E-flat major from measure 5.
const std::vector<TransposeCase> transposeCases{
    {"XPOSE_P5", 5, false}, {"XPOSE_M7", -7, false}, {"XPOSE_P100", 100, false}, {"XPOSE_M100", -100, false},
    {"XPOSE_D1", 1, true},  {"XPOSE_D2", 2, true},   {"XPOSE_DM3", -3, true},    {"XPOSE_D7", 7, true},
};

struct SlideCase {
    const char* file;
    int amount;
    bool measures;
    Tick from;
    Tick thru;
};

const std::vector<SlideCase> slideCases{
    {"SLIDE_M1", 1, true, -1, -1},          {"SLIDE_T100", 100, false, 480, 960},
    {"SLIDE_TM200", -200, false, 480, 960}, {"SLIDE_TM600", -600, false, 480, 960}, // clamped to -480
    {"SLIDE_MM1", -1, true, -1, -1},                                                // clamped to 0: nothing happens
};

void testLengthArithmetic() {
    CHECK_EQ(edit::scaleTicks(241, 50), 121);
    CHECK_EQ(edit::scaleTicks(723, 50), 362);
    CHECK_EQ(edit::scaleTicks(7, 50), 4);
    CHECK_EQ(edit::scaleTicks(241, 150), 362);
    CHECK_EQ(edit::scaleTicks(1, 150), 2);
    CHECK_EQ(edit::scaleTicks(3, 150), 5);
    CHECK_EQ(edit::scaleTicks(241, 33), 80);
    CHECK_EQ(edit::scaleTicks(1, 33), 0);
    CHECK_EQ(edit::scaleTicks(2, 33), 1);
    CHECK_EQ(edit::scaleTicks(3, 175), 5);
    CHECK_EQ(edit::scaleTicks(47, 175), 82);
    CHECK_EQ(edit::scaleTicks(0, 175), 0);
}

void testTransposeArithmetic() {
    using edit::transposeKey;
    using edit::transposeKeyDiatonic;
    CHECK_EQ(transposeKey(60, 5), 65);
    CHECK_EQ(transposeKey(123, 5), 116);
    CHECK_EQ(transposeKey(127, 100), 119);
    CHECK_EQ(transposeKey(0, -7), 5);
    CHECK_EQ(transposeKey(4, -100), 0);
    CHECK_EQ(transposeKey(0, -100), 8);
    // C major, one step up: scale tones to the next, C# and D# as D and E, F# and G# as F and G, A# as B.
    const std::array<int, 12> cUp1{62, 64, 64, 65, 65, 67, 67, 69, 69, 71, 72, 72};
    for (int k = 0; k < 12; ++k) {
        CHECK_EQ(transposeKeyDiatonic(60 + k, 1, 0), cUp1[static_cast<std::size_t>(k)]);
    }
    CHECK_EQ(transposeKeyDiatonic(70, 2, 0), 74);  // A# as B, two steps: D
    CHECK_EQ(transposeKeyDiatonic(61, -3, 0), 57); // C# as D, three down: A
    CHECK_EQ(transposeKeyDiatonic(60, 7, 0), 72);  // an octave
    CHECK_EQ(transposeKeyDiatonic(61, 7, 0), 74);
    CHECK_EQ(transposeKeyDiatonic(60, 1, 2), 62);    // D major: C (a half-step below the tonic) as C#, up: D
    CHECK_EQ(transposeKeyDiatonic(63, 1, 2), 66);    // D major: D# as E, up: F#
    CHECK_EQ(transposeKeyDiatonic(68, 1, 2), 69);    // D major: G# as G, up: A
    CHECK_EQ(transposeKeyDiatonic(64, 1, -3), 67);   // E-flat major: E as F, up: G
    CHECK_EQ(transposeKeyDiatonic(71, 1, -3), 72);   // E-flat major: B as Bb, up: C
    CHECK_EQ(transposeKeyDiatonic(127, 1, 0), 117);  // G, up: A = 129, folded
    CHECK_EQ(transposeKeyDiatonic(127, 1, -3), 116); // in E-flat: G up: Ab = 128, folded
}

int alterationsAt(const Sequence& s, const Tick time) {
    int a = 0;
    for (const KeyEntry& k : s.keyMap) {
        if (k.measure <= s.meterMap.measureAt(time)) {
            a = k.alterations;
        }
    }
    return a;
}

void testSlideAndSelection() {
    Sequence s;
    Stream& t = s.stream(0);
    t.events.push_back(Event::note(100, 0, 60, 100, 10));
    t.events.push_back(Event::note(200, 0, 62, 100, 10));
    t.events.push_back(Event::note(300, 0, 64, 100, 10));
    edit::Selection sel;
    sel.streams = {0};
    sel.from = 200;
    sel.thru = 300;
    CHECK_EQ(edit::measuresToTicks(s, 0, 2), 960);
    CHECK_EQ(edit::slide(s, sel, {-150, edit::EventFilter::all()}), 2); // moved by -150 (From is 200)
    CHECK_EQ(t.events[0].time, 50);
    CHECK_EQ(t.events[0].data1, 62); // sorted before the untouched note at 100
    CHECK_EQ(t.events[1].time, 100);
    CHECK_EQ(t.events[2].time, 150);
    sel.from = 50;
    CHECK_EQ(edit::slide(s, sel, {-500, edit::EventFilter::all()}), 3); // clamped to -50
    CHECK_EQ(t.events[0].time, 0);
    CHECK_EQ(t.events[2].time, 100);
    CHECK_EQ(alterationsAt(s, 0), 0);
}

void testLengthOracles(const fs::path& data) {
    const Sequence source = wrk::readFile((data / "src" / "QUANT.WRK").string());
    for (const LengthCase& c : lengthCases) {
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
        edit::LengthOptions o;
        o.startTimes = c.startTimes;
        o.durations = c.durations;
        o.percent = c.percent;
        edit::length(ours, sel, o);
        checkSameEvents(ours, wrk::readFile(oracle.string()), c.file);
    }
}

void testTransposeOracles(const fs::path& data) {
    const Sequence source = wrk::readFile((data / "src" / "XPOSE.WRK").string());
    CHECK_EQ(alterationsAt(source, 0), 0);
    CHECK_EQ(alterationsAt(source, 960), 2);
    CHECK_EQ(alterationsAt(source, 1920), -3);
    for (const TransposeCase& c : transposeCases) {
        const fs::path oracle = data / "edit" / (std::string(c.file) + ".WRK");
        if (!fs::exists(oracle)) {
            std::printf("  (no oracle %s)\n", oracle.string().c_str());
            continue;
        }
        Sequence ours = source;
        edit::TransposeOptions o;
        o.amount = c.amount;
        o.diatonic = c.diatonic;
        edit::transpose(ours, edit::Selection::all(ours), o);
        checkSameEvents(ours, wrk::readFile(oracle.string()), c.file);
    }
}

void testSlideOracles(const fs::path& data) {
    const Sequence source = wrk::readFile((data / "src" / "QUANT.WRK").string());
    for (const SlideCase& c : slideCases) {
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
        edit::SlideOptions o;
        o.amount = c.measures ? edit::measuresToTicks(ours, sel.from, c.amount) : c.amount;
        edit::slide(ours, sel, o);
        checkSameEvents(ours, wrk::readFile(oracle.string()), c.file);
    }
}

struct FillCase {
    const char* file;
    int stream;     // stream number of the current track
    int controller; // -1: pitch wheel
    std::uint8_t channel;
    int begin;
    int end;
    Tick from;
    Tick thru;
};

const std::vector<FillCase> fillCases{
    {"CFILL_7", 0, 7, 0, 0, 127, 0, 120},  {"CFILL_PW", 0, -1, 1, -8192, 8191, 0, 480},
    {"CFILL_1", 0, 1, 0, 100, 20, 0, 10},  {"CFILL_7B", 3, 7, 3, 50, 60, 0, 30}, // replaces the CC7 at 17
    {"CFILL_7C", 3, 7, 4, 50, 60, 0, 30},                                        // other channel: the CC7 at 17 stays
    {"CFILL_7D", 0, 7, 0, 127, 0, 0, 120},
};

void testControllerFill() {
    Stream t;
    t.events.push_back(Event::note(0, 0, 60, 100, 10));
    t.events.push_back(Event::controller(5, 0, 7, 99));
    t.events.push_back(Event::controller(5, 1, 7, 98)); // other channel
    t.events.push_back(Event::controller(5, 0, 1, 97)); // other controller
    edit::ControllerFillOptions o;
    o.controller = 7;
    o.channel = 0;
    o.begin = 0;
    o.end = 127;
    o.from = 0;
    o.thru = 120;
    CHECK_EQ(edit::controllerFill(t, o), 121);        // 128 values on 121 ticks: seven share a tick and are skipped
    CHECK(t.events[0].kind == EventKind::Controller); // the fill comes before the note at tick 0
    CHECK_EQ(t.events[0].data2, 0);
    CHECK_EQ(t.events[1].data1, 60);
    CHECK_EQ(t.events[2].time, 1);
    CHECK_EQ(t.events[2].data2, 2);
    int cc7ch0 = 0;
    bool otherChannel = false;
    bool otherController = false;
    for (const Event& e : t.events) {
        if (e.kind == EventKind::Controller && e.data1 == 7 && e.channel == 0) {
            ++cc7ch0;
        }
        otherChannel = otherChannel || (e.channel == 1 && e.data2 == 98);
        otherController = otherController || (e.data1 == 1 && e.data2 == 97);
    }
    CHECK_EQ(cc7ch0, 121);
    CHECK(otherChannel);
    CHECK(otherController);
    // A flat fill: one event.
    Stream u;
    o.begin = 64;
    o.end = 64;
    CHECK_EQ(edit::controllerFill(u, o), 1);
    CHECK_EQ(u.events[0].time, 0);
    CHECK_EQ(u.events[0].data2, 64);
}

void testControllerFillOracles(const fs::path& data) {
    const Sequence source = wrk::readFile((data / "src" / "QUANT.WRK").string());
    for (const FillCase& c : fillCases) {
        const fs::path oracle = data / "edit" / (std::string(c.file) + ".WRK");
        if (!fs::exists(oracle)) {
            std::printf("  (no oracle %s)\n", oracle.string().c_str());
            continue;
        }
        Sequence ours = source;
        edit::ControllerFillOptions o;
        if (c.controller >= 0) {
            o.controller = c.controller;
        }
        o.channel = c.channel;
        o.begin = c.begin;
        o.end = c.end;
        o.from = c.from;
        o.thru = c.thru;
        edit::controllerFill(ours.stream(c.stream), o);
        checkSameEvents(ours, wrk::readFile(oracle.string()), c.file);
    }
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
    testLengthArithmetic();
    testLengthOracles(argv[1]);
    testTransposeArithmetic();
    testTransposeOracles(argv[1]);
    testSlideAndSelection();
    testSlideOracles(argv[1]);
    testControllerFill();
    testControllerFillOracles(argv[1]);
    std::printf("%d checks, %d failures\n", test::checks, test::failures);
    return test::failures == 0 ? 0 : 1;
}
