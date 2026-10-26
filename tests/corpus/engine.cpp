// Engine test sequences: played in Cakewalk Apprentice 1.04 while its MIDI output was captured
// (tests/data/engine). 120 ppq, 4/4 unless stated, so one measure is 480 ticks.
#include "sequences.hpp"

#include <string>
#include <vector>

namespace sseq::corpus {

namespace {

Stream& newStream(Sequence& s, int number, const std::string& name, int channel) {
    Stream& t = s.stream(number);
    t.name = name;
    t.channel = static_cast<std::uint8_t>(channel);
    return t;
}

Event note(Tick t, int ch, int key, int vel, Tick dur) {
    return Event::note(t, static_cast<std::uint8_t>(ch), key, vel, dur);
}

// Ordering inside one tick, note on/off pairs, every MIDI kind, the track's patch/volume/pan.
CorpusSequence order() {
    CorpusSequence it{"ENG_ORDR", "Event order within a tick, note-off vs note-on, all MIDI kinds", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Order", 0);
    t.patch = 5;
    t.volume = 100;
    t.pan = 40;
    auto& e = t.events;
    // m1: adjacent different keys, then the same key repeated back to back
    e.push_back(note(0, 0, 60, 101, 120));
    e.push_back(note(120, 0, 62, 102, 120));
    e.push_back(note(240, 0, 64, 103, 120));
    e.push_back(note(360, 0, 64, 104, 120));
    // m2: a chord stored in the order 67, 60, 64
    e.push_back(note(480, 0, 67, 105, 240));
    e.push_back(note(480, 0, 60, 106, 240));
    e.push_back(note(480, 0, 64, 107, 240));
    // m3: patch, controller and note at the same tick, stored in that order; then the other kinds
    e.push_back(Event::patch(960, 0, 20));
    e.push_back(Event::controller(960, 0, 7, 90));
    e.push_back(note(960, 0, 65, 108, 120));
    e.push_back(Event::keyAftertouch(1000, 0, 65, 50));
    e.push_back(Event::channelAftertouch(1040, 0, 60));
    e.push_back(Event::pitchWheel(1080, 0, 1000));
    e.push_back(Event::pitchWheel(1120, 0, -8192));
    e.push_back(Event::pitchWheel(1160, 0, 0));
    e.push_back(note(1200, 0, 72, 109, 1)); // one-tick note
    // m4: overlapping notes on the same key, and a long note ending with the sequence
    e.push_back(note(1440, 0, 69, 110, 240));
    e.push_back(note(1500, 0, 69, 111, 60));
    e.push_back(note(1680, 0, 48, 112, 240));
    sortEvents(e);
    return it;
}

// Several streams: same-tick order across streams, file order vs stream number, channel handling,
// a muted stream.
CorpusSequence multi() {
    CorpusSequence it{"ENG_MULT", "Same-tick order across streams, file order vs number, muted stream", {}};
    Sequence& s = it.sequence;
    struct P {
        int number;
        const char* name;
        int channel; // -1 = none
        int key;
        bool muted;
    };
    // Created in this order on purpose: file order differs from stream number.
    const std::vector<P> ps = {
        {2, "Stream 3 ch3", 2, 64, false}, {0, "Stream 1 ch1", 0, 60, false}, {5, "Stream 6 none", -1, 72, false},
        {1, "Stream 2 ch2", 1, 62, false}, {3, "Stream 4 ch1", 0, 65, false}, {4, "Stream 5 muted", 3, 67, true},
    };
    for (const P& p : ps) {
        Stream& t = s.stream(p.number);
        t.name = p.name;
        if (p.channel >= 0) {
            t.channel = static_cast<std::uint8_t>(p.channel);
        }
        t.muted = p.muted;
        t.patch = 10 + p.number;
        // events carry channel 7 + number; a forced channel replaces it
        const int ch = 7 + p.number;
        for (int i = 0; i < 4; ++i) {
            t.events.push_back(note(Tick{i} * 240, ch, p.key, 100 + p.number, 120));
        }
        t.events.push_back(Event::controller(960, static_cast<std::uint8_t>(ch), 10, 10 * p.number));
    }
    return it;
}

// Stream parameters applied at output: Key+, Vel+ with clipping, Time+, forced channel, reps.
CorpusSequence params() {
    CorpusSequence it{"ENG_PARM", "Key+, Vel+ (with clipping), Time+, forced channel, track reps", {}};
    Sequence& s = it.sequence;
    {
        Stream& t = newStream(s, 0, "Key+12 Vel+20", 0);
        t.keyPlus = 12;
        t.velPlus = 20;
        t.events = {note(0, 0, 60, 100, 120), note(240, 0, 120, 120, 120), note(480, 0, 10, 1, 120)};
    }
    {
        Stream& t = newStream(s, 1, "Key-12 Vel-30", 1);
        t.keyPlus = -12;
        t.velPlus = -30;
        t.events = {note(0, 1, 60, 100, 120), note(240, 1, 5, 20, 120), note(480, 1, 64, 31, 120)};
    }
    {
        Stream& t = newStream(s, 2, "Time+30", 2);
        t.timePlus = 30;
        t.events = {note(0, 2, 67, 90, 120), note(480, 2, 67, 91, 120)};
    }
    {
        Stream& t = newStream(s, 3, "Time-30", 3);
        t.timePlus = -30;
        t.events = {note(0, 3, 69, 92, 120), note(480, 3, 69, 93, 120)};
    }
    {
        Stream& t = newStream(s, 4, "Reps 3", 4);
        t.reps = 3;
        t.loop = true;
        t.events = {note(0, 4, 72, 94, 60), note(120, 4, 74, 95, 60), Event::controller(240, 4, 1, 40)};
    }
    {
        Stream& t = newStream(s, 5, "Forced ch6 from 9", 5);
        t.events = {note(0, 9, 76, 96, 120), Event::controller(120, 9, 11, 70), note(480, 9, 76, 97, 120)};
    }
    return it;
}

// Tempo map in real time: quarter notes across tempo changes.
CorpusSequence tempo() {
    CorpusSequence it{"ENG_TMPO", "Quarter notes across tempo changes (120, 60, 240, 90.5 mid-measure)", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Quarters", 0);
    for (int i = 0; i < 16; ++i) {
        t.events.push_back(note(Tick{i} * 120, 0, 60 + i, 100, 60));
    }
    s.tempoMap.entries = {TempoEntry{0, 12000}, TempoEntry{480, 6000}, TempoEntry{960, 24000}, TempoEntry{1500, 9050}};
    return it;
}

// Controller chase: state set before measure 3, Now stored at measure 3.
CorpusSequence chase() {
    CorpusSequence it{
        "ENG_CHAS", "Chase on start at measure 3 (Now = 3:1:0): patch, controllers, wheel, held note", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Chase", 0);
    t.patch = 3;
    t.volume = 110;
    t.pan = 20;
    auto& e = t.events;
    e.push_back(Event::patch(0, 0, 10));
    e.push_back(Event::controller(0, 0, 7, 50));
    e.push_back(note(0, 0, 48, 100, 1440)); // held across measure 3
    e.push_back(Event::controller(240, 0, 7, 90));
    e.push_back(Event::controller(480, 0, 10, 20));
    e.push_back(Event::controller(600, 0, 64, 127));
    e.push_back(Event::controller(620, 0, 1, 64));
    e.push_back(Event::controller(640, 0, 91, 33));
    e.push_back(Event::pitchWheel(700, 0, 1000));
    e.push_back(Event::channelAftertouch(720, 0, 70));
    e.push_back(Event::keyAftertouch(730, 0, 48, 40));
    e.push_back(Event::patch(900, 0, 20));
    for (int m = 0; m < 4; ++m) {
        e.push_back(note(Tick{m} * 480 + 240, 0, 60 + m, 100, 120));
    }
    e.push_back(Event::controller(1200, 0, 7, 70));
    sortEvents(e);
    // a second stream whose only events lie before the start point
    Stream& u = newStream(s, 1, "Early only", 1);
    u.events = {Event::patch(0, 1, 40), Event::controller(100, 1, 7, 60), note(120, 1, 72, 90, 120)};
    s.vars.setNow(960);
    return it;
}

// Loop region with a note held across the loop end.
CorpusSequence loop() {
    CorpusSequence it{"ENG_LOOP", "Loop from 2:1:0 thru 3:1:0 (From/Thru stored), note held across the loop end", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Loop", 0);
    t.events = {note(0, 0, 55, 100, 120),   note(480, 0, 60, 101, 120),       note(720, 0, 64, 102, 120),
                note(900, 0, 67, 103, 240), Event::controller(940, 0, 1, 99), note(960, 0, 72, 104, 120),
                note(1440, 0, 48, 105, 120)};
    s.vars.setFrom(480);
    s.vars.setThru(960);
    return it;
}

// Sysx banks: auto-send and a sysex event; string events that must send nothing.
CorpusSequence sysx() {
    CorpusSequence it{"ENG_SYSX", "Sysx bank auto-send, sysex events, text and MCI events", {}};
    Sequence& s = it.sequence;
    SysxBank gm;
    gm.bank = 0;
    gm.name = "GM on";
    gm.autoSend = true;
    gm.data = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    SysxBank vol;
    vol.bank = 1;
    vol.name = "Master volume";
    vol.autoSend = false;
    vol.data = {0xf0, 0x7f, 0x7f, 0x04, 0x01, 0x00, 0x50, 0xf7};
    s.sysxBanks = {gm, vol};
    Stream& t = newStream(s, 0, "Sysx", 0);
    t.events = {Event::textEvent(0, "hello"),   Event::sysex(0, 1),   note(0, 0, 60, 100, 120),
                Event::mciCommand(240, "beep"), Event::sysex(480, 0), note(480, 0, 62, 100, 120)};
    return it;
}

// MIDI sync out: clocks, start/stop/continue, song position.
CorpusSequence midiSync() {
    CorpusSequence it{"ENG_SYNC", "Transmit MIDI sync on, send song position on", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Sync", 0);
    t.events = {note(0, 0, 60, 100, 120), note(480, 0, 62, 100, 120), note(960, 0, 64, 100, 120)};
    s.vars.setTransmitMidiSync(true);
    s.vars.setSendSpp(true);
    return it;
}

// Metronome on playback, with count-in.
CorpusSequence metronome() {
    CorpusSequence it{"ENG_METR", "Metronome on playback, accent on, one measure count-in", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "One note", 0);
    t.events = {note(480, 0, 60, 100, 480)};
    s.vars.setMetronomePlayback(true);
    s.vars.setMetronomeAccent(true);
    s.vars.setCountIn(1);
    return it;
}

// ---- second disk

CorpusSequence kindOrder() {
    CorpusSequence it{"E2_KORD", "Stored order of mixed kinds at one tick; close ticks across streams", {}};
    Sequence& s = it.sequence;
    SysxBank b;
    b.bank = 0;
    b.name = "Reset";
    b.data = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    s.sysxBanks = {b};
    Stream& t = newStream(s, 0, "Mixed", 0);
    t.events = {note(0, 0, 60, 101, 120),
                Event::controller(0, 0, 7, 100),
                Event::patch(0, 0, 5),
                Event::pitchWheel(0, 0, 100),
                Event::sysex(0, 0),
                Event::channelAftertouch(0, 0, 50),
                Event::patch(240, 0, 6),
                note(240, 0, 62, 102, 120),
                Event::controller(240, 0, 10, 30)};
    Stream& u = newStream(s, 1, "Other", 1);
    u.events = {note(0, 1, 64, 103, 120), Event::controller(240, 1, 7, 90)};
    // close ticks across streams: stream 2 one tick after stream 3, stream 4 two ticks after
    Stream& a = newStream(s, 2, "Plus 1", 2);
    a.events = {note(481, 2, 67, 104, 120), note(961, 2, 67, 104, 120)};
    Stream& c = newStream(s, 3, "Plus 0", 3);
    c.events = {note(480, 3, 69, 105, 120), note(960, 3, 69, 105, 120)};
    Stream& d = newStream(s, 4, "Plus 2", 4);
    d.events = {note(482, 4, 71, 106, 120), note(963, 4, 71, 106, 120)};
    return it;
}

CorpusSequence overlaps() {
    CorpusSequence it{"E2_OVLP", "Same-key overlaps: longer second note, two streams, identical start", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Overlap", 0);
    t.events = {note(0, 0, 60, 100, 240), note(120, 0, 60, 101, 360),    // second ends after the first
                note(960, 0, 67, 102, 120), note(960, 0, 67, 103, 240)}; // identical start
    Stream& u = newStream(s, 1, "Same channel", 0);
    u.events = {note(480, 0, 64, 104, 480)};
    Stream& v = newStream(s, 2, "Same channel 2", 0);
    v.events = {note(600, 0, 64, 105, 120)};
    return it;
}

CorpusSequence stopMany() {
    CorpusSequence it{"E2_STOP", "Long notes on several channels for Stop (stop after about 1 s)", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Ch1", 0);
    t.events = {note(0, 0, 72, 100, 1920), note(0, 0, 60, 100, 1920)};
    Stream& u = newStream(s, 1, "Ch4", 3);
    u.events = {note(0, 3, 50, 100, 1920)};
    Stream& v = newStream(s, 2, "Ch2", 1);
    v.events = {note(0, 1, 65, 100, 1920), note(120, 1, 40, 100, 1800)};
    return it;
}

CorpusSequence chaseOn() {
    CorpusSequence it = chase();
    it.name = "E2_CHAS";
    it.description = "ENG_CHAS with patch/controller search-back and zero controllers on";
    it.sequence.vars.setPatchSearchBack(true);
    it.sequence.vars.setZeroControllers(true);
    return it;
}

CorpusSequence metronome2() {
    CorpusSequence it{"E2_METR", "Metronome: 60 bpm, 3/4, key 37, velocity 100, duration 10, accent off", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Note", 0);
    t.events = {note(360, 0, 60, 100, 120)};
    s.tempoMap.entries = {TempoEntry{0, 6000}};
    MeterEntry m;
    m.measure = 1;
    m.numerator = 3;
    m.denominatorLog2 = 2;
    s.meterMap.entries = {m};
    s.metronome.key = 37;
    s.metronome.velocity = 100;
    s.metronome.duration = 10;
    s.vars.setMetronomePlayback(true);
    s.vars.setMetronomeAccent(false);
    return it;
}

CorpusSequence sppStart() {
    CorpusSequence it = midiSync();
    it.name = "E2_SPP";
    it.description = "Sync on, Now stored at 2:1:0 (start away from zero)";
    it.sequence.vars.setNow(480);
    return it;
}

// ---- third disk: the remaining questions

/// Metronome only, at `bpm`, with a click `duration` (ticks) and two measures of one held note.
CorpusSequence metronomeLength(const char* name, const int bpm, const int duration) {
    CorpusSequence it{
        name, "Metronome click length: " + std::to_string(bpm) + " bpm, duration " + std::to_string(duration), {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Note", 0);
    t.events = {note(0, 0, 60, 100, 960)};
    s.tempoMap.entries = {TempoEntry{0, bpm * 100}};
    s.metronome.duration = duration;
    s.vars.setMetronomePlayback(true);
    return it;
}

/// Notes every 15 ticks on cycling keys, so the first note after a resume shows where playback
/// restarted.
CorpusSequence dense() {
    CorpusSequence it{"E3_DENS", "A note every 15 ticks for 8 measures (keys cycle 36..83) for stop and resume", {}};
    Stream& t = newStream(it.sequence, 0, "Dense", 0);
    for (int i = 0; i < 256; ++i) {
        t.events.push_back(note(Tick{i} * 15, 0, 36 + i % 48, 100, 10));
    }
    return it;
}

/// SPP with a locate delay.
CorpusSequence sppDelay() {
    CorpusSequence it = sppStart();
    it.name = "E3_SPPD";
    it.description = "Sync on, send SPP on, locate delay 5, Now stored at 2:1:0";
    it.sequence.vars.setSppLocateDelay(5);
    return it;
}

/// The auto-shuttle bytes of VARS, which Apprentice has no dialog for: stop at 3:1:000, rewind to
/// 2:1:000, restart.
CorpusSequence shuttle() {
    CorpusSequence it{"E3_SHTL", "VARS auto-stop at 960, auto-rewind to 480, auto-restart; a note per beat", {}};
    Sequence& s = it.sequence;
    Stream& t = newStream(s, 0, "Beats", 0);
    for (int i = 0; i < 16; ++i) {
        t.events.push_back(note(Tick{i} * 120, 0, 60 + i, 100, 60));
    }
    Vars& v = s.vars;
    v.setFlag(21, true); // auto-stop
    v.setU32(22, 960);
    v.setFlag(26, true); // auto-rewind
    v.setU32(27, 480);
    v.setFlag(57, true); // auto-restart
    return it;
}

} // namespace

std::vector<CorpusSequence> engineSequences() {
    return {order(),
            multi(),
            params(),
            tempo(),
            chase(),
            loop(),
            sysx(),
            midiSync(),
            metronome(),
            kindOrder(),
            overlaps(),
            stopMany(),
            chaseOn(),
            metronome2(),
            sppStart(),
            metronomeLength("E3_MA", 60, 1),
            metronomeLength("E3_MB", 60, 20),
            metronomeLength("E3_MC", 120, 10),
            metronomeLength("E3_MD", 120, 40),
            metronomeLength("E3_ME", 120, 2),
            metronomeLength("E3_MF", 120, 24),
            dense(),
            sppDelay(),
            shuttle()};
}

} // namespace sseq::corpus
