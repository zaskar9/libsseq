// Unit tests for the model, time conversions and the writers (no reference files needed).
#include "check.hpp"
#include "sseq/sseq.hpp"

#include <algorithm>
#include <initializer_list>
#include <vector>

using namespace sseq;

// Tests read optionals they just set; a missing value is a test failure, so the throw is wanted.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

static void testMBT() {
    MeterMap m;
    m.ppq = 120; // 4/4: beat 120, measure 480
    CHECK_EQ(formatMBT(m.tickToMBT(0)), "1:1:000");
    CHECK_EQ(formatMBT(m.tickToMBT(119)), "1:1:119");
    CHECK_EQ(formatMBT(m.tickToMBT(120)), "1:2:000");
    CHECK_EQ(formatMBT(m.tickToMBT(480)), "2:1:000");
    CHECK_EQ(formatMBT(m.tickToMBT(1680)), "4:3:000");
    CHECK_EQ(m.mbtToTick(MBT{4, 3, 0}), 1680);
    CHECK_EQ(m.mbtToTick(MBT{1, 1, 0}), 0);
    MBT p;
    CHECK(parseMBT("12:3:045", p));
    CHECK_EQ(p.measure, 12);
    CHECK_EQ(p.beat, 3);
    CHECK_EQ(p.tick, 45);

    // Meter change to 3/8 at measure 3: measures 1-2 are 480 ticks, from measure 3 on 180 ticks.
    MeterEntry e;
    e.measure = 3;
    e.numerator = 3;
    e.denominatorLog2 = 3;
    m.entries.push_back(e);
    CHECK_EQ(m.measureStart(3), 960);
    CHECK_EQ(m.measureStart(5), 960 + 2 * 180);
    CHECK_EQ(formatMBT(m.tickToMBT(960 + 180 + 60)), "4:2:000");
    CHECK_EQ(m.mbtToTick(MBT{4, 2, 0}), 960 + 180 + 60);
    CHECK_EQ(m.nextMeasure(1000), 1140);
    CHECK_EQ(m.prevMeasure(1140), 960);
    CHECK_EQ(m.prevMeasure(1141), 1140);
}

static void testTempo() {
    TempoMap t;
    t.ppq = 120;
    t.set(0, 18000); // 180 bpm: 3 beats/s, 360 ticks/s
    CHECK(std::abs(t.tickToSeconds(360) - 1.0) < 1e-9);
    CHECK_EQ(t.secondsToTick(1.0), 360);
    // 4:4:117 at 180 bpm is 1917 ticks = 5.325 s = 159.75 frames at 30 fps; Cakewalk rounds to the nearest
    // frame (00:00:05:10; its Event list shows 0.25 s as frame 08, 0.75 s as 23).
    MeterMap m;
    m.ppq = 120;
    Tick now = m.mbtToTick(MBT{4, 4, 117});
    CHECK_EQ(now, 1917);
    Timecode tc;
    tc.rate = FrameRate::Fps30;
    CHECK_EQ(tc.secondsToFrames(t.tickToSeconds(now)), 160);
    CHECK_EQ(tc.secondsToFrames(0.25), 8);
    CHECK_EQ(tc.secondsToFrames(0.75), 23);
    CHECK_EQ(formatHMSF(tc.framesToHMSF(tc.secondsToFrames(t.tickToSeconds(1800)))), "00:00:05:00");
    // Tempo change: 120 bpm for the first 480 ticks (2 s), then 60 bpm (1 tick = 1/120 s).
    t.set(0, 12000);
    t.set(480, 6000);
    CHECK(std::abs(t.tickToSeconds(480) - 2.0) < 1e-9);
    CHECK(std::abs(t.tickToSeconds(600) - 3.0) < 1e-9);
    CHECK_EQ(t.secondsToTick(3.0), 600);
    CHECK_EQ(t.at(479).bpm100, 12000);
    CHECK_EQ(t.at(480).bpm100, 6000);
}

