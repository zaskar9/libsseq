// libsseq: model implementation (events, tracks, song, maps, time conversions).
#include "bytes.hpp"
#include "sseq/song.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <ranges>

namespace sseq {

// ---------------------------------------------------------------- events

namespace {

Event channelEvent(const Tick time, const EventKind kind, const std::uint8_t channel, const int data1,
                   const int data2 = 0, const Tick duration = 0) {
    Event e;
    e.time = time;
    e.kind = kind;
    e.channel = channel;
    e.data1 = data1;
    e.data2 = data2;
    e.duration = duration;
    return e;
}

Event stringEvent(const Tick time, const EventKind kind, const std::string& text) {
    Event e;
    e.time = time;
    e.kind = kind;
    e.payload.assign(text.begin(), text.end());
    return e;
}

} // namespace

Event Event::note(const Tick time, const std::uint8_t channel, const int key, const int velocity, const Tick duration) {
    return channelEvent(time, EventKind::Note, channel, key, velocity, duration);
}
Event Event::keyAftertouch(const Tick time, const std::uint8_t channel, const int key, const int pressure) {
    return channelEvent(time, EventKind::KeyAftertouch, channel, key, pressure);
}
Event Event::controller(const Tick time, const std::uint8_t channel, const int number, const int value) {
    return channelEvent(time, EventKind::Controller, channel, number, value);
}
Event Event::patch(const Tick time, const std::uint8_t channel, const int program) {
    return channelEvent(time, EventKind::Patch, channel, program);
}
Event Event::channelAftertouch(const Tick time, const std::uint8_t channel, const int pressure) {
    return channelEvent(time, EventKind::ChannelAftertouch, channel, pressure);
}
Event Event::pitchWheel(const Tick time, const std::uint8_t channel, const int value) {
    return channelEvent(time, EventKind::PitchWheel, channel, value);
}
Event Event::sysex(const Tick time, const int bank) {
    return channelEvent(time, EventKind::Sysex, 0, bank);
}
Event Event::textEvent(const Tick time, const std::string& text) {
    return stringEvent(time, EventKind::Text, text);
}
Event Event::mciCommand(const Tick time, const std::string& command) {
    return stringEvent(time, EventKind::MciCommand, command);
}

void sortEvents(std::vector<Event>& events) {
    std::ranges::stable_sort(events, {}, &Event::time);
}

// ---------------------------------------------------------------- tracks

bool Track::hasStringEvents() const noexcept {
    return std::ranges::any_of(events, &Event::isString);
}

Tick Track::endTime() const noexcept {
    if (events.empty()) {
        return 0;
    }
    return std::ranges::max(events | std::views::transform(&Event::end));
}

// ---------------------------------------------------------------- vars

Vars::Vars() {
    // Defaults as written by Cakewalk Apprentice 1.04 for a plain song.
    static constexpr std::array<std::uint8_t, kSize> kDefault = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xff,
        0xff, 0x01, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x40, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::ranges::copy(kDefault, raw.begin());
}

std::uint32_t Vars::u32(const std::size_t off) const noexcept {
    return detail::le(raw[off], raw[off + 1], raw[off + 2], raw[off + 3]);
}

void Vars::setU32(const std::size_t off, const std::uint32_t v) noexcept {
    raw[off] = detail::u8(v);
    raw[off + 1] = detail::u8(v >> 8);
    raw[off + 2] = detail::u8(v >> 16);
    raw[off + 3] = detail::u8(v >> 24);
}

// ---------------------------------------------------------------- song

Song::Song() {
    syncPpq();
}

Track* Song::findTrack(const int number) noexcept {
    const auto it = std::ranges::find(tracks, number, &Track::number);
    return it == tracks.end() ? nullptr : &*it;
}

const Track* Song::findTrack(const int number) const noexcept {
    const auto it = std::ranges::find(tracks, number, &Track::number);
    return it == tracks.end() ? nullptr : &*it;
}

Track& Song::track(const int number) {
    if (Track* t = findTrack(number)) {
        return *t;
    }
    Track t;
    t.number = number;
    const auto pos = std::ranges::find_if(tracks, [number](const Track& x) { return x.number > number; });
    return *tracks.insert(pos, std::move(t));
}

std::string Song::commentsText() const {
    if (!comments) {
        return {};
    }
    std::string s(comments->begin(), comments->end());
    while (!s.empty() && s.back() == '\0') {
        s.pop_back();
    }
    return s;
}

void Song::setComments(const std::string& text) {
    comments = std::vector<std::uint8_t>(text.begin(), text.end());
}

Tick Song::endTime() const noexcept {
    if (tracks.empty()) {
        return 0;
    }
    return std::ranges::max(tracks | std::views::transform(&Track::endTime));
}

// ---------------------------------------------------------------- tempo map

const TempoEntry& TempoMap::at(const Tick time) const noexcept {
    const TempoEntry* best = &entries.front();
    for (const TempoEntry& e : entries) {
        if (e.time > time) {
            break;
        }
        best = &e;
    }
    return *best;
}

void TempoMap::set(const Tick time, const int bpm100) {
    if (const auto it = std::ranges::find(entries, time, &TempoEntry::time); it != entries.end()) {
        it->bpm100 = bpm100;
        return;
    }
    TempoEntry n;
    n.time = time;
    n.bpm100 = bpm100;
    const auto pos = std::ranges::find_if(entries, [time](const TempoEntry& e) { return e.time > time; });
    entries.insert(pos, n);
}

void TempoMap::remove(const Tick time) {
    if (time == 0) {
        return; // the entry at 0 is always kept
    }
    const auto [first, last] = std::ranges::remove(entries, time, &TempoEntry::time);
    entries.erase(first, last);
}

double TempoMap::microsecondsPerTick(const TempoEntry& e) const noexcept {
    return 60.0e6 / (e.bpm() * ppq);
}

double TempoMap::tickToSeconds(const Tick tick) const noexcept {
    double seconds = 0;
    Tick last = 0;
    const TempoEntry* cur = &entries.front();
    for (std::size_t i = 1; i < entries.size() && entries[i].time <= tick; ++i) {
        seconds += static_cast<double>(entries[i].time - last) * microsecondsPerTick(*cur) / 1e6;
        last = entries[i].time;
        cur = &entries[i];
    }
    return seconds + static_cast<double>(tick - last) * microsecondsPerTick(*cur) / 1e6;
}

Tick TempoMap::secondsToTick(const double seconds) const noexcept {
    double acc = 0;
    Tick last = 0;
    const TempoEntry* cur = &entries.front();
    for (std::size_t i = 1; i < entries.size(); ++i) {
        const double span = static_cast<double>(entries[i].time - last) * microsecondsPerTick(*cur) / 1e6;
        if (acc + span > seconds) {
            break;
        }
        acc += span;
        last = entries[i].time;
        cur = &entries[i];
    }
    return last + std::llround((seconds - acc) * 1e6 / microsecondsPerTick(*cur));
}

// ---------------------------------------------------------------- meter map

const MeterEntry& MeterMap::atMeasure(const int measure) const noexcept {
    const MeterEntry* best = &entries.front();
    for (const MeterEntry& e : entries) {
        if (e.measure > measure) {
            break;
        }
        best = &e;
    }
    return *best;
}

Tick MeterMap::measureStart(const int measure) const noexcept {
    Tick tick = 0;
    int m = 1;
    const MeterEntry* cur = &entries.front();
    for (std::size_t i = 1; i < entries.size() && entries[i].measure <= measure; ++i) {
        tick += Tick{entries[i].measure - m} * measureTicks(*cur);
        m = entries[i].measure;
        cur = &entries[i];
    }
    return tick + Tick{measure - m} * measureTicks(*cur);
}

MBT MeterMap::tickToMBT(const Tick tick) const noexcept {
    Tick start = 0;
    int m = 1;
    const MeterEntry* cur = &entries.front();
    for (std::size_t i = 1; i < entries.size(); ++i) {
        const Tick next = start + Tick{entries[i].measure - m} * measureTicks(*cur);
        if (next > tick) {
            break;
        }
        start = next;
        m = entries[i].measure;
        cur = &entries[i];
    }
    const Tick mt = measureTicks(*cur);
    const Tick bt = beatTicks(*cur);
    Tick rel = tick - start;
    MBT r;
    r.measure = m + static_cast<int>(rel / mt);
    rel %= mt;
    r.beat = 1 + static_cast<int>(rel / bt);
    r.tick = static_cast<int>(rel % bt);
    return r;
}

Tick MeterMap::mbtToTick(const MBT& mbt) const noexcept {
    const MeterEntry& e = atMeasure(mbt.measure);
    return measureStart(mbt.measure) + Tick{mbt.beat - 1} * beatTicks(e) + mbt.tick;
}

Tick MeterMap::nextMeasure(const Tick tick) const noexcept {
    return measureStart(tickToMBT(tick).measure + 1);
}

Tick MeterMap::prevMeasure(const Tick tick) const noexcept {
    const MBT p = tickToMBT(tick);
    const Tick start = measureStart(p.measure);
    return start < tick ? start : measureStart(std::max(1, p.measure - 1));
}

std::string formatMBT(const MBT& mbt) {
    std::array<char, 48> buf{};
    std::snprintf(buf.data(), buf.size(), "%d:%d:%03d", mbt.measure, mbt.beat, mbt.tick);
    return buf.data();
}

bool parseMBT(const std::string& s, MBT& out) {
    int m = 0;
    int b = 0;
    int t = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &m, &b, &t) != 3) {
        return false;
    }
    out = MBT{m, b, t};
    return true;
}

