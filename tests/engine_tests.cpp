// Engine tests: deterministic playback through a fake clock and a recording sink.
#include "check.hpp"
#include "corpus/sequences.hpp"
#include "sseq/sseq.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
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
    int discards = 0;
    void send(const MidiMessage& m) override { messages.push_back(m); }
    void sendSysex(const double time, const int port, const std::vector<std::uint8_t>& bytes) override {
        sysex.push_back({time, port, bytes});
    }
    void flush() override { ++flushes; }
    /// Drops messages whose time is still ahead, as a real output does on discardScheduled().
    void discardScheduled() override {
        ++discards;
        std::erase_if(messages, [&](const MidiMessage& m) { return m.time > now; });
    }
    double now = 0; // the test sets this to the clock before stopping
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

/// A sequence at 120 bpm, 120 ppq (240 ticks per second): one stream, three quarter notes.
Sequence simpleSequence() {
    Sequence s;
    Stream& t = s.stream(0);
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
    const std::vector<MidiMessage> out = render(simpleSequence());
    // patch, volume, pan, then note-on/off pairs, then the stop at the end: All Notes Off on 16 channels
    CHECK_EQ(out.size(), 9U + 16U);
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
    CHECK_EQ(hex(out[9]), "b0 7b 00");
    CHECK_EQ(hex(out[24]), "bf 7b 00");
    CHECK(near(out[24].time, 1.5));
}

void testStreamParametersAndTransforms() {
    Sequence s = simpleSequence();
    Stream& t = s.stream(0);
    t.keyPlus = 12;
    t.velPlus = -30;
    t.timePlus = 60;
    t.port = 1;
    t.channel = 5;
    const std::vector<MidiMessage> out = render(s);
    CHECK_EQ(out.size(), 9U + 16U);
    CHECK_EQ(out[0].port, 1);
    CHECK_EQ(out.back().port, 1); // All Notes Off goes to the ports in use
    CHECK_EQ(hex(out[0]), "c5 28");
    CHECK_EQ(hex(out[3]), "95 48 46"); // 60+12, 100-30
    CHECK(near(out[3].time, 0.25));    // Time+ 60 ticks = 0.25 s
    CHECK_EQ(out[3].port, 1);
    // Velocity never drops to 0 (which would be a note-off).
    t.velPlus = -127;
    const std::vector<MidiMessage> quiet = render(s);
    CHECK_EQ(hex(quiet[3]), "95 48 01");
    // Key+ beyond 0..127 folds by octaves; Time- before the start plays at the start.
    t.velPlus = 0;
    t.keyPlus = 70; // 60 + 70 = 130 -> 118
    t.timePlus = -60;
    const std::vector<MidiMessage> folded = render(s);
    CHECK_EQ(hex(folded[3]), "95 76 64");
    CHECK(near(folded[3].time, 0.0));
    // the first note keeps its duration from the start: its note-off at tick 120 = 0.5 s
    CHECK(std::ranges::any_of(folded, [](const MidiMessage& m) { return hex(m) == "95 76 00" && near(m.time, 0.5); }));
    t.keyPlus = -70; // 60 - 70 = -10 -> 2
    CHECK_EQ(hex(render(s)[3]), "95 02 64");
}

void testMuteAndSolo() {
    Sequence s = simpleSequence();
    Stream& u = s.stream(1);
    u.name = "Second";
    u.channel = 3;
    u.events.push_back(Event::note(0, 3, 48, 100, 480));
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendStreamParameters = false;
    Transport tr(s, clock, sink, o);
    tr.setMuted(0, true);
    tr.play();
    clock.time = 10;
    tr.pump();
    const auto notes = [](const std::vector<MidiMessage>& v) {
        std::vector<MidiMessage> out;
        std::ranges::copy_if(v, std::back_inserter(out),
                             [](const MidiMessage& m) { return (m.status() & 0xf0) == 0x90; });
        return out;
    };
    CHECK(std::ranges::all_of(notes(sink.messages), [](const MidiMessage& m) { return m.channel() == 3; }));
    CHECK_EQ(notes(sink.messages).size(), 2U);
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
    CHECK(std::ranges::all_of(notes(sink2.messages), [](const MidiMessage& m) { return m.channel() == 2; }));
    CHECK_EQ(notes(sink2.messages).size(), 6U);
}

