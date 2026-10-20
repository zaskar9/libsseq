// Test corpus: song definitions. See songs.hpp.
#include "songs.hpp"

#include <array>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace sseq::corpus {

// ---------------------------------------------------------------- notation helpers

int pitch(const std::string& name) {
    static constexpr std::array<int, 7> kSemitone = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    if (name.size() < 2) {
        throw std::invalid_argument("bad pitch " + name);
    }
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (letter < 'A' || letter > 'G') {
        throw std::invalid_argument("bad pitch " + name);
    }
    int semi = kSemitone[static_cast<std::size_t>(letter - 'A')];
    std::size_t i = 1;
    if (name[i] == '#') {
        ++semi;
        ++i;
    } else if (name[i] == 'b') {
        --semi;
        ++i;
    }
    const int octave = std::stoi(name.substr(i));
    return octave * 12 + semi;
}

namespace {

Tick duration(const std::string& d, const int ppq) {
    Tick t = 0;
    switch (d[0]) {
    case 'w':
        t = Tick{ppq} * 4;
        break;
    case 'h':
        t = Tick{ppq} * 2;
        break;
    case 'q':
        t = ppq;
        break;
    case 'e':
        t = ppq / 2;
        break;
    case 's':
        t = ppq / 4;
        break;
    default:
        throw std::invalid_argument("bad duration " + d);
    }
    for (std::size_t i = 1; i < d.size(); ++i) {
        if (d[i] == '.') {
            t = t * 3 / 2;
        } else if (d[i] == 't') {
            t = t * 2 / 3;
        } else {
            throw std::invalid_argument("bad duration " + d);
        }
    }
    return t;
}

} // namespace

Tick addMelody(Track& track, Tick start, const std::string& notation, const std::uint8_t channel, const int velocity,
               const int ppq, const double gate) {
    std::istringstream in(notation);
    std::string token;
    Tick t = start;
    while (in >> token) {
        if (token == "|") {
            continue;
        }
        if (token[0] == 'r') {
            t += duration(token.substr(1), ppq);
            continue;
        }
        // Split at the first duration letter (w h q e s) that follows the octave digit.
        std::size_t split = token.find_first_of("whqes", 1);
        if (split == std::string::npos) {
            throw std::invalid_argument("bad token " + token);
        }
        const int key = pitch(token.substr(0, split));
        const Tick len = duration(token.substr(split), ppq);
        const Tick played = std::max<Tick>(1, static_cast<Tick>(static_cast<double>(len) * gate));
        track.events.push_back(Event::note(t, channel, key, velocity, played));
        t += len;
    }
    sortEvents(track.events);
    return t;
}

// ---------------------------------------------------------------- building blocks

namespace {

Track& newTrack(Song& s, const int number, const std::string& name, const int channel, const int patch) {
    Track& t = s.track(number);
    t.name = name;
    t.channel = static_cast<std::uint8_t>(channel);
    t.patch = patch;
    return t;
}

void addChord(Track& t, const Tick time, std::initializer_list<int> keys, const int velocity, const Tick dur) {
    for (const int k : keys) {
        t.events.push_back(Event::note(time, t.channel.value_or(0), k, velocity, dur));
    }
}

void setMeter(Song& s, const int measure, const int num, const int denLog2) {
    MeterEntry m;
    m.measure = measure;
    m.numerator = num;
    m.denominatorLog2 = denLog2;
    if (measure == 1) {
        s.meterMap.entries = {m};
    } else {
        s.meterMap.entries.push_back(m);
    }
}

void setKey(Song& s, const int measure, const int alterations) {
    if (measure == 1) {
        s.keyMap = {KeyEntry{1, alterations}};
    } else {
        s.keyMap.push_back(KeyEntry{measure, alterations});
    }
}

void addMarker(Song& s, const Tick time, const std::string& name, const bool locked = false) {
    Marker m;
    m.time = time;
    m.name = name;
    m.smpteLocked = locked;
    s.markers.push_back(m);
}

std::string textOf(int n, const std::string& base) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        s += base;
    }
    return s;
}

