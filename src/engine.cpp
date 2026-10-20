// libsseq: playback engine implementation.
#include "sseq/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <ranges>
#include <tuple>

namespace sseq::engine {

// ---------------------------------------------------------------- clock

namespace {
double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

SteadyClock::SteadyClock() : origin_(steadySeconds()) {}

double SteadyClock::now() const {
    return steadySeconds() - origin_;
}

// ---------------------------------------------------------------- options

PlaybackOptions PlaybackOptions::fromSequence(const Sequence& sequence, const bool recording) {
    PlaybackOptions o;
    o.chase = sequence.vars.patchSearchBack();
    o.zeroControllersOnStop = sequence.vars.zeroControllers();
    o.metronome = recording ? sequence.vars.metronomeRecording() : sequence.vars.metronomePlayback();
    o.metronomeAccent = sequence.vars.metronomeAccent();
    o.countInMeasures = recording ? sequence.vars.countIn() : 0;
    o.stopAtEnd = !recording;
    return o;
}

// ---------------------------------------------------------------- playlist

namespace {

constexpr std::uint8_t clamp7(const int v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 127));
}

enum class ItemKind : std::uint8_t { NoteOff, Control, NoteOn, Sysex, String };

/// One output of the playlist: a short message, a sysex bank, or a string event.
struct Item {
    Tick tick = 0;
    ItemKind kind = ItemKind::NoteOn;
    int stream = 0; // stream number
    int port = 0;
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> data{};
    int bank = -1;                // Sysex
    const Event* event = nullptr; // String
};

MidiMessage toMessage(const Item& it, const double time) {
    MidiMessage m;
    m.time = time;
    m.port = it.port;
    m.size = it.size;
    m.data = it.data;
    return m;
}

Item shortItem(const Tick tick, const ItemKind kind, const Stream& t, const std::uint8_t status, const std::uint8_t d1,
               const int d2 = -1) {
    Item it;
    it.tick = tick;
    it.kind = kind;
    it.stream = t.number;
    it.port = t.port;
    it.data[0] = status;
    it.data[1] = d1;
    if (d2 >= 0) {
        it.data[2] = static_cast<std::uint8_t>(d2);
        it.size = 3;
    } else {
        it.size = 2;
    }
    return it;
}

/// Period of a looped stream: its end rounded up to a whole measure.
Tick loopPeriod(const Sequence& sequence, const Stream& t) {
    const Tick end = t.endTime();
    return end <= 0 ? 0 : sequence.meterMap.nextMeasure(end - 1);
}

/// Loop count 9999 means "keep looping as long as the non-looping streams are still playing".
constexpr int kLoopForever = 9999;

/// End of the last non-looping stream (with Time+ applied); 0 when there is none.
Tick nonLoopingEnd(const Sequence& sequence) {
    Tick end = 0;
    for (const Stream& t : sequence.streams) {
        if (t.reps.value_or(1) <= 1) {
            end = std::max(end, t.endTime() + t.timePlus.value_or(0));
        }
    }
    return end;
}

/// Builds the sorted list of outputs of all streams (mute and solo are applied at scheduling time).
std::vector<Item> buildPlaylist(const Sequence& sequence) {
    std::vector<Item> items;
    const Tick sequenceEnd = nonLoopingEnd(sequence);
    for (const Stream& t : sequence.streams) {
        int reps = std::max(1, t.reps.value_or(1));
        const Tick offset = t.timePlus.value_or(0);
        const Tick period = reps > 1 ? loopPeriod(sequence, t) : 0;
        // Items of an endlessly looping stream stop at the end of the non-looping streams.
        std::optional<Tick> clip;
        if (reps == kLoopForever) {
            reps = 1;
            if (period > 0 && sequenceEnd > offset) {
                clip = sequenceEnd;
                reps = static_cast<int>((sequenceEnd - offset + period - 1) / period);
            }
        }
        for (int rep = 0; rep < reps; ++rep) {
            const Tick shift = offset + rep * period;
            for (const Event& e : t.events) {
                const Tick tick = e.time + shift;
                if (tick < 0) {
                    continue; // Time- pushed it before the start
                }
                if (clip && tick >= *clip) {
                    continue;
                }
                const std::uint8_t ch = t.channel.value_or(e.channel) & 0x0fU;
                switch (e.kind) {
                case EventKind::Note: {
                    const std::uint8_t key = clamp7(e.data1 + t.keyPlus);
                    const std::uint8_t vel = std::max<std::uint8_t>(1, clamp7(e.data2 + t.velPlus));
                    items.push_back(shortItem(tick, ItemKind::NoteOn, t, 0x90 | ch, key, vel));
                    Tick off = tick + std::max<Tick>(1, e.duration);
                    if (clip) {
                        off = std::min(off, *clip);
                    }
                    items.push_back(shortItem(off, ItemKind::NoteOff, t, 0x90 | ch, key, 0));
                    break;
                }
                case EventKind::KeyAftertouch:
                    items.push_back(
                        shortItem(tick, ItemKind::Control, t, 0xa0 | ch, clamp7(e.data1 + t.keyPlus), clamp7(e.data2)));
                    break;
                case EventKind::Controller:
                    items.push_back(shortItem(tick, ItemKind::Control, t, 0xb0 | ch, clamp7(e.data1), clamp7(e.data2)));
                    break;
                case EventKind::Patch:
                    items.push_back(shortItem(tick, ItemKind::Control, t, 0xc0 | ch, clamp7(e.data1)));
                    break;
                case EventKind::ChannelAftertouch:
                    items.push_back(shortItem(tick, ItemKind::Control, t, 0xd0 | ch, clamp7(e.data1)));
                    break;
                case EventKind::PitchWheel: {
                    const int v = std::clamp(e.data1 + 8192, 0, 16383);
                    items.push_back(shortItem(tick, ItemKind::Control, t, 0xe0 | ch,
                                              static_cast<std::uint8_t>(v & 0x7f), (v >> 7) & 0x7f));
                    break;
                }
                case EventKind::Sysex: {
                    Item it;
                    it.tick = tick;
                    it.kind = ItemKind::Sysex;
                    it.stream = t.number;
                    it.port = t.port;
                    it.bank = e.data1;
                    items.push_back(it);
                    break;
                }
                case EventKind::MciCommand:
                case EventKind::Wave:
                case EventKind::Text: {
                    Item it;
                    it.tick = tick;
                    it.kind = ItemKind::String;
                    it.stream = t.number;
                    it.port = t.port;
                    it.event = &e;
                    items.push_back(it);
                    break;
                }
                }
            }
        }
    }
    std::ranges::stable_sort(
        items, [](const Item& a, const Item& b) { return std::tie(a.tick, a.kind) < std::tie(b.tick, b.kind); });
    return items;
}

/// Messages that establish the state at `start`: the streams' parameters, then (with chase) the
/// last patch, pitch wheel, modulation wheel and sustain pedal before `start` on every port and
/// channel (the four kinds Cakewalk's "patch/controller search-back" looks for).
std::vector<Item> startupItems(const Sequence& sequence, const std::vector<Item>& playlist, const Tick start,
                               const PlaybackOptions& o) {
    std::vector<Item> out;
    if (o.sendStreamParameters) {
        for (const Stream& t : sequence.streams) {
            if (t.events.empty()) {
                continue;
            }
            std::uint8_t ch = 0;
            if (t.channel) {
                ch = *t.channel & 0x0fU;
            } else if (const auto first = std::ranges::find_if(t.events, &Event::isMidi); first != t.events.end()) {
                ch = first->channel & 0x0fU;
            }
            if (t.patch) {
                out.push_back(shortItem(start, ItemKind::Control, t, 0xc0 | ch, clamp7(*t.patch)));
            }
            if (t.volume && *t.volume >= 0) {
                out.push_back(shortItem(start, ItemKind::Control, t, 0xb0 | ch, 7, clamp7(*t.volume)));
            }
            if (t.pan && *t.pan >= 0) {
                out.push_back(shortItem(start, ItemKind::Control, t, 0xb0 | ch, 10, clamp7(*t.pan)));
            }
        }
    }
    if (o.chase) {
        // key: port, status byte (with channel), controller number or 0 -> the latest item
        std::map<std::tuple<int, std::uint8_t, std::uint8_t>, Item> latest;
        for (const Item& it : playlist) {
            if (it.tick >= start) {
                break;
            }
            if (it.kind != ItemKind::Control) {
                continue;
            }
            const std::uint8_t status = it.data[0];
            const std::uint8_t kind = status & 0xf0U;
            if (kind == 0xc0 || kind == 0xe0 || (kind == 0xb0 && (it.data[1] == 1 || it.data[1] == 64))) {
                latest[{it.port, status, kind == 0xb0 ? it.data[1] : std::uint8_t{0}}] = it;
            }
        }
        for (auto& [key, it] : latest) {
            Item chased = it;
            chased.tick = start;
            out.push_back(chased);
        }
    }
    return out;
}

MidiMessage message3(const double time, const int port, const std::uint8_t status, const std::uint8_t d1,
                     const std::uint8_t d2) {
    MidiMessage m;
    m.time = time;
    m.port = port;
    m.size = 3;
    m.data = {status, d1, d2};
    return m;
}

/// Messages that silence a channel's continuous controllers, as Cakewalk's "Zero continuous
/// controllers" does: lift the sustain pedal, centre the pitch wheel, zero the modulation wheel, and
/// "reset all controllers".
void zeroControllers(std::vector<MidiMessage>& out, const double time, const int port, const std::uint8_t ch) {
    const auto cc = static_cast<std::uint8_t>(0xb0 | ch);
    out.push_back(message3(time, port, cc, 64, 0));
    out.push_back(message3(time, port, static_cast<std::uint8_t>(0xe0 | ch), 0, 64));
    out.push_back(message3(time, port, cc, 1, 0));
    out.push_back(message3(time, port, cc, 121, 0));
}

/// Ports the sequence's streams send to.
std::set<int> portsInUse(const Sequence& sequence) {
    std::set<int> ports;
    for (const Stream& t : sequence.streams) {
        if (!t.events.empty()) {
            ports.insert(t.port);
        }
    }
    if (ports.empty()) {
        ports.insert(0);
    }
    return ports;
}

/// The beat at or after `tick`, and whether it is the first beat of its measure.
std::pair<Tick, bool> beatAtOrAfter(const MeterMap& meters, const Tick tick) {
    const MBT mbt = meters.tickToMBT(tick);
    const MeterEntry& m = meters.atMeasure(mbt.measure);
    const Tick start = meters.measureStart(mbt.measure);
    const Tick bt = meters.beatTicks(m);
    const Tick k = bt > 0 ? (tick - start + bt - 1) / bt : 0;
    if (k >= m.numerator || bt <= 0) {
        return {meters.nextMeasure(tick), true};
    }
    return {start + k * bt, k == 0};
}

} // namespace