void testLookAheadAndPump() {
    const Sequence s = simpleSequence();
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.lookAhead = 0.1;
    o.sendStreamParameters = false;
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
    CHECK(!tr.pump()); // end reached: stopped by itself, with what a stop sends
    CHECK(!tr.playing());
    CHECK_EQ(tr.now(), 360);
    CHECK_EQ(sink.messages.size(), 6U + 16U);
    CHECK_EQ(hex(sink.messages.back()), "bf 7b 00");
    CHECK(sink.flushes >= 3);
    // Every message exactly once, in time order.
    for (std::size_t i = 1; i < sink.messages.size(); ++i) {
        CHECK(sink.messages[i].time >= sink.messages[i - 1].time);
    }
}

void testStopCutsNotesAndZeroesControllers() {
    const Sequence s = simpleSequence();
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendStreamParameters = false;
    o.zeroControllersOnStop = true;
    o.lookAhead = 0.05;
    Transport tr(s, clock, sink, o);
    tr.play();
    clock.time = 0.2; // inside the first note
    tr.pump();
    CHECK_EQ(sink.messages.size(), 1U);
    sink.now = 0.2;
    tr.stop();
    CHECK_EQ(sink.discards, 1); // pending future messages are dropped before the note-offs
    CHECK(!tr.playing());
    CHECK_EQ(tr.now(), 48); // 0.2 s = 48 ticks
    // Per channel: the sounding notes' offs, then All Notes Off; then on every channel the reset:
    // pitch wheel centred, modulation zero, sustain up, reset all controllers.
    CHECK_EQ(sink.messages.size(), 1U + 1U + 16U + 16U * 4U);
    CHECK_EQ(hex(sink.messages[1]), "b0 7b 00");
    CHECK_EQ(hex(sink.messages[2]), "b1 7b 00");
    CHECK_EQ(hex(sink.messages[3]), "92 3c 00");
    CHECK(near(sink.messages[3].time, 0.2));
    CHECK_EQ(hex(sink.messages[4]), "b2 7b 00");
    CHECK_EQ(hex(sink.messages[18]), "e0 00 40");
    CHECK_EQ(hex(sink.messages[19]), "b0 01 00");
    CHECK_EQ(hex(sink.messages[20]), "b0 40 00");
    CHECK_EQ(hex(sink.messages[21]), "b0 79 00");
    CHECK_EQ(hex(sink.messages.back()), "bf 79 00");
    CHECK(near(sink.messages.back().time, 0.2));
    // Resuming continues from the stop position (tick 48, inside the first note): the first note
    // is not repeated, and the second note-on at tick 120 comes 72 ticks = 0.3 s after the restart.
    const std::size_t before = sink.messages.size();
    clock.time = 1.0;
    tr.play();
    CHECK_EQ(sink.messages.size(), before); // nothing due within the look-ahead yet
    clock.time = 1.3;
    tr.pump();
    CHECK_EQ(sink.messages.size(), before + 1);
    CHECK_EQ(hex(sink.messages[before]), "92 3e 5a");
    CHECK(near(sink.messages[before].time, 1.3));
    tr.stop();
    // Panic while stopped: All Notes Off and the controller reset on every channel.
    sink.messages.clear();
    tr.panic();
    CHECK_EQ(sink.messages.size(), 16U + 16U * 4U);
    CHECK_EQ(hex(sink.messages[0]), "b0 7b 00");
    CHECK_EQ(hex(sink.messages[16]), "e0 00 40");
}

/// Stopping while a note-on is already scheduled ahead (within the look-ahead): the output is told
/// to discard what is pending, and the note-offs are sent for sounding notes, so nothing hangs.
void testStopDiscardsScheduledNotes() {
    const Sequence s = simpleSequence(); // notes at 0, 120, 240 ticks = 0, 0.5, 1.0 s
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendStreamParameters = false;
    o.lookAhead = 0.2;
    Transport tr(s, clock, sink, o);
    tr.play();
    clock.time = 0.35; // horizon 0.55: the second note-on (0.5) is scheduled although not yet due
    tr.pump();
    CHECK_EQ(sink.messages.size(), 3U); // on(0), off(0.5), on(0.5)
    sink.now = 0.35;
    tr.stop();
    CHECK_EQ(sink.discards, 1);
    // What the output still holds: the first note-on and the note-offs the stop sent at 0.35, for
    // both notes the transport considered sounding; no note-on with a time after the stop.
    bool futureOn = false;
    for (const MidiMessage& m : sink.messages) {
        if (m.isNoteOn() && m.time > 0.35) {
            futureOn = true;
        }
    }
    CHECK(!futureOn);
    // The first note's note-off (at 0.5) was dropped with the rest, so the stop ends that note; the
    // second note never started and gets no note-off.
    std::vector<std::string> offsAtStop;
    for (const MidiMessage& m : sink.messages) {
        if (m.isNoteOff() && near(m.time, 0.35)) {
            offsAtStop.push_back(hex(m));
        }
    }
    CHECK_EQ(offsAtStop.size(), 1U);
    CHECK_EQ(offsAtStop.front(), "92 3c 00");
}