static void testTimecode() {
    Timecode tc;
    tc.rate = FrameRate::Fps25;
    HMSF h{1, 2, 3, 0};
    CHECK_EQ(tc.hmsfToFrames(h), 93075); // the value Cakewalk Apprentice 1.04 stored for 01:02:03:00
    CHECK_EQ(formatHMSF(tc.framesToHMSF(93075)), "01:02:03:00");
    Timecode df;
    df.rate = FrameRate::Fps30Drop;
    // Drop-frame: 00:01:00:00 does not exist; 00:00:59:29 is followed by 00:01:00:02.
    CHECK_EQ(formatHMSF(df.framesToHMSF(std::int64_t{60} * 30 - 1)), "00:00:59:29");
    CHECK_EQ(formatHMSF(df.framesToHMSF(std::int64_t{60} * 30)), "00:01:00:02");
    CHECK_EQ(df.hmsfToFrames(HMSF{0, 1, 0, 2}), 1800);
    CHECK_EQ(formatHMSF(df.framesToHMSF(df.hmsfToFrames(HMSF{0, 10, 0, 0}))), "00:10:00:00");
    CHECK_EQ(formatHMSF(df.framesToHMSF(df.hmsfToFrames(HMSF{1, 23, 45, 17}))), "01:23:45:17");
}

static void testWrkRoundTripOfNewSequence() {
    Sequence s;
    Stream& t = s.stream(0);
    t.name = "Piano";
    t.channel = 0;
    t.patch = 1;
    t.volume = 100;
    t.pan = 64;
    t.events.push_back(Event::note(0, 0, 60, 100, 120));
    t.events.push_back(Event::note(120, 0, 64, 90, 120));
    t.events.push_back(Event::controller(60, 0, 1, 50));
    t.events.push_back(Event::textEvent(240, "hello"));
    Stream& u = s.stream(2);
    u.name = "Bass";
    u.muted = true;
    u.loop = true;
    u.reps = 3;
    u.timePlus = -10;
    u.events.push_back(Event::note(0, 1, 36, 80, 480));
    Marker m;
    m.time = 480;
    m.name = "Verse";
    s.markers.push_back(m);
    s.tempoMap.set(0, 9000);
    s.tempoMap.set(480, 10000);
    s.setComments("a comment");

    std::vector<std::uint8_t> bytes = wrk::write(s);
    Sequence r = wrk::read(bytes);
    CHECK_EQ(r.streams.size(), 2U);
    CHECK_EQ(r.streams[0].name, "Piano");
    CHECK_EQ(r.streams[0].events.size(), 4U);
    CHECK(r.streams[0].events[1].kind == EventKind::Controller);
    CHECK_EQ(r.streams[0].events[3].text(), "hello");
    CHECK_EQ(r.streams[0].patch.value(), 1);
    CHECK_EQ(r.streams[0].volume.value(), 100);
    CHECK_EQ(r.streams[0].pan.value(), 64);
    CHECK_EQ(r.streams[1].number, 2);
    CHECK_EQ(r.streams[1].muted, true);
    CHECK_EQ(r.streams[1].loop, true);
    CHECK_EQ(r.streams[1].reps.value(), 3);
    CHECK_EQ(r.streams[1].timePlus.value(), -10);
    CHECK_EQ(r.markers.size(), 1U);
    CHECK_EQ(r.markers[0].name, "Verse");
    CHECK_EQ(r.markers[0].time, 480);
    CHECK_EQ(r.tempoMap.entries.size(), 2U);
    CHECK_EQ(r.tempoMap.entries[1].bpm100, 10000);
    CHECK_EQ(r.commentsText(), "a comment");
    // Writing the re-read sequence in Preserve mode reproduces the bytes.
    CHECK(wrk::write(r) == bytes);
    // And so does Normalize mode, since the first write was normalized.
    CHECK(wrk::write(r, wrk::WriteMode::Normalize) == bytes);
}