// ---------------------------------------------------------------- transport

struct Transport::Impl {
    const Sequence* sequence;
    Clock* clock;
    MidiSink* sink;
    PlaybackOptions options;
    StringEventHandler stringHandler;

    std::vector<Item> playlist;
    std::set<int> muted;
    std::set<int> solo;

    bool playing = false;
    Tick position = 0;      // position while stopped
    double anchorClock = 0; // clock time of anchorTick while playing
    Tick anchorTick = 0;
    std::size_t next = 0;        // next playlist item to schedule
    double scheduledThrough = 0; // clock time up to which items have been scheduled
    double endClock = 0;         // clock time at which playback ends by itself
    bool reachedEnd = false;
    std::map<std::tuple<int, std::uint8_t, std::uint8_t>, int> sounding; // (port, status, key) -> notes sounding
    Tick clickTick = 0;                                                  // next metronome beat to schedule
    std::vector<std::pair<double, bool>> countInClicks;                  // (clock time, downbeat), ascending
    std::multimap<double, MidiMessage> clickOffs;                        // metronome note-offs not yet sent

    Impl(const Sequence& s, Clock& c, MidiSink& k, const PlaybackOptions& o)
        : sequence(&s), clock(&c), sink(&k), options(o), playlist(buildPlaylist(s)) {
        for (const Stream& t : s.streams) {
            if (t.muted) {
                muted.insert(t.number);
            }
        }
    }