void testSeekAndChase() {
    Sequence s = simpleSequence();
    Stream& t = s.stream(0);
    t.events.push_back(Event::patch(60, 2, 7));
    t.events.push_back(Event::controller(100, 2, 1, 50));
    t.events.push_back(Event::controller(110, 2, 1, 70));
    t.events.push_back(Event::controller(112, 2, 10, 20)); // pan: not among the chased kinds
    t.events.push_back(Event::pitchWheel(115, 2, 1000));
    sortEvents(t.events);
    PlaybackOptions o;
    o.chase = true;
    // Start inside the second note: the first note is skipped, chased state precedes the notes.
    const std::vector<MidiMessage> out = render(s, o, 130);
    CHECK_EQ(hex(out[0]), "c2 28"); // stream parameters
    CHECK_EQ(hex(out[1]), "b2 07 64");
    CHECK_EQ(hex(out[2]), "b2 0a 40");
    // chased in the order patch, wheel, modulation, sustain, volume: c2 -> 7, e2 -> 1000, b2 01 -> 70
    CHECK_EQ(hex(out[3]), "c2 07");
    CHECK_EQ(hex(out[4]), "e2 68 47");
    CHECK_EQ(hex(out[5]), "b2 01 46");
    CHECK(near(out[5].time, 0.0));
    // The second note started before the position, so it is not played and gets no note-off;
    // the third note follows.
    CHECK_EQ(hex(out[6]), "92 40 50");
    CHECK(near(out[6].time, (240 - 130) / 240.0));
    CHECK_EQ(hex(out[7]), "92 40 00");
    o.chase = false;
    const std::vector<MidiMessage> plain = render(s, o, 130);
    CHECK_EQ(plain.size(), 5U + 16U); // 3 parameters, the third note's on and off, the stop
    // A stream with nothing left at the start position is not chased.
    Stream& early = s.stream(1);
    early.channel = 4;
    early.events = {Event::patch(0, 4, 30), Event::controller(10, 4, 7, 60)};
    o.chase = true;
    o.sendStreamParameters = false;
    const std::vector<MidiMessage> chased = render(s, o, 130);
    CHECK(std::ranges::none_of(chased, [](const MidiMessage& m) { return m.channel() == 4 && m.status() != 0xb4; }));
    CHECK(std::ranges::none_of(chased, [](const MidiMessage& m) { return hex(m) == "c4 1e"; }));
}

