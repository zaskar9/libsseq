// Engine tests: deterministic playback through a fake clock and a recording sink.
#include "check.hpp"
#include "corpus/songs.hpp"
#include "sseq/sseq.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace sseq;
using namespace sseq::engine;

namespace {

class FakeClock final : public Clock {
public:
    double time = 0;
    [[nodiscard]] double now() const override { return time; }
};

struct SysexRecord {
    double time;
    int port;
    std::vector<std::uint8_t> bytes;
};

class RecordingSink final : public MidiSink {
public:
    std::vector<MidiMessage> messages;
    std::vector<SysexRecord> sysex;
    int flushes = 0;
    void send(const MidiMessage& m) override { messages.push_back(m); }
    void sendSysex(const double time, const int port, const std::vector<std::uint8_t>& bytes) override {
        sysex.push_back({time, port, bytes});
    }
    void flush() override { ++flushes; }
};

bool near(const double a, const double b) {
    return std::fabs(a - b) < 1e-9;
}

std::string hex(const MidiMessage& m) {
    std::string s;
    for (std::uint8_t i = 0; i < m.size; ++i) {
        std::array<char, 4> buf{};
        std::snprintf(buf.data(), buf.size(), "%02x", m.data[i]);
        s += buf.data();
        if (i + 1 < m.size) {
            s += ' ';
        }
    }
    return s;
}

/// A song at 120 bpm, 120 ppq (240 ticks per second): one track, three quarter notes.
Song simpleSong() {
    Song s;
    Track& t = s.track(0);
    t.name = "Lead";
    t.channel = 2;
    t.patch = 40;
    t.volume = 100;
    t.pan = 64;
    t.events.push_back(Event::note(0, 2, 60, 100, 120));
    t.events.push_back(Event::note(120, 2, 62, 90, 120));
    t.events.push_back(Event::note(240, 2, 64, 80, 120));
    return s;
}

// ---------------------------------------------------------------- tests

void testRenderSimple() {
    const std::vector<MidiMessage> out = render(simpleSong());
    // patch, volume, pan, then note-on/off pairs
    CHECK_EQ(out.size(), 9U);
    CHECK_EQ(hex(out[0]), "c2 28");
    CHECK_EQ(hex(out[1]), "b2 07 64");
    CHECK_EQ(hex(out[2]), "b2 0a 40");
    CHECK_EQ(hex(out[3]), "92 3c 64");
    CHECK(near(out[3].time, 0.0));
    CHECK_EQ(hex(out[4]), "92 3c 00");
    CHECK(near(out[4].time, 0.5));
    CHECK_EQ(hex(out[5]), "92 3e 5a");
    CHECK(near(out[5].time, 0.5));
    CHECK_EQ(hex(out[8]), "92 40 00");
    CHECK(near(out[8].time, 1.5));
    // Note-off before the next note-on at the same tick.
    CHECK(out[4].isNoteOff());
    CHECK(out[5].isNoteOn());
}

void testTrackParametersAndTransforms() {
    Song s = simpleSong();
    Track& t = s.track(0);
    t.keyPlus = 12;
    t.velPlus = -30;
    t.timePlus = 60;
    t.port = 1;
    t.channel = 5;
    const std::vector<MidiMessage> out = render(s);
    CHECK_EQ(out.size(), 9U);
    CHECK_EQ(out[0].port, 1);
    CHECK_EQ(hex(out[0]), "c5 28");
    CHECK_EQ(hex(out[3]), "95 48 46"); // 60+12, 100-30
    CHECK(near(out[3].time, 0.25));    // Time+ 60 ticks = 0.25 s
    CHECK_EQ(out[3].port, 1);
    // Velocity never drops to 0 (which would be a note-off).
    t.velPlus = -127;
    const std::vector<MidiMessage> quiet = render(s);
    CHECK_EQ(hex(quiet[3]), "95 48 01");
}

void testMuteAndSolo() {
    Song s = simpleSong();
    Track& u = s.track(1);
    u.name = "Second";
    u.channel = 3;
    u.events.push_back(Event::note(0, 3, 48, 100, 480));
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendTrackParameters = false;
    Transport tr(s, clock, sink, o);
    tr.setMuted(0, true);
    tr.play();
    clock.time = 10;
    tr.pump();
    CHECK(std::ranges::all_of(sink.messages, [](const MidiMessage& m) { return m.channel() == 3; }));
    CHECK_EQ(sink.messages.size(), 2U);
    tr.stop();

    RecordingSink sink2;
    Transport tr2(s, clock, sink2, o);
    tr2.setSolo(0, true); // solo wins over the mute set
    tr2.setMuted(0, true);
    CHECK(tr2.isAudible(0));
    CHECK(!tr2.isAudible(1));
    clock.time = 0;
    tr2.play();
    clock.time = 10;
    tr2.pump();
    CHECK(std::ranges::all_of(sink2.messages, [](const MidiMessage& m) { return m.channel() == 2; }));
    CHECK_EQ(sink2.messages.size(), 6U);
}

void testLookAheadAndPump() {
    const Song s = simpleSong();
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.lookAhead = 0.1;
    o.sendTrackParameters = false;
    Transport tr(s, clock, sink, o);
    clock.time = 5.0;
    tr.play(); // schedules up to 5.1: only the first note-on (at 5.0)
    CHECK_EQ(sink.messages.size(), 1U);
    CHECK(near(sink.messages[0].time, 5.0));
    clock.time = 5.45;
    CHECK(tr.pump()); // up to 5.55: the first note-off and the second note-on at 5.5
    CHECK_EQ(sink.messages.size(), 3U);
    CHECK(near(sink.messages[1].time, 5.5));
    CHECK(tr.playing());
    CHECK_EQ(tr.now(), 108); // 0.45 s = 108 ticks
    clock.time = 6.4;
    CHECK(tr.pump()); // everything scheduled (last off at 6.5) but the end is not reached yet
    CHECK_EQ(sink.messages.size(), 6U);
    CHECK(tr.playing());
    clock.time = 6.5;
    CHECK(!tr.pump()); // end reached: stopped by itself
    CHECK(!tr.playing());
    CHECK_EQ(tr.now(), 360);
    CHECK(sink.flushes >= 3);
    // Every message exactly once, in time order.
    for (std::size_t i = 1; i < sink.messages.size(); ++i) {
        CHECK(sink.messages[i].time >= sink.messages[i - 1].time);
    }
}

void testStopCutsNotesAndZeroesControllers() {
    const Song s = simpleSong();
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendTrackParameters = false;
    o.zeroControllersOnStop = true;
    o.lookAhead = 0.05;
    Transport tr(s, clock, sink, o);
    tr.play();
    clock.time = 0.2; // inside the first note
    tr.pump();
    CHECK_EQ(sink.messages.size(), 1U);
    tr.stop();
    CHECK(!tr.playing());
    CHECK_EQ(tr.now(), 48); // 0.2 s = 48 ticks
    // note-off at 0.2, then mod wheel, sustain, pitch wheel, channel pressure
    CHECK_EQ(sink.messages.size(), 6U);
    CHECK_EQ(hex(sink.messages[1]), "92 3c 00");
    CHECK(near(sink.messages[1].time, 0.2));
    CHECK_EQ(hex(sink.messages[2]), "b2 01 00");
    CHECK_EQ(hex(sink.messages[3]), "b2 40 00");
    CHECK_EQ(hex(sink.messages[4]), "e2 00 40");
    CHECK_EQ(hex(sink.messages[5]), "d2 00");
    // Resuming continues from the stop position (tick 48, inside the first note): the first note
    // is not repeated, and the second note-on at tick 120 comes 72 ticks = 0.3 s after the restart.
    clock.time = 1.0;
    tr.play();
    CHECK_EQ(sink.messages.size(), 6U); // nothing due within the look-ahead yet
    clock.time = 1.3;
    tr.pump();
    CHECK_EQ(sink.messages.size(), 7U);
    CHECK_EQ(hex(sink.messages[6]), "92 3e 5a");
    CHECK(near(sink.messages[6].time, 1.3));
    tr.stop();
}

void testSeekAndChase() {
    Song s = simpleSong();
    Track& t = s.track(0);
    t.events.push_back(Event::patch(60, 2, 7));
    t.events.push_back(Event::controller(100, 2, 1, 50));
    t.events.push_back(Event::controller(110, 2, 1, 70));
    t.events.push_back(Event::pitchWheel(115, 2, 1000));
    sortEvents(t.events);
    PlaybackOptions o;
    o.chase = true;
    // Start inside the second note: the first note is skipped, chased state precedes the notes.
    const std::vector<MidiMessage> out = render(s, o, 130);
    CHECK_EQ(hex(out[0]), "c2 28"); // track parameters
    CHECK_EQ(hex(out[1]), "b2 07 64");
    CHECK_EQ(hex(out[2]), "b2 0a 40");
    // chased, sorted by (port, status, controller): b2 01 -> 70, c2 -> 7, e2 -> 1000
    CHECK_EQ(hex(out[3]), "b2 01 46");
    CHECK_EQ(hex(out[4]), "c2 07");
    CHECK_EQ(hex(out[5]), "e2 68 47");
    CHECK(near(out[5].time, 0.0));
    // The second note started before the position, so it is not played and gets no note-off;
    // the third note follows.
    CHECK_EQ(hex(out[6]), "92 40 50");
    CHECK(near(out[6].time, (240 - 130) / 240.0));
    CHECK_EQ(hex(out[7]), "92 40 00");
    o.chase = false;
    const std::vector<MidiMessage> plain = render(s, o, 130);
    CHECK_EQ(plain.size(), 5U); // 3 parameters, the third note's on and off
}

void testTempoChangeAndRatio() {
    Song s = simpleSong();
    s.tempoMap.set(120, 6000); // from the second note on: 60 bpm = 120 ticks per second
    PlaybackOptions o;
    o.sendTrackParameters = false;
    std::vector<MidiMessage> out = render(s, o);
    CHECK(near(out[0].time, 0.0));
    CHECK(near(out[2].time, 0.5)); // second note-on at tick 120
    CHECK(near(out[4].time, 1.5)); // third note-on at tick 240: 0.5 + 120 ticks at 60 bpm
    CHECK(near(out[5].time, 2.5));
    o.tempoRatio = 2.0;
    out = render(s, o);
    CHECK(near(out[5].time, 1.25));
}

void testLoop() {
    const Song s = simpleSong();
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendTrackParameters = false;
    o.loopFrom = 0;
    o.loopThru = 240; // the first two notes, one second
    o.lookAhead = 0.3;
    Transport tr(s, clock, sink, o);
    tr.play();
    for (int step = 0; step < 25; ++step) {
        clock.time = step * 0.1;
        CHECK(tr.pump());
    }
    // Two full passes and part of a third have been scheduled; the third note never plays.
    CHECK(std::ranges::none_of(sink.messages, [](const MidiMessage& m) { return m.data[1] == 64; }));
    std::vector<double> onsets;
    for (const MidiMessage& m : sink.messages) {
        if (m.isNoteOn() && m.data[1] == 60) {
            onsets.push_back(m.time);
        }
    }
    CHECK(onsets.size() >= 3);
    CHECK(near(onsets[0], 0.0));
    CHECK(near(onsets[1], 1.0));
    CHECK(near(onsets[2], 2.0));
    CHECK(tr.playing());
    tr.stop();
}

void testSysexAndStringEvents() {
    Song s = simpleSong();
    SysxBank bank;
    bank.name = "reset";
    bank.data = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    s.sysxBanks.push_back(bank);
    Track& t = s.track(0);
    t.events.push_back(Event::sysex(120, 0));
    t.events.push_back(Event::textEvent(240, "verse"));
    t.events.push_back(Event::mciCommand(240, "play x"));
    sortEvents(t.events);
    FakeClock clock;
    RecordingSink sink;
    Transport tr(s, clock, sink);
    std::vector<std::pair<std::string, double>> strings;
    tr.setStringEventHandler([&](const int track, const Event& e, const double time) {
        CHECK_EQ(track, 0);
        strings.emplace_back(e.text(), time);
    });
    tr.play();
    clock.time = 5;
    tr.pump();
    CHECK_EQ(sink.sysex.size(), 1U);
    CHECK(near(sink.sysex[0].time, 0.5));
    CHECK_EQ(sink.sysex[0].bytes.size(), 6U);
    CHECK_EQ(strings.size(), 2U);
    CHECK_EQ(strings[0].first, "verse");
    CHECK(near(strings[0].second, 1.0));
    CHECK_EQ(strings[1].first, "play x");
}

void testTrackLoopAndCorpus() {
    // A looped track repeats with a whole-measure period.
    Song s;
    Track& t = s.track(0);
    t.channel = 0;
    t.reps = 3;
    t.events.push_back(Event::note(0, 0, 60, 100, 300)); // ends at 300, inside measure 1 -> period 480
    PlaybackOptions o;
    o.sendTrackParameters = false;
    const std::vector<MidiMessage> out = render(s, o);
    CHECK_EQ(out.size(), 6U);
    CHECK(near(out[2].time, 2.0)); // second repetition at tick 480
    CHECK(near(out[4].time, 4.0));

    // The corpus songs render: one note-on and one note-off per note, times monotonic.
    for (const corpus::CorpusSong& c : corpus::allSongs()) {
        const std::vector<MidiMessage> r = render(c.song, o);
        std::size_t notes = 0;
        for (const Track& tr : c.song.tracks) {
            const int reps = std::max(1, tr.reps.value_or(1));
            if (tr.muted) {
                continue;
            }
            const Tick shift = tr.timePlus.value_or(0);
            notes +=
                static_cast<std::size_t>(reps) *
                static_cast<std::size_t>(std::ranges::count_if(tr.events, [shift](const Event& e) {
                    return e.kind == EventKind::Note && e.time + shift >= 0; // Time- can push a note before the start
                }));
        }
        const auto ons = static_cast<std::size_t>(std::ranges::count_if(r, &MidiMessage::isNoteOn));
        const auto offs = static_cast<std::size_t>(std::ranges::count_if(r, &MidiMessage::isNoteOff));
        CHECK_EQ(ons, notes);
        CHECK_EQ(offs, notes);
        CHECK(std::ranges::is_sorted(r, {}, &MidiMessage::time));
    }
}

} // namespace

int main() {
    testRenderSimple();
    testTrackParametersAndTransforms();
    testMuteAndSolo();
    testLookAheadAndPump();
    testStopCutsNotesAndZeroesControllers();
    testSeekAndChase();
    testTempoChangeAndRatio();
    testLoop();
    testSysexAndStringEvents();
    testTrackLoopAndCorpus();
    return test::report("engine tests");
}