    [[nodiscard]] double sequenceSeconds(const Tick tick) const { return sequence->tempoMap.tickToSeconds(tick); }
    [[nodiscard]] double tickToClock(const Tick tick) const {
        return anchorClock + (sequenceSeconds(tick) - sequenceSeconds(anchorTick)) / options.tempoRatio;
    }
    [[nodiscard]] Tick clockToTick(const double time) const {
        if (time <= anchorClock) {
            return anchorTick; // before the anchor only during the count-in
        }
        return sequence->tempoMap.secondsToTick(sequenceSeconds(anchorTick) +
                                                (time - anchorClock) * options.tempoRatio);
    }
    [[nodiscard]] bool audible(const int stream) const {
        if (!solo.empty()) {
            return solo.contains(stream);
        }
        return !muted.contains(stream);
    }
    [[nodiscard]] Tick lastTick() const { return playlist.empty() ? 0 : playlist.back().tick; }
    [[nodiscard]] bool clickEnabled() const {
        return options.metronome && sequence->metronome.velocity > 0 && sequence->metronome.duration > 0;
    }
    [[nodiscard]] bool countInEnabled() const {
        return options.countInMeasures > 0 && sequence->metronome.velocity > 0 && sequence->metronome.duration > 0;
    }

    /// A metronome tick: note on at `time`, note off `durationSeconds` later.
    void click(const double time, const double durationSeconds, const bool downbeat) {
        const Metronome& m = sequence->metronome;
        const auto status = static_cast<std::uint8_t>(0x90 | (m.channel & 0x0f));
        const auto key = clamp7(m.key);
        int vel = m.velocity;
        if (options.metronomeAccent && !downbeat) {
            vel = vel * 3 / 4;
        }
        sendClickOffs(time);
        sink->send(message3(time, m.port, status, key, clamp7(vel)));
        clickOffs.emplace(time + durationSeconds, message3(time + durationSeconds, m.port, status, key, 0));
    }