// ---------------------------------------------------------------- timecode

int Timecode::nominalFps() const noexcept {
    switch (rate) {
    case FrameRate::Fps24:
        return 24;
    case FrameRate::Fps25:
        return 25;
    case FrameRate::Fps30Drop:
    case FrameRate::Fps30:
        return 30;
    }
    return 30;
}

std::int64_t Timecode::secondsToFrames(const double seconds) const noexcept {
    const double fps = dropFrame() ? 30000.0 / 1001.0 : nominalFps();
    return static_cast<std::int64_t>(std::floor(seconds * fps + 1e-9));
}

double Timecode::framesToSeconds(const std::int64_t frames) const noexcept {
    const double fps = dropFrame() ? 30000.0 / 1001.0 : nominalFps();
    return static_cast<double>(frames) / fps;
}

HMSF Timecode::framesToHMSF(std::int64_t frames) const noexcept {
    const int fps = nominalFps();
    if (dropFrame()) {
        // Standard SMPTE drop-frame: frame numbers 0 and 1 are skipped in the first second
        // of every minute except every tenth minute.
        constexpr std::int64_t perMinute = 60 * 30 - 2;
        constexpr std::int64_t perTenMinutes = 10 * perMinute + 2;
        const std::int64_t d = frames / perTenMinutes;
        std::int64_t m = frames % perTenMinutes;
        if (m >= 2) {
            m += 2 * ((m - 2) / perMinute);
        }
        frames = d * 18000 + m;
    }
    HMSF t;
    t.frames = static_cast<int>(frames % fps);
    frames /= fps;
    t.seconds = static_cast<int>(frames % 60);
    frames /= 60;
    t.minutes = static_cast<int>(frames % 60);
    frames /= 60;
    t.hours = static_cast<int>(frames);
    return t;
}

std::int64_t Timecode::hmsfToFrames(const HMSF& t) const noexcept {
    const int fps = nominalFps();
    std::int64_t total = ((std::int64_t{t.hours} * 60 + t.minutes) * 60 + t.seconds) * fps + t.frames;
    if (dropFrame()) {
        const std::int64_t minutes = std::int64_t{t.hours} * 60 + t.minutes;
        total -= 2 * (minutes - minutes / 10);
    }
    return total;
}

std::string formatHMSF(const HMSF& t) {
    std::array<char, 48> buf{};
    std::snprintf(buf.data(), buf.size(), "%02d:%02d:%02d:%02d", t.hours, t.minutes, t.seconds, t.frames);
    return buf.data();
}

bool parseHMSF(const std::string& s, HMSF& out) {
    int h = 0;
    int m = 0;
    int sec = 0;
    int f = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d:%d", &h, &m, &sec, &f) != 4) {
        return false;
    }
    out = HMSF{h, m, sec, f};
    return true;
}

// ---------------------------------------------------------------- file helpers

namespace detail {

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error("cannot open " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFileBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw Error("cannot create " + path);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte buffer passed to a char stream
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace detail
} // namespace sseq