// ---------------------------------------------------------------- synthetic songs

CorpusSong synMinimal() {
    CorpusSong c{"SYN_MIN", "Minimal song: one track, one note, defaults everywhere", {}};
    Track& t = c.song.track(0);
    t.name = "One note";
    t.events.push_back(Event::note(0, 0, 60, 100, 120));
    return c;
}

CorpusSong synKinds() {
    CorpusSong c{"SYN_KIND",
                 "Every event kind: note, key and channel aftertouch, controller, patch, wheel, sysex, MCI, text",
                 {}};
    Song& s = c.song;
    SysxBank bank;
    bank.bank = 0;
    bank.name = "GM reset";
    bank.autoSend = false;
    bank.data = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    s.sysxBanks.push_back(bank);
    Track& t = newTrack(s, 0, "All kinds", 2, 5);
    t.events.push_back(Event::textEvent(0, "text at start"));
    t.events.push_back(Event::mciCommand(0, "play c:\\sounds\\hello.wav"));
    t.events.push_back(Event::sysex(0, 0));
    t.events.push_back(Event::patch(0, 2, 20));
    t.events.push_back(Event::controller(0, 2, 7, 100));
    t.events.push_back(Event::controller(0, 2, 10, 64));
    t.events.push_back(Event::note(0, 2, 60, 100, 240));
    t.events.push_back(Event::keyAftertouch(60, 2, 60, 80));
    t.events.push_back(Event::channelAftertouch(120, 2, 90));
    t.events.push_back(Event::pitchWheel(180, 2, 0));
    t.events.push_back(Event::pitchWheel(200, 2, -8192));
    t.events.push_back(Event::pitchWheel(220, 2, 8191));
    t.events.push_back(Event::note(240, 2, 64, 90, 240));
    t.events.push_back(Event::controller(300, 2, 1, 127));
    t.events.push_back(Event::controller(360, 2, 64, 0));
    t.events.push_back(Event::textEvent(480, "text at the end"));
    sortEvents(t.events);
    // A second track with plain MIDI only, so the file has both stream chunk kinds.
    Track& u = newTrack(s, 1, "MIDI only", 3, 48);
    u.events.push_back(Event::note(0, 3, 48, 100, 480));
    u.events.push_back(Event::controller(240, 3, 11, 90));
    return c;
}

CorpusSong synParams() {
    CorpusSong c{"SYN_PARM",
                 "Track parameters: sparse numbers, channel none, Key+/Vel+/Time+, loop, mute, patch, volume, pan",
                 {}};
    Song& s = c.song;
    struct P {
        int number;
        const char* name;
        int channel; // -1 = none
        int keyPlus, velPlus;
        int port;
        bool muted, selected;
        int reps;               // 0 = none
        int timePlus;           // 0 = none
        int patch, volume, pan; // -1 = none
    };
    const std::array<P, 8> params = {{
        {0, "Plain", 0, 0, 0, 0, false, false, 0, 0, -1, -1, -1},
        {1, "Forced channel 16", 15, 0, 0, 0, false, false, 0, 0, 127, 127, 127},
        {3, "Key+12 Vel+20", -1, 12, 20, 0, false, true, 0, 0, 0, 0, 0},
        {7, "Key-12 Vel-20 muted", 4, -12, -20, 1, true, false, 0, 0, 64, 100, 64},
        {15, "Loop 4 Time+240", 5, 0, 0, 1, false, false, 4, 240, 33, 90, 32},
        {31, "Time-120 volume only", 6, 0, 0, 0, false, false, 0, -120, -1, 80, -1},
        {32, "x", 7, 0, 0, 0, false, false, 0, 0, -1, -1, 96}, // one-character name: Apprentice rejects empty names
        {33, "A track name of exactly thirty-two", 8, 0, 0, 0, false, false, 2, 0, 1, 1, 1},
    }};
    for (const P& p : params) {
        Track& t = s.track(p.number);
        t.name = p.name;
        if (p.channel >= 0) {
            t.channel = static_cast<std::uint8_t>(p.channel);
        }
        t.keyPlus = static_cast<std::int8_t>(p.keyPlus);
        t.velPlus = static_cast<std::int8_t>(p.velPlus);
        t.port = static_cast<std::uint8_t>(p.port);
        t.muted = p.muted;
        t.selected = p.selected;
        if (p.reps > 0) {
            t.reps = p.reps;
            t.loop = p.reps > 1;
        }
        if (p.timePlus != 0) {
            t.timePlus = p.timePlus;
        }
        if (p.patch >= 0) {
            t.patch = p.patch;
        }
        if (p.volume >= 0) {
            t.volume = p.volume;
        }
        if (p.pan >= 0) {
            t.pan = p.pan;
        }
        const std::uint8_t ch = t.channel.value_or(static_cast<std::uint8_t>(p.number % 16));
        for (int i = 0; i < 4; ++i) {
            t.events.push_back(Event::note(Tick{i} * 120, ch, 60 + i * 2, 64 + i * 10, 100));
        }
    }
    return c;
}