    /// Sends the metronome note-offs due up to `upTo`, so that output stays in time order.
    void sendClickOffs(const double upTo) {
        while (!clickOffs.empty() && clickOffs.begin()->first <= upTo) {
            sink->send(clickOffs.begin()->second);
            clickOffs.erase(clickOffs.begin());
        }
    }

    void clickAt(const Tick tick, const bool downbeat) {
        const double time = tickToClock(tick);
        click(time, tickToClock(tick + sequence->metronome.duration) - time, downbeat);
    }

    void emit(const Item& it, const double time) {
        switch (it.kind) {
        case ItemKind::NoteOn:
            ++sounding[{it.port, it.data[0], it.data[1]}];
            sink->send(toMessage(it, time));
            break;
        case ItemKind::NoteOff: {
            // Only for a note that was started (not for notes of a stream muted at their start).
            const auto found = sounding.find({it.port, it.data[0], it.data[1]});
            if (found == sounding.end()) {
                break;
            }
            if (--found->second == 0) {
                sounding.erase(found);
            }
            sink->send(toMessage(it, time));
            break;
        }
        case ItemKind::Control:
            sink->send(toMessage(it, time));
            break;
        case ItemKind::Sysex:
            if (options.sendSysexBanks && it.bank >= 0 &&
                static_cast<std::size_t>(it.bank) < sequence->sysxBanks.size()) {
                sink->sendSysex(time, it.port, sequence->sysxBanks[static_cast<std::size_t>(it.bank)].data);
            }
            break;
        case ItemKind::String:
            if (stringHandler) {
                stringHandler(it.stream, *it.event, time);
            }
            break;
        }
    }

    /// Sends note-offs for every note still sounding, at `time`.
    void silence(const double time) {
        for (auto& [due, m] : clickOffs) {
            m.time = std::min(due, time);
            sink->send(m);
        }
        clickOffs.clear();
        for (const auto& [key, count] : sounding) {
            MidiMessage m;
            m.time = time;
            m.port = std::get<0>(key);
            m.size = 3;
            m.data = {std::get<1>(key), std::get<2>(key), 0};
            sink->send(m);
        }
        sounding.clear();
    }

    void start(const Tick tick, const double at, const bool withCountIn) {
        anchorTick = tick;
        anchorClock = at;
        countInClicks.clear();
        if (withCountIn && countInEnabled()) {
            // Count-in: whole measures in the meter and tempo at the start position, before `at`.
            const MeterEntry& m = sequence->meterMap.atMeasure(sequence->meterMap.measureAt(tick));
            const Tick bt = sequence->meterMap.beatTicks(m);
            const double beatSeconds = (sequenceSeconds(tick + bt) - sequenceSeconds(tick)) / options.tempoRatio;
            const double durationSeconds = beatSeconds * static_cast<double>(sequence->metronome.duration) /
                                           static_cast<double>(std::max<Tick>(1, bt));
            const int beats = options.countInMeasures * m.numerator;
            anchorClock = at + beats * beatSeconds;
            for (int i = 0; i < beats; ++i) {
                countInClicks.emplace_back(at + i * beatSeconds, i % m.numerator == 0);
            }
            countInDuration = durationSeconds;
        }
        next = static_cast<std::size_t>(std::ranges::lower_bound(playlist, tick, {}, &Item::tick) - playlist.begin());
        clickTick = beatAtOrAfter(sequence->meterMap, tick).first;
        scheduledThrough = at;
        reachedEnd = false;
        for (const Item& it : startupItems(*sequence, playlist, tick, options)) {
            if (audible(it.stream)) {
                emit(it, at);
            }
        }
    }
    double countInDuration = 0; // note length of a count-in click, in seconds