void testTempoChangeAndRatio() {
    Sequence s = simpleSequence();
    s.tempoMap.set(120, 6000); // from the second note on: 60 bpm = 120 ticks per second
    PlaybackOptions o;
    o.sendStreamParameters = false;
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
    const Sequence s = simpleSequence();
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendStreamParameters = false;
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
    Sequence s = simpleSequence();
    SysxBank bank;
    bank.name = "reset";
    bank.data = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    s.sysxBanks.push_back(bank);
    Stream& t = s.stream(0);
    t.events.push_back(Event::sysex(120, 0));
    t.events.push_back(Event::textEvent(240, "verse"));
    t.events.push_back(Event::mciCommand(240, "play x"));
    sortEvents(t.events);
    FakeClock clock;
    RecordingSink sink;
    Transport tr(s, clock, sink);
    std::vector<std::pair<std::string, double>> strings;
    tr.setStringEventHandler([&](const int stream, const Event& e, const double time) {
        CHECK_EQ(stream, 0);
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

void testStreamLoopAndCorpus() {
    // A looped stream repeats with a whole-measure period.
    Sequence s;
    Stream& t = s.stream(0);
    t.channel = 0;
    t.reps = 3;
    t.events.push_back(Event::note(0, 0, 60, 100, 300)); // ends at 300, inside measure 1 -> period 480
    PlaybackOptions o;
    o.sendStreamParameters = false;
    const std::vector<MidiMessage> out = render(s, o);
    CHECK_EQ(out.size(), 6U + 16U);
    CHECK(near(out[2].time, 2.0)); // second repetition at tick 480
    CHECK(near(out[4].time, 4.0));

    // Loop count 9999: repeat while the non-looping streams play, cut at their end.
    t.reps = 9999;
    Stream& bass = s.stream(1);
    bass.channel = 1;
    bass.events.push_back(Event::note(0, 1, 36, 100, 1100)); // ends at 1100: inside the third repetition
    const std::vector<MidiMessage> forever = render(s, o);
    // bass on/off, three repetitions of the looped note (the third cut short at 1100), the stop
    CHECK_EQ(forever.size(), 8U + 16U);
    CHECK(near(forever.back().time, 1100 / 240.0));
    std::size_t cut = 0;
    for (const MidiMessage& m : forever) {
        if (m.isNoteOff() && m.channel() == 0 && near(m.time, 1100 / 240.0)) {
            ++cut;
        }
    }
    CHECK_EQ(cut, 1U);
    s.streams.pop_back();
    t.reps = 9999;
    CHECK_EQ(render(s, o).size(), 2U + 16U); // no non-looping stream: played once
    t.reps = 3;

    // The corpus sequences render: one note-on and one note-off per note, times monotonic.
    for (const corpus::CorpusSequence& c : corpus::allSequences()) {
        const std::vector<MidiMessage> r = render(c.sequence, o);
        std::size_t notes = 0;
        for (const Stream& tr : c.sequence.streams) {
            const int reps = std::max(1, tr.reps.value_or(1));
            if (tr.muted) {
                continue;
            }
            const Tick shift = tr.timePlus.value_or(0);
            (void)shift; // Time- that pushes a note before the start plays it at the start
            notes += static_cast<std::size_t>(reps) *
                     static_cast<std::size_t>(
                         std::ranges::count_if(tr.events, [](const Event& e) { return e.kind == EventKind::Note; }));
        }
        const auto ons = static_cast<std::size_t>(std::ranges::count_if(r, &MidiMessage::isNoteOn));
        const auto offs = static_cast<std::size_t>(std::ranges::count_if(r, &MidiMessage::isNoteOff));
        CHECK_EQ(ons, notes);
        CHECK_EQ(offs, notes);
        CHECK(std::ranges::is_sorted(r, {}, &MidiMessage::time));
    }
}

void testMetronome() {
    // 3/4 from measure 1, 120 ppq: beats at 0, 120, 240, 360, ...
    Sequence s = simpleSequence();
    s.meterMap.entries.front().numerator = 3;
    s.metronome.port = 1;
    s.metronome.channel = 9;
    s.metronome.key = 42;
    s.metronome.velocity = 100;
    s.metronome.duration = 10;
    PlaybackOptions o;
    o.sendStreamParameters = false;
    o.metronome = true;
    o.metronomeAccent = true;                          // does not change the MIDI click
    const std::vector<MidiMessage> out = render(s, o); // notes end at tick 360
    std::vector<MidiMessage> clicks;
    std::ranges::copy_if(out, std::back_inserter(clicks),
                         [](const MidiMessage& m) { return m.port == 1 && (m.status() & 0xf0) == 0x90; });
    // beats 0, 120, 240 and the end beat 360, whose note-off comes with the stop
    CHECK_EQ(clicks.size(), 8U);
    CHECK_EQ(hex(clicks[0]), "99 2a 64");
    CHECK(near(clicks[0].time, 0.0));
    CHECK_EQ(hex(clicks[1]), "99 2a 00");
    CHECK(near(clicks[1].time, 40 / 240.0)); // duration 10 MIDI clocks: off after 8 clocks = 40 ticks
    CHECK_EQ(hex(clicks[2]), "99 2a 64");    // same velocity on every beat
    CHECK(near(clicks[2].time, 0.5));
    CHECK(near(clicks[6].time, 1.5));
    CHECK(near(clicks[7].time, 1.5));
    CHECK(std::ranges::is_sorted(out, {}, &MidiMessage::time));
    // Velocity 0 disables the metronome note.
    s.metronome.velocity = 0;
    CHECK_EQ(render(s, o).size(), 6U + 16U);
    s.metronome.velocity = 100;

    // Count-in: two measures of 3/4 at 120 bpm = 3 s before the sequence starts.
    o.countInMeasures = 2;
    o.metronomeAccent = false;
    FakeClock clock;
    RecordingSink sink;
    Transport tr(s, clock, sink, o);
    clock.time = 10.0;
    tr.play();
    CHECK(tr.countingIn());
    CHECK_EQ(tr.now(), 0);
    clock.time = 12.95;
    tr.pump();
    CHECK(tr.countingIn());
    CHECK_EQ(tr.now(), 0);
    std::size_t countIn = 0;
    for (const MidiMessage& m : sink.messages) {
        if (m.port == 1 && m.isNoteOn() && m.time < 13.0) {
            ++countIn;
        }
    }
    CHECK_EQ(countIn, 6U);
    CHECK(near(sink.messages.front().time, 10.0));
    clock.time = 13.0;
    tr.pump();
    CHECK(!tr.countingIn());
    CHECK_EQ(tr.now(), 0);
    CHECK_EQ(tr.tickAt(13.5).value_or(-1), 120);
    CHECK(!tr.tickAt(11.0)); // inside the count-in
    // the first note and the first in-sequence click are at 13.0
    bool noteAt13 = false;
    for (const MidiMessage& m : sink.messages) {
        if (m.port == 0 && m.isNoteOn() && near(m.time, 13.0)) {
            noteAt13 = true;
        }
    }
    CHECK(noteAt13);
    tr.stop();
    // fromSequence picks the flags up from VARS.
    s.vars.setMetronomePlayback(false);
    s.vars.setMetronomeRecording(true);
    s.vars.setCountIn(1);
    s.vars.setMetronomeAccent(true);
    const PlaybackOptions play = PlaybackOptions::fromSequence(s);
    CHECK(!play.metronome);
    CHECK_EQ(play.countInMeasures, 0);
    const PlaybackOptions rec = PlaybackOptions::fromSequence(s, true);
    CHECK(rec.metronome);
    CHECK(rec.metronomeAccent);
    CHECK_EQ(rec.countInMeasures, 1);
}

// ---------------------------------------------------------------- oracles

/// One line of a capture: milliseconds and the message bytes in hex ("90 3c 64").
struct Captured {
    double ms = 0;
    std::string bytes;
};

std::vector<Captured> readCapture(const std::string& path) {
    std::vector<Captured> out;
    std::ifstream in(path);
    CHECK(in.good());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        Captured c;
        fields >> c.ms;
        std::string byte;
        while (fields >> byte) {
            c.bytes += (c.bytes.empty() ? "" : " ") + byte;
        }
        out.push_back(c);
    }
    return out;
}

/// One playback of a capture: from the first message that is not a MIDI clock to the end of the
/// first All Notes Off burst and the controller reset that may follow it; times from its start.
std::vector<Captured> onePlayback(std::vector<Captured> c) {
    const auto first = std::ranges::find_if(c, [](const Captured& m) { return m.bytes != "f8"; });
    c.erase(c.begin(), first);
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (c[i].bytes == "bf 7b 00") {
            std::size_t end = i + 1;
            while (end < c.size() && c[end].ms - c[i].ms < 100 && c[end].bytes != "f8" &&
                   (c[end].bytes[0] == 'b' || c[end].bytes[0] == 'e')) {
                ++end;
            }
            c.resize(end);
            break;
        }
    }
    const double t0 = c.empty() ? 0 : c.front().ms;
    for (Captured& m : c) {
        m.ms -= t0;
    }
    return c;
}

/// Every message the transport sends (short and sysex), as playing from `from` to `thru` (the end
/// when absent) with the sequence's own settings.
std::vector<Captured> playback(const Sequence& s, const Tick from, const std::optional<Tick> thru) {
    class AllSink final : public MidiSink {
    public:
        std::vector<Captured> out;
        void send(const MidiMessage& m) override { out.push_back({m.time * 1000, hex(m)}); }
        void sendSysex(const double time, int /*port*/, const std::vector<std::uint8_t>& bytes) override {
            std::string h;
            for (const std::uint8_t b : bytes) {
                std::array<char, 4> buf{};
                std::snprintf(buf.data(), buf.size(), "%02x", b);
                h += (h.empty() ? "" : " ") + std::string(buf.data());
            }
            out.push_back({time * 1000, h});
        }
    };
    FakeClock clock;
    AllSink sink;
    PlaybackOptions o = PlaybackOptions::fromSequence(s);
    o.lookAhead = 0;
    Transport tr(s, clock, sink, o);
    tr.seek(from);
    tr.play();
    const Tick end = thru.value_or(tr.endTick());
    clock.time = s.tempoMap.tickToSeconds(end) - s.tempoMap.tickToSeconds(from);
    tr.pump();
    if (tr.playing()) {
        tr.stop();
    }
    return sink.out;
}

/// The original engine's output for the engine test sequences (tests/data/engine/README.md): the
/// same messages in the same order, and (where the capture's timing is meaningful) within 25 ms.
void testOracles(const std::string& data) {
    struct Take {
        const char* capture;
        const char* sequence; // in engine/src, or src/ for the corpus songs
        Tick from;
        Tick thru; // -1: to the end
        bool times;
    };
    const std::array<Take, 27> takes = {{
        {"ENG_ORDR-play", "engine/src/ENG_ORDR", 0, -1, true},
        {"ENG_ORDR-stop", "engine/src/ENG_ORDR", 0, 585, true},
        {"ENG_ORDR-resume", "engine/src/ENG_ORDR", 585, 1176, false}, // restarted 17 ticks early
        {"ENG_MULT-play", "engine/src/ENG_MULT", 0, -1, true},
        {"ENG_PARM-play", "engine/src/ENG_PARM", 0, -1, true},
        {"ENG_TMPO-play", "engine/src/ENG_TMPO", 0, -1, true},
        {"ENG_CHAS-play", "engine/src/ENG_CHAS", 960, -1, true},
        {"ENG_LOOP-play", "engine/src/ENG_LOOP", 0, -1, true},
        {"ENG_SYSX-play", "engine/src/ENG_SYSX", 0, -1, true},
        {"ENG_SYNC-play", "engine/src/ENG_SYNC", 0, 609, true},
        {"ENG_METR-play", "engine/src/ENG_METR", 0, -1, true},
        {"E2_KORD-play", "engine/src/E2_KORD", 0, -1, true},
        {"E2_OVLP-play", "engine/src/E2_OVLP", 0, -1, true},
        {"E2_STOP-play", "engine/src/E2_STOP", 0, 390, true},
        {"E2_CHAS-play", "engine/src/E2_CHAS", 960, -1, true},
        {"E2_METR-play", "engine/src/E2_METR", 0, -1, true},
        {"E2_SPP-play", "engine/src/E2_SPP", 480, -1, true},
        {"FURELISE-play", "src/FURELISE", 0, -1, true},
        {"E3_MA-play", "engine/src/E3_MA", 0, -1, true}, // click durations 1, 20, 10, 40, 2, 24
        {"E3_MB-play", "engine/src/E3_MB", 0, -1, true},
        {"E3_MC-play", "engine/src/E3_MC", 0, -1, true},
        {"E3_MD-play", "engine/src/E3_MD", 0, -1, true},
        {"E3_ME-play", "engine/src/E3_ME", 0, -1, true},
        {"E3_MF-play", "engine/src/E3_MF", 0, -1, true},
        {"E3_DENS-stop", "engine/src/E3_DENS", 0, 561, true},
        {"E3_SPPD-play", "engine/src/E3_SPPD", 480, -1, true},
        {"E3_SHTL-play", "engine/src/E3_SHTL", 0, -1, true}, // the auto-shuttle bytes are ignored
    }};
    for (const Take& take : takes) {
        const Sequence s = wrk::readFile(data + "/" + take.sequence + ".WRK");
        const std::vector<Captured> want = onePlayback(readCapture(data + "/engine/" + take.capture + ".txt"));
        const std::vector<Captured> got =
            playback(s, take.from, take.thru < 0 ? std::nullopt : std::optional<Tick>(take.thru));
        CHECK_EQ(got.size(), want.size());
        double worst = 0;
        for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
            if (got[i].bytes != want[i].bytes) {
                std::fprintf(stderr, "%s: message %zu is %s, the capture has %s\n", take.capture, i,
                             got[i].bytes.c_str(), want[i].bytes.c_str());
                CHECK(false);
                break;
            }
            worst = std::max(worst, std::fabs(got[i].ms - want[i].ms));
        }
        if (take.times && worst >= 25) {
            std::fprintf(stderr, "%s: times differ by up to %.1f ms\n", take.capture, worst);
            CHECK(worst < 25);
        }
    }
    // Loading a file: the streams' patch, volume and pan (clocks of a previous file's sync left out).
    const std::array<std::pair<const char*, const char*>, 5> opens = {{
        {"ENG_ORDR-open", "engine/src/ENG_ORDR"},
        {"ENG_MULT-open", "engine/src/ENG_MULT"},
        {"ENG_SYSX-open", "engine/src/ENG_SYSX"}, // the auto-send bank is not sent
        {"E2_CHAS-open", "engine/src/E2_CHAS"},
        {"FURELISE-open", "src/FURELISE"},
    }};
    for (const auto& [capture, sequence] : opens) {
        std::vector<std::string> want;
        for (const Captured& c : readCapture(data + "/engine/" + capture + ".txt")) {
            if (c.bytes != "f8") {
                want.push_back(c.bytes);
            }
        }
        std::vector<std::string> got;
        for (const MidiMessage& m : openMessages(wrk::readFile(data + "/" + sequence + ".WRK"))) {
            got.push_back(hex(m));
        }
        CHECK(got == want);
    }
    // The original engine sends no Song Position Pointer, also with "Send MIDI SPP" on and after a
    // locate, and keeps sending Clock while stopped.
    for (const char* capture : {"E2_SPP-locate", "E2_SPP-play", "E3_SPPD-play"}) {
        const std::vector<Captured> sync = readCapture(data + "/engine/" + std::string(capture) + ".txt");
        CHECK(std::ranges::none_of(sync, [](const Captured& c) { return c.bytes.starts_with("f2"); }));
    }
    const std::vector<Captured> idle = readCapture(data + "/engine/ENG_SYNC-open.txt");
    CHECK(idle.size() > 100);
    CHECK(std::ranges::all_of(idle, [](const Captured& c) { return c.bytes == "f8"; }));
}