static void testSmfRoundTrip() {
    Sequence s;
    Stream& t = s.stream(0);
    t.name = "Lead";
    t.channel = 3;
    t.patch = 5;
    t.volume = 90;
    t.pan = 32;
    t.keyPlus = 2;
    t.events.push_back(Event::note(0, 3, 60, 100, 100));
    t.events.push_back(Event::note(100, 3, 62, 100, 100)); // starts when the first ends
    t.events.push_back(Event::note(100, 3, 65, 100, 50));
    t.events.push_back(Event::textEvent(50, "cue"));
    s.tempoMap.set(0, 10000);
    Marker m;
    m.time = 200;
    m.name = "M";
    m.smpteLocked = true;
    s.markers.push_back(m);

    std::vector<std::uint8_t> mid = smf::write(s);
    Sequence r = smf::read(mid);
    CHECK_EQ(r.streams.size(), 1U);
    const Stream& u = r.streams[0];
    CHECK_EQ(u.name, "Lead");
    CHECK_EQ(u.channel.value(), 3);
    CHECK_EQ(u.patch.value(), 5);
    CHECK_EQ(u.volume.value(), 90);
    CHECK_EQ(u.pan.value(), 32);
    CHECK_EQ(u.events.size(), 4U);
    CHECK_EQ(u.events[0].data1, 62); // Key+ baked in
    CHECK_EQ(u.events[0].duration, 100);
    CHECK_EQ(u.events[1].text(), "cue");
    CHECK_EQ(u.events[2].duration, 100);
    CHECK_EQ(u.events[3].duration, 50);
    CHECK_EQ(r.tempoMap.entries[0].bpm100, 10000);
    CHECK_EQ(r.markers.size(), 1U);
    CHECK_EQ(r.markers[0].smpteLocked, true);
    CHECK_EQ(r.markers[0].time, 200);
    // Without the Cakewalk port meta event the file is 5 bytes shorter per stream and reads the same.
    smf::WriteOptions plain;
    plain.portMeta = false;
    const std::vector<std::uint8_t> plainMid = smf::write(s, plain);
    CHECK_EQ(plainMid.size(), mid.size() - 5);
    CHECK_EQ(smf::read(plainMid).streams[0].events.size(), 4U);
    CHECK(std::ranges::search(plainMid, std::vector<std::uint8_t>{0xff, 0x21}).empty());
}