    /// Schedules the metronome up to `horizon`, but not beyond `limit` (ticks).
    void scheduleClicks(const double horizon, const Tick limit) {
        if (!clickEnabled()) {
            return;
        }
        while (clickTick < limit && tickToClock(clickTick) <= horizon) {
            const auto [tick, downbeat] = beatAtOrAfter(sequence->meterMap, clickTick);
            clickAt(tick, downbeat);
            clickTick = beatAtOrAfter(sequence->meterMap, tick + 1).first;
        }
    }

    /// Schedules items up to `horizon` (clock time). Handles the loop range and the end.
    void schedule(const double horizon) {
        while (!countInClicks.empty() && countInClicks.front().first <= horizon) {
            click(countInClicks.front().first, countInDuration, countInClicks.front().second);
            countInClicks.erase(countInClicks.begin());
        }
        const bool looping = options.loopFrom && options.loopThru && *options.loopThru > *options.loopFrom;
        while (!reachedEnd) {
            // Loop: when the loop end falls before the horizon, wrap there.
            if (looping) {
                const double wrapAt = tickToClock(*options.loopThru);
                if (wrapAt <= horizon && (next >= playlist.size() || playlist[next].tick >= *options.loopThru)) {
                    scheduleClicks(horizon, *options.loopThru);
                    silence(wrapAt);
                    anchorTick = *options.loopFrom;
                    anchorClock = wrapAt;
                    next = static_cast<std::size_t>(std::ranges::lower_bound(playlist, anchorTick, {}, &Item::tick) -
                                                    playlist.begin());
                    clickTick = beatAtOrAfter(sequence->meterMap, anchorTick).first;
                    continue;
                }
            }
            if (next >= playlist.size()) {
                scheduleClicks(horizon, looping ? *options.loopThru : lastTick());
                endClock = tickToClock(lastTick());
                reachedEnd = true;
                break;
            }
            const Item& it = playlist[next];
            if (looping && it.tick >= *options.loopThru) {
                // past the loop end but the wrap time is not within the horizon yet
                scheduleClicks(horizon, *options.loopThru);
                break;
            }
            const double time = tickToClock(it.tick);
            if (time > horizon) {
                scheduleClicks(horizon, looping ? *options.loopThru : lastTick());
                break;
            }
            scheduleClicks(time, std::min(it.tick + 1, lastTick())); // clicks up to this item keep time order
            sendClickOffs(time);
            // Note-offs go through emit() regardless of mute: it sends them only for sounding notes.
            if (it.kind == ItemKind::NoteOff || audible(it.stream)) {
                emit(it, time);
            }
            ++next;
        }
        sendClickOffs(horizon);
        scheduledThrough = horizon;
    }

    /// Controller reset on every channel of every port in use.
    void zeroAll(const double time, const bool allNotesOff) const {
        std::vector<MidiMessage> out;
        for (const int port : portsInUse(*sequence)) {
            for (std::uint8_t ch = 0; ch < 16; ++ch) {
                if (allNotesOff) {
                    out.push_back(message3(time, port, static_cast<std::uint8_t>(0xb0 | ch), 123, 0));
                }
                zeroControllers(out, time, port, ch);
            }
        }
        for (const MidiMessage& m : out) {
            sink->send(m);
        }
    }
};

Transport::Transport(const Sequence& sequence, Clock& clock, MidiSink& sink, PlaybackOptions options)
    : impl_(std::make_unique<Impl>(sequence, clock, sink, options)) {}

Transport::~Transport() = default;

const PlaybackOptions& Transport::options() const noexcept {
    return impl_->options;
}

void Transport::setOptions(const PlaybackOptions& options) {
    impl_->options = options;
}

void Transport::setTempoRatio(const double ratio) {
    if (impl_->playing) {
        // Re-anchor at the current position so the change applies from now on.
        const double t = impl_->clock->now();
        const Tick tick = impl_->clockToTick(t);
        impl_->anchorTick = tick;
        impl_->anchorClock = t;
    }
    impl_->options.tempoRatio = ratio;
}