CorpusSong synMaps() {
    CorpusSong c{
        "SYN_MAPS",
        "Tempo, meter and key changes, markers, SMPTE format and offset, metronome, thru, VARS flags, comments",
        {}};
    Song& s = c.song;
    s.tempoMap.entries.clear();
    s.tempoMap.set(0, 12000);
    s.tempoMap.set(480, 6000);
    s.tempoMap.set(960, 24000);
    s.tempoMap.set(1440, 9950);
    s.tempoMap.set(1441, 10050);
    setMeter(s, 1, 4, 2);
    setMeter(s, 2, 3, 2);
    setMeter(s, 3, 6, 3);
    setMeter(s, 4, 2, 1);
    setMeter(s, 5, 5, 2);
    setMeter(s, 7, 4, 2);
    setKey(s, 1, 0);
    setKey(s, 2, 2);
    setKey(s, 4, -3);
    setKey(s, 7, 7);
    addMarker(s, 0, "Start");
    addMarker(s, 480, "Second measure");
    addMarker(s, 960, "Locked here", true);
    addMarker(s, 2000, "A marker name that is rather long for a marker");
    s.timecode.rate = FrameRate::Fps25;
    s.timecode.offsetFrames = 25 * 3600 + 25 * 60 * 2 + 25 * 3; // 01:02:03:00
    s.metronome.port = 1;
    s.metronome.channel = 9;
    s.metronome.key = 37;
    s.metronome.velocity = 127;
    s.metronome.duration = 12;
    s.thru.mode = 1;
    s.thru.port = 0; // Apprentice resets the thru port to one that exists on the machine
    s.thru.channel = 5;
    s.thru.keyPlus = -7;
    s.thru.velPlus = 15;
    s.vars.setNow(600);
    s.vars.setFrom(480);
    s.vars.setThru(1440);
    s.vars.setZeroControllers(true);
    s.vars.setSendSpp(false);
    s.vars.setUseContinue(false);
    s.vars.setPatchSearchBack(true);
    s.vars.setMetronomePlayback(true);
    s.vars.setMetronomeRecording(false);
    s.vars.setMetronomeAccent(true);
    s.vars.setCountIn(2);
    s.vars.setSppLocateDelay(9);
    s.vars.setTransmitMidiSync(true);
    s.setComments("Line one of the comments.\nLine two, with a tab\there.\n\nLast line after a blank one.");
    Track& t = newTrack(s, 0, "Pulse", 0, 115);
    for (int i = 0; i < 30; ++i) {
        t.events.push_back(Event::note(Tick{i} * 120, 0, 72, 100, 60));
    }
    return c;
}