void testMidiClock() {
    Sequence s = simpleSequence(); // 120 ppq: a clock every 5 ticks = 1/48 s at 120 bpm
    PlaybackOptions o;
    o.sendStreamParameters = false;
    o.midiClock = true;
    std::vector<MidiMessage> out = render(s, o);
    CHECK_EQ(hex(out[0]), "fa");
    CHECK_EQ(hex(out[1]), "f8");
    CHECK_EQ(hex(out[2]), "92 3c 64");
    CHECK(near(out[3].time, 1 / 48.0));
    const auto clocks = std::ranges::count_if(out, [](const MidiMessage& m) { return hex(m) == "f8"; });
    CHECK_EQ(clocks, 73);                      // ticks 0, 5, ... 360
    CHECK_EQ(hex(out[out.size() - 17]), "fc"); // Stop before the All Notes Off burst
    out = render(s, o, 120);
    CHECK_EQ(hex(out[0]), "fb"); // away from the start: Continue
    o.startNeverContinue = true;
    out = render(s, o, 120);
    CHECK_EQ(hex(out[0]), "fa");
    // While stopped, pump() keeps the clock running at the tempo.
    FakeClock clock;
    RecordingSink sink;
    o.lookAhead = 0;
    Transport tr(s, clock, sink, o);
    CHECK(!tr.pump());
    clock.time = 0.5;
    CHECK(!tr.pump());
    CHECK_EQ(sink.messages.size(), 25U); // 0 .. 0.5 s at 1/48 s
    CHECK(std::ranges::all_of(sink.messages, [](const MidiMessage& m) { return hex(m) == "f8"; }));
}

