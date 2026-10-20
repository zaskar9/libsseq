// Unit tests for the model, time conversions and the writers (no reference files needed).
#include "check.hpp"
#include "sseq/sseq.hpp"

using namespace sseq;

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
    // 4:4:117 at 180 bpm is 1917 ticks = 5.325 s = frame 159 at 30 fps (Apprentice showed 00:00:05:10
    // in a screenshot, but its two displays are not sampled at the same instant, so 09/10 is ambiguous).
    MeterMap m;
    m.ppq = 120;
    Tick now = m.mbtToTick(MBT{4, 4, 117});
    CHECK_EQ(now, 1917);
    Timecode tc;
    tc.rate = FrameRate::Fps30;
    CHECK_EQ(tc.secondsToFrames(t.tickToSeconds(now)), 159);
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
    CHECK_EQ(tc.hmsfToFrames(h), 93075); // the value Apprentice stored for 01:02:03:00
    CHECK_EQ(formatHMSF(tc.framesToHMSF(93075)), "01:02:03:00");
    Timecode df;
    df.rate = FrameRate::Fps30Drop;
    // Drop-frame: 00:01:00:00 does not exist; 00:00:59:29 is followed by 00:01:00:02.
    CHECK_EQ(formatHMSF(df.framesToHMSF(60 * 30 - 1)), "00:00:59:29");
    CHECK_EQ(formatHMSF(df.framesToHMSF(60 * 30)), "00:01:00:02");
    CHECK_EQ(df.hmsfToFrames(HMSF{0, 1, 0, 2}), 1800);
    CHECK_EQ(formatHMSF(df.framesToHMSF(df.hmsfToFrames(HMSF{0, 10, 0, 0}))), "00:10:00:00");
    CHECK_EQ(formatHMSF(df.framesToHMSF(df.hmsfToFrames(HMSF{1, 23, 45, 17}))), "01:23:45:17");
}

static void testWrkRoundTripOfNewSong() {
    Song s;
    Track& t = s.track(0);
    t.name = "Piano";
    t.channel = 0;
    t.patch = 1;
    t.volume = 100;
    t.pan = 64;
    t.events.push_back(Event::note(0, 0, 60, 100, 120));
    t.events.push_back(Event::note(120, 0, 64, 90, 120));
    t.events.push_back(Event::controller(60, 0, 1, 50));
    t.events.push_back(Event::textEvent(240, "hello"));
    Track& u = s.track(2);
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
    Song r = wrk::read(bytes);
    CHECK_EQ(r.tracks.size(), 2u);
    CHECK_EQ(r.tracks[0].name, "Piano");
    CHECK_EQ(r.tracks[0].events.size(), 4u);
    CHECK(r.tracks[0].events[1].kind == EventKind::Controller);
    CHECK_EQ(r.tracks[0].events[3].text(), "hello");
    CHECK_EQ(*r.tracks[0].patch, 1);
    CHECK_EQ(*r.tracks[0].volume, 100);
    CHECK_EQ(*r.tracks[0].pan, 64);
    CHECK_EQ(r.tracks[1].number, 2);
    CHECK_EQ(r.tracks[1].muted, true);
    CHECK_EQ(r.tracks[1].loop, true);
    CHECK_EQ(*r.tracks[1].reps, 3);
    CHECK_EQ(*r.tracks[1].timePlus, -10);
    CHECK_EQ(r.markers.size(), 1u);
    CHECK_EQ(r.markers[0].name, "Verse");
    CHECK_EQ(r.markers[0].time, 480);
    CHECK_EQ(r.tempoMap.entries.size(), 2u);
    CHECK_EQ(r.tempoMap.entries[1].bpm100, 10000);
    CHECK_EQ(r.commentsText(), "a comment");
    // Writing the re-read song in Preserve mode reproduces the bytes.
    CHECK(wrk::write(r) == bytes);
    // And so does Normalize mode, since the first write was normalized.
    CHECK(wrk::write(r, wrk::WriteMode::Normalize) == bytes);
}

static void testSmfRoundTrip() {
    Song s;
    Track& t = s.track(0);
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
    Song r = smf::read(mid);
    CHECK_EQ(r.tracks.size(), 1u);
    const Track& u = r.tracks[0];
    CHECK_EQ(u.name, "Lead");
    CHECK_EQ(*u.channel, 3);
    CHECK_EQ(*u.patch, 5);
    CHECK_EQ(*u.volume, 90);
    CHECK_EQ(*u.pan, 32);
    CHECK_EQ(u.events.size(), 4u);
    CHECK_EQ(u.events[0].data1, 62); // Key+ baked in
    CHECK_EQ(u.events[0].duration, 100);
    CHECK_EQ(u.events[1].text(), "cue");
    CHECK_EQ(u.events[2].duration, 100);
    CHECK_EQ(u.events[3].duration, 50);
    CHECK_EQ(r.tempoMap.entries[0].bpm100, 10000);
    CHECK_EQ(r.markers.size(), 1u);
    CHECK_EQ(r.markers[0].smpteLocked, true);
    CHECK_EQ(r.markers[0].time, 200);
}

int main() {
    testMBT();
    testTempo();
    testTimecode();
    testWrkRoundTripOfNewSong();
    testSmfRoundTrip();
    return test::report("unit tests");
}