CorpusSong synOverlap() {
    CorpusSong c{
        "SYN_OVLP", "Overlapping same-key notes, simultaneous note-offs, 1-tick durations, chords, large times", {}};
    Song& s = c.song;
    Track& t = newTrack(s, 0, "Overlaps", 0, 0);
    // Same key overlapping: later note ends first, then both orders of simultaneous offs.
    t.events.push_back(Event::note(0, 0, 60, 100, 200));
    t.events.push_back(Event::note(100, 0, 60, 90, 50));
    t.events.push_back(Event::note(400, 0, 62, 100, 100)); // same end as the next, different starts
    t.events.push_back(Event::note(450, 0, 64, 100, 50));
    t.events.push_back(Event::note(600, 0, 67, 100, 100)); // same start and same end
    t.events.push_back(Event::note(600, 0, 65, 100, 100));
    t.events.push_back(Event::note(600, 0, 69, 100, 100));
    t.events.push_back(Event::note(800, 0, 71, 100, 1)); // shortest possible
    t.events.push_back(Event::note(801, 0, 71, 100, 1));
    t.events.push_back(Event::note(900, 0, 48, 1, 120));      // lowest velocity
    t.events.push_back(Event::note(900, 0, 127, 127, 120));   // highest key and velocity
    t.events.push_back(Event::note(900, 0, 0, 64, 120));      // lowest key
    t.events.push_back(Event::note(1200, 0, 60, 100, 65535)); // longest duration the file can store
    Track& u = newTrack(s, 1, "Far away", 1, 0);
    u.events.push_back(Event::note(1000000, 1, 60, 100, 120)); // time needs 3 bytes
    u.events.push_back(Event::note(1000120, 1, 62, 100, 120));
    return c;
}

CorpusSong synDense() {
    CorpusSong c{
        "SYN_DENS", "Dense song: 8 tracks with 1500 notes each and one track with 9000 notes (stream over 64 KB)", {}};
    Song& s = c.song;
    for (int tr = 0; tr < 8; ++tr) {
        Track& t = newTrack(s, tr, "Dense " + std::to_string(tr + 1), tr, tr * 8);
        for (int i = 0; i < 1500; ++i) {
            t.events.push_back(Event::note(Tick{i} * 30, static_cast<std::uint8_t>(tr), 36 + (i * 7 + tr) % 60,
                                           40 + (i * 13) % 80, 25));
        }
    }
    Track& big = newTrack(s, 8, "Nine thousand", 8, 0);
    for (int i = 0; i < 9000; ++i) {
        big.events.push_back(Event::note(Tick{i} * 15, 8, 48 + (i * 5) % 36, 100, 10));
    }
    return c;
}

CorpusSong synStrings() {
    CorpusSong c{"SYN_STRS",
                 "String events: lyric-style text on every beat, a 127-byte text (Apprentice's maximum), MCI commands",
                 {}};
    Song& s = c.song;
    Track& t = newTrack(s, 0, "Lyrics", 0, 52);
    // Distinct words: Apprentice keeps string events in a case-insensitive atom table, so texts that
    // differ only in case collapse into one spelling, and an empty text is dropped.
    const std::array<std::string, 10> syllables = {"Row,",  "row2,", "row3", "your", "boat",
                                                   "gent-", "ly",    "down", "the",  "stream"};
    for (int i = 0; i < 10; ++i) {
        t.events.push_back(Event::note(Tick{i} * 120, 0, 60 + i % 5, 100, 100));
        t.events.push_back(Event::textEvent(Tick{i} * 120, syllables[static_cast<std::size_t>(i)]));
    }
    t.events.push_back(Event::textEvent(1200, textOf(127, "x"))); // 127 bytes: the longest text Apprentice loads
    t.events.push_back(Event::mciCommand(0, "open c:\\sounds\\intro.wav alias intro"));
    t.events.push_back(Event::mciCommand(60, "play intro"));
    t.events.push_back(Event::mciCommand(1200, "close intro"));
    sortEvents(t.events);
    return c;
}