MidiMessage input(const double time, std::initializer_list<std::uint8_t> bytes) {
    MidiMessage m;
    m.time = time;
    m.size = static_cast<std::uint8_t>(bytes.size());
    std::ranges::copy(bytes, m.data.begin());
    return m;
}

bool sameEvent(const Event& a, const Event& b) {
    return std::tie(a.time, a.kind, a.channel, a.data1, a.data2, a.duration) ==
           std::tie(b.time, b.kind, b.channel, b.data1, b.data2, b.duration);
}

void testRecorder() {
    Sequence s = simpleSequence(); // 240 ticks per second
    FakeClock clock;
    RecordingSink sink;
    PlaybackOptions o;
    o.sendStreamParameters = false;
    o.stopAtEnd = false; // recording continues past the end of the existing material
    Transport tr(s, clock, sink, o);
    Recorder rec(tr);
    // Nothing is recorded while stopped.
    rec.receive(input(0.0, {0x90, 60, 100}));
    CHECK(rec.empty());
    clock.time = 5.0;
    tr.play();                                 // tick 0 at 5.0 s
    rec.receive(input(5.0, {0x91, 60, 100}));  // note at tick 0
    rec.receive(input(5.25, {0x91, 64, 90}));  // tick 60, overlapping
    rec.receive(input(5.5, {0x91, 60, 0}));    // vel-0 note-off: first note, duration 120
    rec.receive(input(5.5, {0xb1, 64, 127}));  // sustain at tick 120
    rec.receive(input(5.75, {0xe1, 0, 0x60})); // pitch wheel at tick 180: 0x3000 - 8192 = 4096
    rec.receive(input(5.75, {0xc1, 5}));       // patch
    rec.receive(input(5.8, {0xf8}));           // clock: ignored
    rec.receive(input(5.9, {0x81, 64, 0}));    // note-off at tick 216: second note, duration 156
    rec.receive(input(6.0, {0x91, 67, 80}));   // still held at the end
    CHECK_EQ(rec.count(), 6U);
    clock.time = 6.5;
    CHECK(tr.pump()); // past the last event, still running
    CHECK_EQ(tr.now(), 360);
    rec.finish(clock.time); // tick 360
    tr.stop();
    CHECK_EQ(tr.now(), 360);
    const std::vector<Event>& e = rec.events();
    CHECK_EQ(e.size(), 6U);
    CHECK(sameEvent(e[0], Event::note(0, 1, 60, 100, 120)));
    CHECK(sameEvent(e[1], Event::note(60, 1, 64, 90, 156)));
    CHECK(sameEvent(e[2], Event::controller(120, 1, 64, 127)));
    CHECK(sameEvent(e[3], Event::pitchWheel(180, 1, 4096)));
    CHECK(sameEvent(e[4], Event::patch(180, 1, 5)));
    CHECK(sameEvent(e[5], Event::note(240, 1, 67, 80, 120))); // closed by finish()
    CHECK(std::ranges::is_sorted(e, {}, &Event::time));

    // Keep take: merged into the stream in time order, the take is cleared.
    Stream& t = s.stream(0);
    const std::size_t before = t.events.size();
    rec.commit(t);
    CHECK_EQ(t.events.size(), before + 6);
    CHECK(std::ranges::is_sorted(t.events, {}, &Event::time));
    CHECK(rec.empty());

    // Same key played twice before either is released: LIFO pairing, as in SMF import.
    tr.rewind();
    clock.time = 10.0;
    tr.play();
    rec.receive(input(10.0, {0x90, 60, 100}));
    rec.receive(input(10.5, {0x90, 60, 100}));
    rec.receive(input(10.75, {0x80, 60, 0})); // closes the second (tick 120, duration 60)
    rec.receive(input(11.0, {0x80, 60, 0}));  // closes the first (duration 240)
    rec.finish(11.0);
    tr.stop();
    CHECK_EQ(rec.events().size(), 2U);
    CHECK_EQ(rec.events()[0].duration, 240);
    CHECK_EQ(rec.events()[1].duration, 60);
    rec.clear();

    // Punch range and event filter; overwrite commit replaces what was in the range.
    RecordOptions ro;
    ro.punchIn = 120;
    ro.punchOut = 240;
    ro.controllers = false;
    rec.setOptions(ro);
    tr.rewind();
    clock.time = 20.0;
    tr.play();
    rec.receive(input(20.0, {0x90, 60, 100})); // tick 0: before punch-in
    rec.receive(input(20.5, {0x90, 62, 100})); // tick 120: in
    rec.receive(input(20.6, {0xb0, 1, 10}));   // filtered
    rec.receive(input(20.9, {0x90, 64, 100})); // tick 216: in
    rec.receive(input(21.0, {0x90, 65, 100})); // tick 240: out
    rec.receive(input(21.5, {0x80, 62, 0}));
    rec.finish(21.5);
    tr.stop();
    CHECK_EQ(rec.events().size(), 2U);
    CHECK_EQ(rec.events()[0].data1, 62);
    CHECK_EQ(rec.events()[1].data1, 64);
    Stream fresh;
    fresh.events.push_back(Event::note(0, 0, 40, 100, 60));
    fresh.events.push_back(Event::note(130, 0, 41, 100, 60)); // in the range: replaced
    fresh.events.push_back(Event::note(300, 0, 42, 100, 60));
    rec.commitReplacing(fresh, 120, 240);
    CHECK_EQ(fresh.events.size(), 4U);
    CHECK_EQ(fresh.events[1].data1, 62);
    CHECK_EQ(fresh.events[3].data1, 42);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sseq_engine_tests <tests/data dir>\n");
        return 2;
    }
    testRenderSimple();
    testStreamParametersAndTransforms();
    testMuteAndSolo();
    testLookAheadAndPump();
    testStopCutsNotesAndZeroesControllers();
    testStopDiscardsScheduledNotes();
    testSeekAndChase();
    testTempoChangeAndRatio();
    testLoop();
    testSysexAndStringEvents();
    testStreamLoopAndCorpus();
    testMetronome();
    testRecorder();
    testMidiClock();
    testOracles(argv[1]);
    return test::report("engine tests");
}
