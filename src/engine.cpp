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

PlaybackOptions PlaybackOptions::fromSong(const Song& song) {
    PlaybackOptions o;
    o.chase = song.vars.patchSearchBack();
    o.zeroControllersOnStop = song.vars.zeroControllers();
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
    int track = 0; // track number
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

Item shortItem(const Tick tick, const ItemKind kind, const Track& t, const std::uint8_t status, const std::uint8_t d1,
               const int d2 = -1) {
    Item it;
    it.tick = tick;
    it.kind = kind;
    it.track = t.number;
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

/// Period of a looped track: its end rounded up to a whole measure.
Tick loopPeriod(const Song& song, const Track& t) {
    const Tick end = t.endTime();
    return end <= 0 ? 0 : song.meterMap.nextMeasure(end - 1);
}

/// Builds the sorted list of outputs of all tracks (mute and solo are applied at scheduling time).
std::vector<Item> buildPlaylist(const Song& song) {
    std::vector<Item> items;
    for (const Track& t : song.tracks) {
        const int reps = std::max(1, t.reps.value_or(1));
        const Tick period = reps > 1 ? loopPeriod(song, t) : 0;
        const Tick offset = t.timePlus.value_or(0);
        for (int rep = 0; rep < reps; ++rep) {
            const Tick shift = offset + rep * period;
            for (const Event& e : t.events) {
                const Tick tick = e.time + shift;
                if (tick < 0) {
                    continue; // Time- pushed it before the start
                }
                const std::uint8_t ch = t.channel.value_or(e.channel) & 0x0fU;
                switch (e.kind) {
                case EventKind::Note: {
                    const std::uint8_t key = clamp7(e.data1 + t.keyPlus);
                    const std::uint8_t vel = std::max<std::uint8_t>(1, clamp7(e.data2 + t.velPlus));
                    items.push_back(shortItem(tick, ItemKind::NoteOn, t, 0x90 | ch, key, vel));
                    items.push_back(
                        shortItem(tick + std::max<Tick>(1, e.duration), ItemKind::NoteOff, t, 0x90 | ch, key, 0));
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
                    it.track = t.number;
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
                    it.track = t.number;
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

/// Messages that establish the state at `start`: the tracks' parameters, then (with chase) the
/// last patch, controller values and wheel position before `start` on every port and channel.
std::vector<Item> startupItems(const Song& song, const std::vector<Item>& playlist, const Tick start,
                               const PlaybackOptions& o) {
    std::vector<Item> out;
    if (o.sendTrackParameters) {
        for (const Track& t : song.tracks) {
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
            if (kind == 0xc0 || kind == 0xb0 || kind == 0xe0) {
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

/// Messages that silence a channel's continuous controllers.
void zeroControllers(std::vector<MidiMessage>& out, const double time, const int port, const std::uint8_t ch) {
    const auto msg = [&](std::initializer_list<std::uint8_t> bytes) {
        MidiMessage m;
        m.time = time;
        m.port = port;
        m.size = static_cast<std::uint8_t>(bytes.size());
        std::ranges::copy(bytes, m.data.begin());
        out.push_back(m);
    };
    msg({static_cast<std::uint8_t>(0xb0 | ch), 1, 0});  // modulation
    msg({static_cast<std::uint8_t>(0xb0 | ch), 64, 0}); // sustain
    msg({static_cast<std::uint8_t>(0xe0 | ch), 0, 64}); // pitch wheel centre
    msg({static_cast<std::uint8_t>(0xd0 | ch), 0});     // channel pressure
}

} // namespace

// ---------------------------------------------------------------- transport

struct Transport::Impl {
    const Song* song;
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
    std::set<std::pair<int, std::uint8_t>> channelsUsed;                 // (port, channel) for zeroing on stop

    Impl(const Song& s, Clock& c, MidiSink& k, const PlaybackOptions& o)
        : song(&s), clock(&c), sink(&k), options(o), playlist(buildPlaylist(s)) {
        for (const Track& t : s.tracks) {
            if (t.muted) {
                muted.insert(t.number);
            }
        }
    }

    [[nodiscard]] double songSeconds(const Tick tick) const { return song->tempoMap.tickToSeconds(tick); }
    [[nodiscard]] double tickToClock(const Tick tick) const {
        return anchorClock + (songSeconds(tick) - songSeconds(anchorTick)) / options.tempoRatio;
    }
    [[nodiscard]] Tick clockToTick(const double time) const {
        return song->tempoMap.secondsToTick(songSeconds(anchorTick) + (time - anchorClock) * options.tempoRatio);
    }
    [[nodiscard]] bool audible(const int track) const {
        if (!solo.empty()) {
            return solo.contains(track);
        }
        return !muted.contains(track);
    }
    [[nodiscard]] Tick lastTick() const { return playlist.empty() ? 0 : playlist.back().tick; }

    void emit(const Item& it, const double time) {
        switch (it.kind) {
        case ItemKind::NoteOn:
            ++sounding[{it.port, it.data[0], it.data[1]}];
            sink->send(toMessage(it, time));
            channelsUsed.insert({it.port, it.data[0] & 0x0fU});
            break;
        case ItemKind::NoteOff: {
            // Only for a note that was started (not for notes of a track muted at their start).
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
            channelsUsed.insert({it.port, it.data[0] & 0x0fU});
            break;
        case ItemKind::Sysex:
            if (options.sendSysexBanks && it.bank >= 0 && static_cast<std::size_t>(it.bank) < song->sysxBanks.size()) {
                sink->sendSysex(time, it.port, song->sysxBanks[static_cast<std::size_t>(it.bank)].data);
            }
            break;
        case ItemKind::String:
            if (stringHandler) {
                stringHandler(it.track, *it.event, time);
            }
            break;
        }
    }

    /// Sends note-offs for every note still sounding, at `time`.
    void silence(const double time) {
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

    void start(const Tick tick, const double at) {
        anchorTick = tick;
        anchorClock = at;
        next = static_cast<std::size_t>(std::ranges::lower_bound(playlist, tick, {}, &Item::tick) - playlist.begin());
        scheduledThrough = at;
        reachedEnd = false;
        for (const Item& it : startupItems(*song, playlist, tick, options)) {
            if (audible(it.track)) {
                emit(it, at);
            }
        }
    }

    /// Schedules items up to `horizon` (clock time). Handles the loop range and the end.
    void schedule(const double horizon) {
        while (!reachedEnd) {
            // Loop: when the loop end falls before the horizon, wrap there.
            if (options.loopFrom && options.loopThru && *options.loopThru > *options.loopFrom) {
                const double wrapAt = tickToClock(*options.loopThru);
                if (wrapAt <= horizon && (next >= playlist.size() || playlist[next].tick >= *options.loopThru)) {
                    silence(wrapAt);
                    anchorTick = *options.loopFrom;
                    anchorClock = wrapAt;
                    next = static_cast<std::size_t>(std::ranges::lower_bound(playlist, anchorTick, {}, &Item::tick) -
                                                    playlist.begin());
                    continue;
                }
            }
            if (next >= playlist.size()) {
                endClock = tickToClock(lastTick());
                reachedEnd = true;
                break;
            }
            const Item& it = playlist[next];
            if (options.loopThru && it.tick >= *options.loopThru) {
                // past the loop end but the wrap time is not within the horizon yet
                break;
            }
            const double time = tickToClock(it.tick);
            if (time > horizon) {
                break;
            }
            // Note-offs go through emit() regardless of mute: it sends them only for sounding notes.
            if (it.kind == ItemKind::NoteOff || audible(it.track)) {
                emit(it, time);
            }
            ++next;
        }
        scheduledThrough = horizon;
    }
};

Transport::Transport(const Song& song, Clock& clock, MidiSink& sink, PlaybackOptions options)
    : impl_(std::make_unique<Impl>(song, clock, sink, options)) {}

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
    impl_->start(impl_->position, impl_->clock->now());
    pump();
}

void Transport::stop() {
    if (!impl_->playing) {
        return;
    }
    const double t = impl_->clock->now();
    impl_->position = std::min(impl_->clockToTick(t), impl_->lastTick());
    impl_->playing = false;
    impl_->silence(t);
    if (impl_->options.zeroControllersOnStop) {
        std::vector<MidiMessage> out;
        for (const auto& [port, ch] : impl_->channelsUsed) {
            zeroControllers(out, t, port, ch);
        }
        for (const MidiMessage& m : out) {
            impl_->sink->send(m);
        }
    }
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
    impl_->start(std::max<Tick>(0, tick), t);
    pump();
}

bool Transport::playing() const noexcept {
    return impl_->playing;
}

Tick Transport::now() const {
    if (!impl_->playing) {
        return impl_->position;
    }
    return std::min(impl_->clockToTick(impl_->clock->now()), impl_->lastTick());
}

Tick Transport::endTick() const noexcept {
    return impl_->lastTick();
}

void Transport::setMuted(const int trackNumber, const bool muted) {
    if (muted) {
        impl_->muted.insert(trackNumber);
    } else {
        impl_->muted.erase(trackNumber);
    }
}

void Transport::setSolo(const int trackNumber, const bool solo) {
    if (solo) {
        impl_->solo.insert(trackNumber);
    } else {
        impl_->solo.erase(trackNumber);
    }
}

bool Transport::isAudible(const int trackNumber) const {
    return impl_->audible(trackNumber);
}

bool Transport::pump() {
    if (!impl_->playing) {
        return false;
    }
    const double now = impl_->clock->now();
    impl_->schedule(now + impl_->options.lookAhead);
    impl_->sink->flush();
    if (impl_->reachedEnd && now >= impl_->endClock) {
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

std::vector<MidiMessage> render(const Song& song, const PlaybackOptions& options, const Tick from,
                                const std::optional<Tick> thru) {
    FixedClock clock;
    CollectingSink sink;
    PlaybackOptions o = options;
    o.lookAhead = 0;
    Transport t(song, clock, sink, o);
    t.seek(from);
    t.play();
    const Tick end = thru.value_or(t.endTick());
    // Advance the clock to the end and schedule everything in one go.
    const double endTime = (song.tempoMap.tickToSeconds(end) - song.tempoMap.tickToSeconds(from)) / o.tempoRatio;
    clock.time = endTime;
    t.pump();
    if (t.playing()) {
        t.stop();
    }
    std::erase_if(sink.messages, [&](const MidiMessage& m) { return m.time > endTime + 1e-9; });
    return sink.messages;
}

} // namespace sseq::engine