CorpusSong synWave() {
    CorpusSong c{"SYN_WAVE", "A Wave event with a small payload (format unverified; kept separate on purpose)", {}};
    Song& s = c.song;
    Track& t = newTrack(s, 0, "Wave", 0, 0);
    t.events.push_back(Event::note(0, 0, 60, 100, 120));
    Event w;
    w.time = 120;
    w.kind = EventKind::Wave;
    // 44-byte PCM WAV header for 8-bit mono 8 kHz plus 16 samples of silence.
    const std::array<std::uint8_t, 44> header = {
        'R', 'I', 'F',  'F',  52, 0, 0,    0,    'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16,  0,   0,  0, 1, 0,
        1,   0,   0x40, 0x1f, 0,  0, 0x40, 0x1f, 0,   0,   1,   0,   8,   0,   'd', 'a', 't', 'a', 16, 0, 0, 0};
    w.payload.assign(header.begin(), header.end());
    w.payload.insert(w.payload.end(), 16, 0x80);
    t.events.push_back(w);
    return c;
}

// ---------------------------------------------------------------- realistic songs (public domain)

CorpusSong rowBoat() {
    CorpusSong c{"ROWBOAT", "Row, Row, Row Your Boat (traditional, 19th c.) as a four-part round in 6/8", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 10800);
    setMeter(s, 1, 6, 3);
    const std::string melody = "C5q. C5q. | C5q D5e E5q. | E5q D5e E5q F5e | G5h. | "
                               "C6e C6e C6e G5e G5e G5e | E5e E5e E5e C5e C5e C5e | G5q F5e E5q D5e | C5h.";
    const std::array<int, 4> patches = {52, 53, 54, 73};
    for (int v = 0; v < 4; ++v) {
        Track& t = newTrack(s, v, "Voice " + std::to_string(v + 1), v, patches[static_cast<std::size_t>(v)]);
        t.volume = 100 - Tick{v} * 10;
        t.pan = 20 + Tick{v} * 28;
        Tick at = Tick{v} * 720; // enter two measures apart
        for (int rep = 0; rep < 2; ++rep) {
            at = addMelody(t, at, melody, static_cast<std::uint8_t>(v), 96 - v * 8, 120, 0.9);
        }
    }
    addMarker(s, 0, "Voice 1");
    addMarker(s, 720, "Voice 2");
    addMarker(s, 1440, "Voice 3");
    addMarker(s, 2160, "Voice 4");
    s.setComments("Row, Row, Row Your Boat. Traditional; public domain. Four-part round, each voice sings twice.");
    return c;
}

CorpusSong frereJacques() {
    CorpusSong c{"FRERE", "Frere Jacques (traditional) as a round made with identical tracks and Time+ offsets", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 11000);
    const std::string melody = "C5q D5q E5q C5q | C5q D5q E5q C5q | E5q F5q G5h | E5q F5q G5h | "
                               "G5e A5e G5e F5e E5q C5q | G5e A5e G5e F5e E5q C5q | C5q G4q C5h | C5q G4q C5h";
    for (int v = 0; v < 4; ++v) {
        Track& t = newTrack(s, v, "Round " + std::to_string(v + 1), v, 71 + v);
        if (v > 0) {
            t.timePlus = v * 960; // two measures later, applied at playback (Time+)
        }
        addMelody(t, 0, melody, static_cast<std::uint8_t>(v), 100, 120, 0.95);
    }
    s.setComments("Frere Jacques. Traditional French round; public domain. All four tracks hold the same notes; "
                  "the Time+ parameter shifts the entries.");
    return c;
}