// A DOS-era Cakewalk file (version 2.0 as written by Cakewalk Professional / Cakewalk 4 for DOS)
// built from the documented layout: old TEMPO chunk with the cached double, METER records with
// cached ticks, MEMRGN chunks and the Cakewalk Professional-only chunk 25. None of these can be
// produced by Cakewalk Apprentice 1.04, so they are tested with fixtures.
static std::vector<std::uint8_t> legacyFixture() {
    std::vector<std::uint8_t> b = {'C', 'A', 'K', 'E', 'W', 'A', 'L', 'K', 0x1a, 0, 2};
    const auto chunk = [&b](const std::uint8_t type, std::initializer_list<std::uint8_t> body) {
        b.push_back(type);
        const auto len = static_cast<std::uint32_t>(body.size());
        b.insert(b.end(), {static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8), 0, 0});
        b.insert(b.end(), body);
    };
    chunk(10, {0x78, 0x00}); // TIMEBASE 120
    std::vector<std::uint8_t> vars(89, 0);
    vars[18] = 1;
    vars[19] = 1;
    b.push_back(3);
    b.insert(b.end(), {89, 0, 0, 0});
    b.insert(b.end(), vars.begin(), vars.end());                                               // VARS
    chunk(8, {0x02, 0x00, '\n', 0x00});                                                        // COMMENTS "\n\0"
    chunk(1, {0x00, 0x00, 0x05, 'T', 'h', 'e', 'm', 'e', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // TRACK 1
    chunk(2, {0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x90, 0x3c, 0x64, 0x78, 0x00});        // STREAM: one note
    // TEMPO: 90 bpm, with the cached usec/tick as a double (5555.555...)
    chunk(4, {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
              0x5a, 0x00, 0x39, 0x8e, 0xe3, 0x38, 0x8e, 0xb3, 0xb5, 0x40});
    // METER: measure 1, 4/4, cached ticks/beat 120 and ticks/measure 480
    chunk(5, {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x02, 0x78, 0x00, 0xe0, 0x01});
    for (std::uint8_t i = 0; i < 10; ++i) { // MEMRGN x 10
        std::vector<std::uint8_t> m(42, 0);
        m[0] = i;
        b.push_back(7);
        b.insert(b.end(), {42, 0, 0, 0});
        b.insert(b.end(), m.begin(), m.end());
    }
    chunk(11, {0x1e, 0x00, 0x00, 0x00, 0x00, 0x00}); // TIMEFMT 30 fps
    std::vector<std::uint8_t> c25 = {0x10, 0x00};    // chunk 25: 16 x {7, 63, i, 0}
    for (std::uint8_t i = 0; i < 16; ++i) {
        c25.insert(c25.end(), {7, 63, i, 0});
    }
    b.push_back(25);
    b.insert(b.end(), {static_cast<std::uint8_t>(c25.size()), 0, 0, 0});
    b.insert(b.end(), c25.begin(), c25.end());
    b.push_back(0xff);
    return b;
}

static void testLegacyFixture() {
    const std::vector<std::uint8_t> bytes = legacyFixture();
    const Sequence s = wrk::read(bytes);
    CHECK_EQ(s.ppq, 120);
    CHECK_EQ(s.legacyTempoChunk, true);
    CHECK_EQ(s.tempoMap.entries.size(), 1U);
    CHECK_EQ(s.tempoMap.entries[0].bpm100, 9000);
    CHECK_EQ(s.tempoMap.entries[0].reserved2[7], 0x40); // the cached double is carried
    CHECK_EQ(s.meterMap.entries[0].reserved2[0], 0x78); // and so are the cached meter ticks
    CHECK_EQ(s.unknownChunks.size(), 11U);              // 10 MEMRGN + chunk 25
    CHECK_EQ(s.unknownChunks[10].type, 25);
    CHECK_EQ(s.streams.size(), 1U);
    CHECK_EQ(s.streams[0].name, "Theme");
    CHECK_EQ(s.streams[0].events[0].data1, 60);
    CHECK_EQ(s.commentsText(), "\n");
    CHECK_EQ(s.layout.size(), 19U); // 8 modelled chunks, 10 MEMRGN, chunk 25
    // Lossless: Preserve mode reproduces the file, legacy chunks included.
    CHECK(wrk::write(s, wrk::WriteMode::Preserve) == bytes);
    // Normalize mode upgrades it: NTEMPO instead of TEMPO, no MEMRGN, no chunk 25, TRKNAME added.
    const Sequence n = wrk::read(wrk::write(s, wrk::WriteMode::Normalize));
    CHECK_EQ(n.legacyTempoChunk, false);
    CHECK_EQ(n.tempoMap.entries[0].bpm100, 9000);
    CHECK_EQ(n.unknownChunks.size(), 0U);
    CHECK_EQ(n.streams[0].name, "Theme");
    CHECK(std::ranges::any_of(n.layout, [](const LayoutEntry& e) { return e.type == wrk::TRKNAME; }));
}

static void testMalformedInput() {
    const auto throws = [](const std::vector<std::uint8_t>& b) {
        try {
            (void)wrk::read(b);
        } catch (const Error&) {
            return true;
        }
        return false;
    };
    CHECK(throws({}));
    CHECK(throws({'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0, 120})); // an SMF, not a WRK
    std::vector<std::uint8_t> truncated = legacyFixture();
    truncated.resize(truncated.size() / 2);
    CHECK(throws(truncated));
    std::vector<std::uint8_t> badLen = legacyFixture();
    badLen[12] = 0xff; // TIMEBASE length byte -> chunk runs past the end
    CHECK(throws(badLen));
    const auto smfThrows = [](const std::vector<std::uint8_t>& b) {
        try {
            (void)smf::read(b);
        } catch (const Error&) {
            return true;
        }
        return false;
    };
    CHECK(smfThrows({}));
    CHECK(smfThrows(legacyFixture()));
    CHECK(smfThrows({'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 2, 0, 1, 0, 120})); // format 2
}

// NOLINTEND(bugprone-unchecked-optional-access)

/// A format 0 file with two channels (as the classic General MIDI demo files are): the track must
/// get no forced channel, keep every channel's programs, and turn its text into markers. A
/// single-channel track gets the channel and the tick-0 parameters, even after a note at tick 0.
static void testSmfImportRules() {
    const auto file = [](const std::vector<std::uint8_t>& track) {
        std::vector<std::uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 120, 'M', 'T', 'r', 'k'};
        const auto n = static_cast<std::uint32_t>(track.size());
        b.insert(b.end(), {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
                           static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)});
        b.insert(b.end(), track.begin(), track.end());
        return b;
    };
    // Mixed: program on ch 1 and ch 2 at tick 0, CC7 on ch 2 after a note at tick 0, a text meta.
    const std::vector<std::uint8_t> mixed = {0,   0xff, 0x01, 5,   'h', 'e',  'l', 'l',
                                             'o',                  // text at 0 -> marker (first track)
                                             0,   0xc0, 5,         // program 5 on ch 1
                                             0,   0xc1, 9,         // program 9 on ch 2
                                             0,   0x90, 60,   100, // note on ch 1
                                             0,   0xb1, 7,    100, // CC7 on ch 2 at tick 0, after a note
                                             120, 0x80, 60,   0,   // note off
                                             0,   0x91, 64,   90,  120, 0x81, 64,  0, // note on ch 2
                                             0,   0xff, 0x2f, 0};
    Sequence m = smf::read(file(mixed));
    CHECK_EQ(m.streams.size(), 1U);
    CHECK(!m.streams[0].channel);
    CHECK(!m.streams[0].patch);
    CHECK(!m.streams[0].volume);
    CHECK_EQ(m.streams[0].events.size(), 5U); // 2 programs, CC7, 2 notes; text became a marker
    CHECK_EQ(m.markers.size(), 1U);
    CHECK_EQ(m.markers[0].name, "hello");
    // Apprentice's own (lossy) behaviour on request.
    smf::ReadOptions apprentice;
    apprentice.absorbParametersOnMixedTracks = true;
    Sequence a = smf::read(file(mixed), apprentice);
    CHECK_EQ(a.streams[0].patch.value_or(-1), 9); // the last program at tick 0, whatever the channel
    CHECK_EQ(a.streams[0].volume.value_or(-1), 100);
    CHECK_EQ(a.streams[0].events.size(), 2U);
    // Single channel: forced channel, parameters absorbed although they follow a note at tick 0.
    const std::vector<std::uint8_t> single = {0,  0xc2, 5, 0,    0x92, 60,  100,  0,  0xb2, 7, 90,   0,    0xb2,
                                              10, 20,   0, 0xc2, 6,    120, 0x82, 60, 0,    0, 0xff, 0x2f, 0};
    Sequence s = smf::read(file(single));
    CHECK_EQ(s.streams.size(), 1U);
    CHECK_EQ(static_cast<int>(s.streams[0].channel.value_or(99)), 2);
    CHECK_EQ(s.streams[0].patch.value_or(-1), 6); // the last one wins
    CHECK_EQ(s.streams[0].volume.value_or(-1), 90);
    CHECK_EQ(s.streams[0].pan.value_or(-1), 20);
    CHECK_EQ(s.streams[0].events.size(), 1U);
    CHECK_EQ(s.ppq, 120);
}

/// Text forms as Cakewalk's Event list shows them (observed in Cakewalk Apprentice 1.04).
static void testDisplay() {
    using namespace display;
    CHECK_EQ(keyName(60), "C 5");
    CHECK_EQ(keyName(0), "C 0");
    CHECK_EQ(keyName(127), "G 10");
    CHECK_EQ(keyName(56), "G#4");
    CHECK_EQ(keyName(48), "C 4");
    int key = -1;
    CHECK(parseKeyName("C 5", key));
    CHECK_EQ(key, 60);
    CHECK(parseKeyName("G#4", key));
    CHECK_EQ(key, 56);
    CHECK(parseKeyName("g10", key));
    CHECK_EQ(key, 127);
    CHECK(parseKeyName("Bb3", key));
    CHECK_EQ(key, 46);
    CHECK(!parseKeyName("H2", key));
    CHECK(!parseKeyName("C 11", key));
    CHECK_EQ(durationText(50, 120), "50");
    CHECK_EQ(durationText(108, 120), "108"); // less than a quarter note, whatever the meter
    CHECK_EQ(durationText(120, 120), "1:000");
    CHECK_EQ(durationText(200, 120), "1:080");
    CHECK_EQ(durationText(65535, 120), "546:015");
    Tick ticks = 0;
    CHECK(parseDurationText("1:080", 120, ticks));
    CHECK_EQ(ticks, 200);
    CHECK(parseDurationText("50", 120, ticks));
    CHECK_EQ(ticks, 50);
    CHECK(!parseDurationText("x", 120, ticks));
    CHECK_EQ(kindName(EventKind::KeyAftertouch), "KeyAft");
    CHECK_EQ(kindName(EventKind::Controller), "Contrl");
    CHECK_EQ(kindName(EventKind::PitchWheel), "Wheel");
    CHECK_EQ(kindName(EventKind::Sysex), "Sysx");
    CHECK_EQ(kindName(EventKind::MciCommand), "MCICmd");
    CHECK_EQ(generalMidiProgramName(0), "Acoustic Grand Piano");
    CHECK_EQ(generalMidiProgramName(20), "Reed Organ");
    CHECK_EQ(generalMidiProgramName(127), "Gunshot");
    CHECK_EQ(generalMidiProgramName(128), "");
    CHECK_EQ(generalMidiPercussionName(42), "Closed Hi-Hat");
    CHECK_EQ(generalMidiPercussionName(34), "");
    CHECK_EQ(valuesText(Event::note(0, 2, 60, 100, 240), 120), "C 5  100  2:000");
    CHECK_EQ(valuesText(Event::controller(0, 2, 7, 100), 120), "7  100");
    CHECK_EQ(valuesText(Event::patch(0, 2, 20), 120), "Reed Organ");
    CHECK_EQ(valuesText(Event::pitchWheel(0, 2, -8192), 120), "-8192");
    CHECK_EQ(valuesText(Event::keyAftertouch(0, 2, 60, 80), 120), "C 5  80");
    CHECK_EQ(valuesText(Event::channelAftertouch(0, 2, 90), 120), "90");
    CHECK_EQ(valuesText(Event::sysex(0, 3), 120), "3");
    CHECK_EQ(valuesText(Event::textEvent(0, "text at start"), 120), "text at start");
    CHECK_EQ(channelText(Event::note(0, 2, 60, 100, 240)), "3");
    CHECK_EQ(channelText(Event::sysex(0, 0)), "n/a");
    CHECK_EQ(channelText(Event::textEvent(0, "x")), "n/a");
}

int main() { // NOLINT(bugprone-exception-escape): a throwing check is a failed test
    testMBT();
    testTempo();
    testTimecode();
    testWrkRoundTripOfNewSequence();
    testSmfRoundTrip();
    testSmfImportRules();
    testDisplay();
    testLegacyFixture();
    testMalformedInput();
    return test::report("unit tests");
}