void Transport::setStringEventHandler(StringEventHandler handler) {
    impl_->stringHandler = std::move(handler);
}

void Transport::play() {
    if (impl_->playing) {
        return;
    }
    impl_->playing = true;
    impl_->start(impl_->position, impl_->clock->now(), true);
    pump();
}

void Transport::stop() {
    if (!impl_->playing) {
        return;
    }
    const double t = impl_->clock->now();
    impl_->position = impl_->clockToTick(t);
    if (impl_->options.stopAtEnd) {
        impl_->position = std::min(impl_->position, impl_->lastTick());
    }
    impl_->playing = false;
    impl_->countInClicks.clear();
    impl_->silence(t);
    if (impl_->options.zeroControllersOnStop) {
        impl_->zeroAll(t, false);
    }
    impl_->sink->flush();
}

void Transport::panic() {
    stop();
    impl_->zeroAll(impl_->clock->now(), true);
    impl_->sink->flush();
}

void Transport::rewind() {
    stop();
    impl_->position = 0;
}

void Transport::seek(const Tick tick) {
    if (!impl_->playing) {
        impl_->position = std::max<Tick>(0, tick);
        return;
    }
    const double t = impl_->clock->now();
    impl_->silence(t);
    impl_->start(std::max<Tick>(0, tick), t, false);
    pump();
}

bool Transport::playing() const noexcept {
    return impl_->playing;
}

bool Transport::countingIn() const {
    return impl_->playing && impl_->clock->now() < impl_->anchorClock;
}

std::optional<Tick> Transport::tickAt(const double time) const {
    if (!impl_->playing || time < impl_->anchorClock) {
        return std::nullopt;
    }
    return impl_->clockToTick(time);
}

Tick Transport::now() const {
    if (!impl_->playing) {
        return impl_->position;
    }
    const Tick tick = impl_->clockToTick(impl_->clock->now());
    return impl_->options.stopAtEnd ? std::min(tick, impl_->lastTick()) : tick;
}

Tick Transport::endTick() const noexcept {
    return impl_->lastTick();
}

void Transport::setMuted(const int streamNumber, const bool muted) {
    if (muted) {
        impl_->muted.insert(streamNumber);
    } else {
        impl_->muted.erase(streamNumber);
    }
}

void Transport::setSolo(const int streamNumber, const bool solo) {
    if (solo) {
        impl_->solo.insert(streamNumber);
    } else {
        impl_->solo.erase(streamNumber);
    }
}

bool Transport::isAudible(const int streamNumber) const {
    return impl_->audible(streamNumber);
}

bool Transport::pump() {
    if (!impl_->playing) {
        return false;
    }
    const double now = impl_->clock->now();
    impl_->schedule(now + impl_->options.lookAhead);
    impl_->sink->flush();
    if (impl_->options.stopAtEnd && impl_->reachedEnd && now >= impl_->endClock) {
        impl_->playing = false;
        impl_->position = impl_->lastTick();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- render

namespace {

class FixedClock final : public Clock {
public:
    double time = 0;
    [[nodiscard]] double now() const override { return time; }
};

class CollectingSink final : public MidiSink {
public:
    std::vector<MidiMessage> messages;
    void send(const MidiMessage& m) override { messages.push_back(m); }
    void sendSysex(double /*time*/, int /*port*/, const std::vector<std::uint8_t>& /*bytes*/) override {}
};

} // namespace

std::vector<MidiMessage> render(const Sequence& sequence, const PlaybackOptions& options, const Tick from,
                                const std::optional<Tick> thru) {
    FixedClock clock;
    CollectingSink sink;
    PlaybackOptions o = options;
    o.lookAhead = 0;
    o.countInMeasures = 0;
    Transport t(sequence, clock, sink, o);
    t.seek(from);
    t.play();
    const Tick end = thru.value_or(t.endTick());
    // Advance the clock to the end and schedule everything in one go.
    const double endTime =
        (sequence.tempoMap.tickToSeconds(end) - sequence.tempoMap.tickToSeconds(from)) / o.tempoRatio;
    clock.time = endTime;
    t.pump();
    if (t.playing()) {
        t.stop();
    }
    std::erase_if(sink.messages, [&](const MidiMessage& m) { return m.time > endTime + 1e-9; });
    return sink.messages;
}

} // namespace sseq::engine