CorpusSong odeToJoy() {
    CorpusSong c{"ODETOJOY", "Ode to Joy theme (Beethoven, Symphony No. 9, 1824) with bass and chords", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 12000);
    Track& m = newTrack(s, 0, "Melody", 0, 56);
    m.volume = 110;
    m.pan = 64;
    const std::string a = "E5q E5q F5q G5q | G5q F5q E5q D5q | C5q C5q D5q E5q | E5q. D5e D5h | ";
    const std::string b = "E5q E5q F5q G5q | G5q F5q E5q D5q | C5q C5q D5q E5q | D5q. C5e C5h | ";
    const std::string bridge = "D5q D5q E5q C5q | D5q E5e F5e E5q C5q | D5q E5e F5e E5q D5q | C5q D5q G4h | ";
    addMelody(m, 0, a + b + bridge + b, 0, 100, 120, 0.9);
    Track& bass = newTrack(s, 1, "Bass", 1, 33);
    bass.volume = 90;
    const std::array<int, 32> roots = {48, 48, 43, 43, 48, 48, 43, 48, 48, 48, 43, 43, 48, 48, 43, 48,
                                       43, 43, 48, 48, 43, 43, 48, 43, 48, 48, 43, 43, 48, 48, 43, 48};
    for (int i = 0; i < 32; ++i) {
        bass.events.push_back(Event::note(Tick{i} * 240, 1, roots[static_cast<std::size_t>(i)], 90, 220));
    }
    Track& chords = newTrack(s, 2, "Chords", 2, 48);
    chords.volume = 70;
    chords.pan = 30;
    for (int i = 0; i < 32; ++i) {
        if (roots[static_cast<std::size_t>(i)] == 48) {
            addChord(chords, Tick{i} * 240, {60, 64, 67}, 70, 230);
        } else {
            addChord(chords, Tick{i} * 240, {59, 62, 67}, 70, 230);
        }
    }
    s.setComments("Ode to Joy, theme from the fourth movement of Beethoven's Symphony No. 9 (1824). "
                  "Public domain. Simple arrangement in C major.");
    return c;
}

CorpusSong jingleBells() {
    CorpusSong c{"JINGLE", "Jingle Bells, chorus (James Lord Pierpont, 1857) with bass", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 14000);
    Track& m = newTrack(s, 0, "Melody", 0, 9);
    m.volume = 120;
    const std::string chorus = "E5q E5q E5h | E5q E5q E5h | E5q G5q C5q. D5e | E5w | "
                               "F5q F5q F5q. F5e | F5q E5q E5q E5e E5e | E5q D5q D5q E5q | D5h G5h | "
                               "E5q E5q E5h | E5q E5q E5h | E5q G5q C5q. D5e | E5w | "
                               "F5q F5q F5q. F5e | F5q E5q E5q E5e E5e | G5q G5q F5q D5q | C5w";
    addMelody(m, 0, chorus, 0, 105, 120, 0.85);
    Track& bass = newTrack(s, 1, "Bass", 1, 32);
    bass.volume = 95;
    const std::string bassLine = "C4q G3q C4q G3q | C4q G3q C4q G3q | C4q G3q C4q G3q | C4q G3q C4q G3q | "
                                 "F3q C4q F3q C4q | C4q G3q C4q G3q | G3q D4q G3q D4q | G3q D4q G3q D4q | "
                                 "C4q G3q C4q G3q | C4q G3q C4q G3q | C4q G3q C4q G3q | C4q G3q C4q G3q | "
                                 "F3q C4q F3q C4q | C4q G3q C4q G3q | G3q D4q G3q D4q | C4h C4h";
    addMelody(bass, 0, bassLine, 1, 90, 120, 0.8);
    addMarker(s, 0, "Chorus");
    addMarker(s, Tick{8} * 480, "Chorus again");
    s.setComments("Jingle Bells, chorus. James Lord Pierpont, 1857. Public domain.");
    return c;
}

