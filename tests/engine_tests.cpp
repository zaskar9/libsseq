// Engine tests: deterministic playback through a fake clock and a recording sink.
#include "check.hpp"
#include "corpus/sequences.hpp"
#include "sseq/sseq.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

void testStreamParametersAndTransforms() {
    Sequence s = simpleSequence();
    Stream& t = s.stream(0);
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
    tr.stop();
    CHECK(!tr.playing());
    CHECK_EQ(tr.now(), 48); // 0.2 s = 48 ticks
    // note-off at 0.2, then on all 16 channels of the one port in use: sustain up, pitch wheel
    // centred, modulation zero, reset all controllers (Cakewalk's "Zero continuous controllers")
    CHECK_EQ(sink.messages.size(), 2U + 16U * 4U);
    CHECK_EQ(hex(sink.messages[1]), "92 3c 00");
    CHECK(near(sink.messages[1].time, 0.2));
    CHECK_EQ(hex(sink.messages[2]), "b0 40 00");
    CHECK_EQ(hex(sink.messages[3]), "e0 00 40");
    CHECK_EQ(hex(sink.messages[4]), "b0 01 00");
    CHECK_EQ(hex(sink.messages[5]), "b0 79 00");
    CHECK_EQ(hex(sink.messages[2 + 4 * 15]), "bf 40 00");
    CHECK(near(sink.messages[65].time, 0.2));
    // Resuming continues from the stop position (tick 48, inside the first note): the first note
    // is not repeated, and the second note-on at tick 120 comes 72 ticks = 0.3 s after the restart.
    clock.time = 1.0;
    tr.play();
    CHECK_EQ(sink.messages.size(), 66U); // nothing due within the look-ahead yet
    clock.time = 1.3;
    tr.pump();
    CHECK_EQ(sink.messages.size(), 67U);
    CHECK_EQ(hex(sink.messages[66]), "92 3e 5a");
    CHECK(near(sink.messages[66].time, 1.3));
    tr.stop();
    // Panic: stop, then all-notes-off and the controller reset on every channel.
    sink.messages.clear();
    tr.panic();
    CHECK_EQ(sink.messages.size(), 16U * 5U);
    CHECK_EQ(hex(sink.messages[0]), "b0 7b 00");
    CHECK_EQ(hex(sink.messages[1]), "b0 40 00");
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
    CHECK_EQ(out.size(), 6U);
    CHECK(near(out[2].time, 2.0)); // second repetition at tick 480
    CHECK(near(out[4].time, 4.0));

    // Loop count 9999: repeat while the non-looping streams play, cut at their end.
    t.reps = 9999;
    Stream& bass = s.stream(1);
    bass.channel = 1;
    bass.events.push_back(Event::note(0, 1, 36, 100, 1100)); // ends at 1100: inside the third repetition
    const std::vector<MidiMessage> forever = render(s, o);
    // bass on/off, three repetitions of the looped note (the third cut short at 1100)
    CHECK_EQ(forever.size(), 8U);
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
    CHECK_EQ(render(s, o).size(), 2U); // no non-looping stream: played once
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
    o.metronomeAccent = true;
    const std::vector<MidiMessage> out = render(s, o); // notes end at tick 360
    std::vector<MidiMessage> clicks;
    std::ranges::copy_if(out, std::back_inserter(clicks), [](const MidiMessage& m) { return m.port == 1; });
    // beats 0, 120, 240 (beat 360 is the end and not played), on and off each
    CHECK_EQ(clicks.size(), 6U);
    CHECK_EQ(hex(clicks[0]), "99 2a 64"); // downbeat at full velocity
    CHECK(near(clicks[0].time, 0.0));
    CHECK_EQ(hex(clicks[1]), "99 2a 00");
    CHECK(near(clicks[1].time, 10 / 240.0));
    CHECK_EQ(hex(clicks[2]), "99 2a 4b"); // other beats at 3/4
    CHECK(near(clicks[2].time, 0.5));
    CHECK(std::ranges::is_sorted(out, {}, &MidiMessage::time));
    // Velocity 0 disables the metronome note.
    s.metronome.velocity = 0;
    CHECK_EQ(render(s, o).size(), 6U);
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

int main() {
    testRenderSimple();
    testStreamParametersAndTransforms();
    testMuteAndSolo();
    testLookAheadAndPump();
    testStopCutsNotesAndZeroesControllers();
    testSeekAndChase();
    testTempoChangeAndRatio();
    testLoop();
    testSysexAndStringEvents();
    testStreamLoopAndCorpus();
    testMetronome();
    testRecorder();
    return test::report("engine tests");
}