CorpusSong silentNight() {
    CorpusSong c{"SILENT", "Silent Night (Franz Xaver Gruber, 1818) in 6/8 with bass and SMPTE-locked markers", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 8400);
    setMeter(s, 1, 6, 3);
    s.timecode.rate = FrameRate::Fps30;
    Track& m = newTrack(s, 0, "Melody", 0, 73);
    const std::string melody =
        "G5q. A5e G5q | E5h. | G5q. A5e G5q | E5h. | D6h D6q | B5h. | C6h C6q | G5h. | "
        "A5h A5q | C6q. B5e A5q | G5q. A5e G5q | E5h. | A5h A5q | C6q. B5e A5q | G5q. A5e G5q | E5h. | "
        "D6h D6q | F6q. D6e B5q | C6h. | E6h. | C6q G5q E5q | G5q. F5e D5q | C5h. | C5h.";
    addMelody(m, 0, melody, 0, 88, 120, 0.95);
    Track& bass = newTrack(s, 1, "Bass", 1, 42);
    bass.volume = 80;
    const std::string bassLine = "C4h. | C4h. | C4h. | C4h. | G3h. | G3h. | C4h. | C4h. | "
                                 "F3h. | F3h. | C4h. | C4h. | F3h. | F3h. | C4h. | C4h. | "
                                 "G3h. | G3h. | C4h. | C4h. | C4h. | G3h. | C4h. | C4h.";
    addMelody(bass, 0, bassLine, 1, 75, 120, 0.9);
    addMarker(s, 0, "Verse 1");
    addMarker(s, Tick{8} * 360, "Sleep in heavenly peace", true);
    addMarker(s, Tick{16} * 360, "Second half", true);
    s.setComments("Silent Night (Stille Nacht). Franz Xaver Gruber, 1818. Public domain.");
    return c;
}

CorpusSong greensleeves() {
    CorpusSong c{"GREENS", "Greensleeves (English traditional, 16th c.) in A minor, 6/8, with a ritardando", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 9600);
    s.tempoMap.set(Tick{14} * 360, 8800);
    s.tempoMap.set(Tick{15} * 360, 7600);
    setMeter(s, 1, 6, 3);
    setKey(s, 1, 0);
    Track& m = newTrack(s, 0, "Melody", 0, 24);
    const std::string melody =
        "re A4e | C5q D5e E5q F5e E5e | D5q B4e G4q A4e B4e | C5q A4e A4q G#4e A4e | B4q G#4e E4q A4e B4e | "
        "C5q D5e E5q F5e E5e | D5q B4e G4q A4e B4e | C5e B4e A4q G#4e F#4e G#4e | A4q. A4q. | "
        "G5h. | G5q F5e E5q D5e E5e | D5q B4e G4q A4e B4e | C5q A4e A4q G#4e A4e | B4q G#4e E4q. | "
        "G5h. | G5q F5e E5q D5e E5e | A4h.";
    addMelody(m, 0, melody, 0, 84, 120, 0.9);
    Track& acc = newTrack(s, 1, "Accompaniment", 1, 46);
    acc.volume = 70;
    for (int bar = 0; bar < 16; ++bar) {
        const bool minor = bar % 4 != 3;
        addChord(acc, Tick{bar} * 360,
                 minor ? std::initializer_list<int>{45, 52, 57} : std::initializer_list<int>{43, 50, 55}, 60, 350);
    }
    s.setComments("Greensleeves. English traditional, first registered 1580. Public domain.");
    return c;
}

CorpusSong amazingGrace() {
    CorpusSong c{"AMAZING", "Amazing Grace (tune New Britain, 1829) in F major, 3/4, with a pickup", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 7200);
    setMeter(s, 1, 3, 2);
    setKey(s, 1, -1);
    Track& m = newTrack(s, 0, "Melody", 0, 20);
    const std::string melody = "rh C5q | F5h A5e F5e | A5h G5q | F5h D5q | C5h C5q | F5h A5e F5e | A5h G5q | C6h. | "
                               "C6h A5e C6e | A5h F5q | A5h A5e G5e | F5h D5q | C5h C5q | F5h A5e F5e | A5h G5q | F5h.";
    addMelody(m, 0, melody, 0, 92, 120, 0.95);
    Track& bass = newTrack(s, 1, "Bass", 1, 32);
    const std::string bassLine = "rh. | F3h. | F3h. | Bb3h. | F3h. | F3h. | C4h. | F3h. | F3h. | "
                                 "F3h. | Bb3h. | F3h. | C4h. | F3h. | C4h. | F3h. | F3h.";
    addMelody(bass, 0, bassLine, 1, 80, 120, 0.9);
    s.setComments("Amazing Grace, tune New Britain (1829). Public domain.");
    return c;
}

CorpusSong twinkle() {
    CorpusSong c{"TWINKLE",
                 "Twinkle, Twinkle, Little Star (French traditional, 18th c.) with block chords and varied velocities",
                 {}};
    Song& s = c.song;
    s.tempoMap.set(0, 10000);
    Track& m = newTrack(s, 0, "Melody", 0, 11);
    const std::string melody = "C5q C5q G5q G5q | A5q A5q G5h | F5q F5q E5q E5q | D5q D5q C5h | "
                               "G5q G5q F5q F5q | E5q E5q D5h | G5q G5q F5q F5q | E5q E5q D5h | "
                               "C5q C5q G5q G5q | A5q A5q G5h | F5q F5q E5q E5q | D5q D5q C5h";
    const Tick end = addMelody(m, 0, melody, 0, 100, 120, 0.8);
    // Varied velocities: crescendo over the piece.
    for (std::size_t i = 0; i < m.events.size(); ++i) {
        m.events[i].data2 = 60 + static_cast<int>(i * 60 / m.events.size());
    }
    Track& ch = newTrack(s, 1, "Chords", 1, 0);
    ch.volume = 75;
    const std::array<int, 24> chordRoots = {0, 0, 5, 0, 5, 0, 7, 0, 7, 0, 7, 0, 7, 0, 7, 0, 0, 0, 5, 0, 5, 0, 7, 0};
    for (int i = 0; i < 24 && Tick{i} * 240 < end; ++i) {
        const int r = 48 + chordRoots[static_cast<std::size_t>(i)];
        addChord(ch, Tick{i} * 240, {r, r + 4, r + 7}, 70, 230);
    }
    s.setComments("Twinkle, Twinkle, Little Star (Ah! vous dirai-je, maman). French traditional, 18th century. "
                  "Public domain.");
    return c;
}

CorpusSong furElise() {
    CorpusSong c{"FURELISE", "Fuer Elise, opening (Beethoven, WoO 59, 1810) in 3/8 for two hands", {}};
    Song& s = c.song;
    s.tempoMap.set(0, 7000); // eighth note = 140, in 3/8
    setMeter(s, 1, 3, 3);
    setKey(s, 1, 0);
    Track& rh = newTrack(s, 0, "Right hand", 0, 0);
    rh.pan = 80;
    // Cakewalk octave numbering: middle C = C5, so Beethoven's e'' is E6 here.
    const std::string right =
        "E6s D#6s | E6s D#6s E6s B5s D6s C6s | A5e rs C5s E5s A5s | B5e rs E5s G#5s B5s | "
        "C6e rs E5s E6s D#6s | E6s D#6s E6s B5s D6s C6s | A5e rs C5s E5s A5s | B5e rs E5s C6s B5s | A5q";
    addMelody(rh, 0, right, 0, 80, 120, 0.9);
    Track& lh = newTrack(s, 1, "Left hand", 1, 0);
    lh.pan = 40;
    const std::string left = "re | re. | A3s E4s A4s re. | E3s E4s G#4s re. | A3s E4s A4s re. | re. | "
                             "A3s E4s A4s re. | E3s E4s G#4s re. | A3e";
    addMelody(lh, 0, left, 1, 70, 120, 0.9);
    s.setComments("Fuer Elise, Bagatelle No. 25 in A minor, WoO 59, opening phrase. Ludwig van Beethoven, 1810. "
                  "Public domain.");
    return c;
}

} // namespace

std::vector<CorpusSong> allSongs() {
    return {synMinimal(),  synKinds(),     synParams(),    synMaps(),      synOverlap(), synDense(),
            synStrings(),  synWave(),      rowBoat(),      frereJacques(), odeToJoy(),   jingleBells(),
            silentNight(), greensleeves(), amazingGrace(), twinkle(),      furElise()};
}

} // namespace sseq::corpus
