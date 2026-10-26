// libsseq: playback engine implementation.
//
// The output order and the note-off, stop and start behaviour follow captures of the original
// engine's MIDI output (see tests/data/engine/README.md); engine.hpp summarises them.
#include "sseq/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
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
    o.midiClock = sequence.vars.transmitMidiSync();
    o.startNeverContinue = !sequence.vars.useContinue();
    return o;
}

// ---------------------------------------------------------------- playlist

namespace {

constexpr Tick kNever = std::numeric_limits<Tick>::max();

constexpr std::uint8_t clamp7(const int v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 127));
}

/// Key+ that leaves 0..127 folds the key back by octaves (observed: 120 + 12 plays 120, 5 - 12 plays 5).
constexpr std::uint8_t foldKey(int key) noexcept {
    while (key > 127) {
        key -= 12;
    }
    while (key < 0) {
        key += 12;
    }
    return static_cast<std::uint8_t>(key);
}

enum class ItemKind : std::uint8_t { NoteOn, Control, Sysex, String };

/// One output of the playlist: a note (its note-off goes to the pending heap when it plays), another
/// short message, a sysex bank, or a string event.
struct Item {
    Tick tick = 0;
    ItemKind kind = ItemKind::NoteOn;
    int stream = 0; // stream number
    int rank = 0;   // position of the stream in stream-number order
    int index = 0;  // position of the event in its stream (counting repetitions)
    int port = 0;
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> data{};
    Tick offTick = 0;             // NoteOn
    int bank = -1;                // Sysex
    const Event* event = nullptr; // String
};

MidiMessage message(const double time, const int port, const std::uint8_t size, const std::array<std::uint8_t, 3>& d) {
    MidiMessage m;
    m.time = time;
    m.port = port;
    m.size = size;
    m.data = d;
    return m;
}

MidiMessage message3(const double time, const int port, const std::uint8_t status, const std::uint8_t d1,
                     const std::uint8_t d2) {
    return message(time, port, 3, {status, d1, d2});
}

MidiMessage message1(const double time, const int port, const std::uint8_t status) {
    return message(time, port, 1, {status, 0, 0});
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

/// The streams in stream-number order.
std::vector<const Stream*> streamsByNumber(const Sequence& sequence) {
    std::vector<const Stream*> out;
    out.reserve(sequence.streams.size());
    for (const Stream& t : sequence.streams) {
        out.push_back(&t);
    }
    std::ranges::stable_sort(out, {}, &Stream::number);
    return out;
}

/// The output channel of a stream's parameters: the forced channel, else the first MIDI event's.
std::uint8_t parameterChannel(const Stream& t) {
    if (t.channel) {
        return *t.channel & 0x0fU;
    }
    if (const auto first = std::ranges::find_if(t.events, &Event::isMidi); first != t.events.end()) {
        return first->channel & 0x0fU;
    }
    return 0;
}

/// Patch, volume and pan of a stream (those that are set), at time 0.
void appendStreamParameters(std::vector<MidiMessage>& out, const Stream& t, const double time) {
    const std::uint8_t ch = parameterChannel(t);
    if (t.patch) {
        out.push_back(message(time, t.port, 2, {static_cast<std::uint8_t>(0xc0 | ch), clamp7(*t.patch), 0}));
    }
    if (t.volume && *t.volume >= 0) {
        out.push_back(message3(time, t.port, static_cast<std::uint8_t>(0xb0 | ch), 7, clamp7(*t.volume)));
    }
    if (t.pan && *t.pan >= 0) {
        out.push_back(message3(time, t.port, static_cast<std::uint8_t>(0xb0 | ch), 10, clamp7(*t.pan)));
    }
}

struct Playlist {
    std::vector<Item> items;
    Tick end = 0; // the last tick anything happens (note-offs included)
};

/// Builds the sorted list of outputs of all streams (mute and solo are applied at scheduling time).
Playlist buildPlaylist(const Sequence& sequence) {
    Playlist p;
    const Tick sequenceEnd = nonLoopingEnd(sequence);
    const std::vector<const Stream*> ordered = streamsByNumber(sequence);
    for (std::size_t rank = 0; rank < ordered.size(); ++rank) {
        const Stream& t = *ordered[rank];
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
        int index = 0;
        for (int rep = 0; rep < reps; ++rep) {
            const Tick shift = offset + rep * period;
            for (const Event& e : t.events) {
                // Time- that moves an event before the start plays it at the start.
                const Tick tick = std::max<Tick>(0, e.time + shift);
                if (clip && tick >= *clip) {
                    continue;
                }
                const std::uint8_t ch = t.channel.value_or(e.channel) & 0x0fU;
                Item it;
                switch (e.kind) {
                case EventKind::Note: {
                    it = shortItem(tick, ItemKind::NoteOn, t, 0x90 | ch, foldKey(e.data1 + t.keyPlus),
                                   std::max<std::uint8_t>(1, clamp7(e.data2 + t.velPlus)));
                    it.offTick = tick + std::max<Tick>(1, e.duration);
                    if (clip) {
                        it.offTick = std::min(it.offTick, *clip);
                    }
                    break;
                }
                case EventKind::KeyAftertouch:
                    it =
                        shortItem(tick, ItemKind::Control, t, 0xa0 | ch, foldKey(e.data1 + t.keyPlus), clamp7(e.data2));
                    break;
                case EventKind::Controller:
                    it = shortItem(tick, ItemKind::Control, t, 0xb0 | ch, clamp7(e.data1), clamp7(e.data2));
                    break;
                case EventKind::Patch:
                    it = shortItem(tick, ItemKind::Control, t, 0xc0 | ch, clamp7(e.data1));
                    break;
                case EventKind::ChannelAftertouch:
                    it = shortItem(tick, ItemKind::Control, t, 0xd0 | ch, clamp7(e.data1));
                    break;
                case EventKind::PitchWheel: {
                    const int v = std::clamp(e.data1 + 8192, 0, 16383);
                    it = shortItem(tick, ItemKind::Control, t, 0xe0 | ch, static_cast<std::uint8_t>(v & 0x7f),
                                   (v >> 7) & 0x7f);
                    break;
                }
                case EventKind::Sysex:
                    it.tick = tick;
                    it.kind = ItemKind::Sysex;
                    it.stream = t.number;
                    it.port = t.port;
                    it.bank = e.data1;
                    break;
                case EventKind::MciCommand:
                case EventKind::Wave:
                case EventKind::Text:
                    it.tick = tick;
                    it.kind = ItemKind::String;
                    it.stream = t.number;
                    it.port = t.port;
                    it.event = &e;
                    break;
                }
                it.rank = static_cast<int>(rank);
                it.index = index++;
                p.end = std::max(p.end, it.kind == ItemKind::NoteOn ? it.offTick : it.tick);
                p.items.push_back(it);
            }
        }
    }
    // One tick: the streams in number order, each in stored order; sysex after the short messages.
    std::ranges::sort(p.items, [](const Item& a, const Item& b) {
        return std::tuple(a.tick, a.kind == ItemKind::Sysex, a.rank, a.index) <
               std::tuple(b.tick, b.kind == ItemKind::Sysex, b.rank, b.index);
    });
    return p;
}

/// The last patch, pitch wheel, modulation, sustain and volume of each audible stream before
/// `start`, for the streams that still have events at or after `start` ("search-back").
template <typename Audible>
std::vector<Item> chaseItems(const Sequence& sequence, const Playlist& playlist, const Tick start,
                             const Audible& audible) {
    std::vector<Item> out;
    for (const Stream* t : streamsByNumber(sequence)) {
        if (!audible(t->number)) {
            continue;
        }
        const bool continues = std::ranges::any_of(
            playlist.items, [&](const Item& it) { return it.stream == t->number && it.tick >= start; });
        if (!continues) {
            continue;
        }
        // patch, wheel, CC 1, CC 64, CC 7: the latest of each before `start`, per output channel
        std::map<std::tuple<int, std::uint8_t>, std::array<std::optional<Item>, 5>> latest;
        for (const Item& it : playlist.items) {
            if (it.tick >= start) {
                break;
            }
            if (it.stream != t->number || it.kind != ItemKind::Control) {
                continue;
            }
            const std::uint8_t kind = it.data[0] & 0xf0U;
            int slot = -1;
            if (kind == 0xc0) {
                slot = 0;
            } else if (kind == 0xe0) {
                slot = 1;
            } else if (kind == 0xb0 && it.data[1] == 1) {
                slot = 2;
            } else if (kind == 0xb0 && it.data[1] == 64) {
                slot = 3;
            } else if (kind == 0xb0 && it.data[1] == 7) {
                slot = 4;
            }
            if (slot >= 0) {
                latest[{it.port, static_cast<std::uint8_t>(it.data[0] & 0x0fU)}][static_cast<std::size_t>(slot)] = it;
            }
        }
        for (auto& [key, slots] : latest) {
            for (const std::optional<Item>& it : slots) {
                if (it) {
                    out.push_back(*it);
                }
            }
        }
    }
    return out;
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

/// A pending note-off.
struct PendingOff {
    Tick tick = 0;
    int port = 0;
    std::uint8_t status = 0;
    std::uint8_t key = 0;
};

/// The original engine's note-off queue: a binary min-heap on the time. A new entry moves up past
/// parents with an equal or later time; after removing the top, the last entry moves down only past
/// children with an earlier time (the left child on a tie). Entries with equal times therefore leave
/// in the order this produces, which the captures confirm.
class OffHeap {
public:
    [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
    [[nodiscard]] const PendingOff& top() const { return heap_.front(); }
    void clear() noexcept { heap_.clear(); }

    void push(const PendingOff& off) {
        heap_.push_back(off);
        std::size_t i = heap_.size() - 1;
        while (i > 0) {
            const std::size_t parent = (i - 1) / 2;
            if (heap_[parent].tick < heap_[i].tick) {
                break;
            }
            std::swap(heap_[parent], heap_[i]);
            i = parent;
        }
    }

    PendingOff pop() {
        const PendingOff out = heap_.front();
        heap_.front() = heap_.back();
        heap_.pop_back();
        std::size_t i = 0;
        for (;;) {
            const std::size_t left = 2 * i + 1;
            if (left >= heap_.size()) {
                break;
            }
            std::size_t child = left;
            if (left + 1 < heap_.size() && heap_[left + 1].tick < heap_[left].tick) {
                child = left + 1;
            }
            if (!(heap_[child].tick < heap_[i].tick)) {
                break;
            }
            std::swap(heap_[child], heap_[i]);
            i = child;
        }
        return out;
    }

private:
    std::vector<PendingOff> heap_;
};

using NoteKey = std::tuple<int, std::uint8_t, std::uint8_t>; // port, status (0x9n), key

} // namespace

// ---------------------------------------------------------------- transport

struct Transport::Impl {
    const Sequence* sequence;
    Clock* clock;
    MidiSink* sink;
    PlaybackOptions options;
    StringEventHandler stringHandler;

    Playlist playlist;
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
    OffHeap offs;                                              // note-offs not yet due
    std::set<NoteKey> sounding;                                // notes started and not yet ended
    Tick clickTick = 0;                                        // next metronome beat to schedule
    std::vector<std::pair<double, bool>> countInClicks;        // (clock time, downbeat), ascending
    std::multimap<double, MidiMessage> clickOffs;              // metronome note-offs not yet sent
    std::vector<std::tuple<double, NoteKey, bool>> handedOver; // note-ons (true) and -offs sent with a time
                                                               // that may still be ahead of the clock
    std::int64_t nextClock = 0;                                // index of the next MIDI clock (24 per quarter note)
    double idleClock = -1;                                     // next MIDI clock while stopped (< 0: not started)

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
    [[nodiscard]] Tick lastTick() const { return playlist.end; }
    [[nodiscard]] bool clickEnabled() const {
        return options.metronome && sequence->metronome.velocity > 0 && sequence->metronome.duration > 0;
    }
    [[nodiscard]] bool countInEnabled() const {
        return options.countInMeasures > 0 && sequence->metronome.velocity > 0 && sequence->metronome.duration > 0;
    }
    [[nodiscard]] Tick clockTicks(const std::int64_t index) const {
        return static_cast<Tick>(index * sequence->ppq / 24);
    }

    /// A metronome tick: note on at `time`, note off `durationSeconds` later. A click still sounding
    /// (its length reaches past this beat) ends first.
    void click(const double time, const double durationSeconds) {
        const Metronome& m = sequence->metronome;
        const auto status = static_cast<std::uint8_t>(0x90 | (m.channel & 0x0f));
        const auto key = clamp7(m.key);
        sendClickOffs(time);
        for (auto& [due, off] : clickOffs) {
            off.time = time;
            sink->send(off);
        }
        clickOffs.clear();
        sink->send(message3(time, m.port, status, key, clamp7(m.velocity)));
        clickOffs.emplace(time + durationSeconds, message3(time + durationSeconds, m.port, status, key, 0));
    }

    /// Sends the metronome note-offs due up to `upTo`, so that output stays in time order.
    void sendClickOffs(const double upTo) {
        while (!clickOffs.empty() && clickOffs.begin()->first <= upTo) {
            sink->send(clickOffs.begin()->second);
            clickOffs.erase(clickOffs.begin());
        }
    }

    /// The click's length in ticks. The metronome duration counts MIDI clocks (1/24 quarter note,
    /// the dialog allows 1 to 24), and the original ends the click two clocks before that count; a
    /// duration of 1 wraps round to 23 clocks.
    [[nodiscard]] Tick clickTicks() const {
        const int d = sequence->metronome.duration;
        const int clocks = d >= 2 ? d - 2 : 23;
        return static_cast<Tick>(clocks) * sequence->ppq / 24;
    }

    /// A click on the beat at `tick`. Its note-off comes after clickTicks(), but not later than
    /// one MIDI clock before the next measure; a click still sounding at the next beat ends there.
    void clickAt(const Tick tick) {
        const double time = tickToClock(tick);
        const Tick lastClock = sequence->meterMap.nextMeasure(tick) - sequence->ppq / 24;
        click(time, tickToClock(std::min(tick + clickTicks(), lastClock)) - time);
    }

    void noteOn(const Item& it, const double time) {
        const NoteKey key{it.port, it.data[0], it.data[1]};
        if (sounding.contains(key)) {
            // the key is sounding: end it first
            sink->send(message3(time, it.port, it.data[0], it.data[1], 0));
        }
        sounding.insert(key);
        sink->send(message(time, it.port, it.size, it.data));
        handedOver.emplace_back(time, key, true);
        offs.push({it.offTick, it.port, it.data[0], it.data[1]});
    }

    void noteOff(const PendingOff& off, const double time) {
        const NoteKey key{off.port, off.status, off.key};
        if (sounding.erase(key) > 0) {
            sink->send(message3(time, off.port, off.status, off.key, 0));
            handedOver.emplace_back(time, key, false);
        }
    }

    /// The output dropped what was still ahead of `time` (discardScheduled): wind the sounding
    /// notes back to that time, so a dropped note-on gets no note-off and a note whose note-off was
    /// dropped gets one.
    void rewindSounding(const double time) {
        for (const auto& [t, key, on] : std::views::reverse(handedOver)) {
            if (t <= time) {
                break;
            }
            if (on) {
                sounding.erase(key);
            } else {
                sounding.insert(key);
            }
        }
        handedOver.clear();
    }

    /// Forgets the notes handed over up to `time` (they are delivered by now).
    void pruneHandedOver(const double time) {
        std::erase_if(handedOver, [time](const auto& entry) { return std::get<0>(entry) <= time; });
    }

    void emit(const Item& it, const double time) {
        switch (it.kind) {
        case ItemKind::NoteOn:
            noteOn(it, time);
            break;
        case ItemKind::Control:
            sink->send(message(time, it.port, it.size, it.data));
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

    /// The notes sounding (sequence notes and metronome clicks) by port, then channel, then key.
    [[nodiscard]] std::map<int, std::array<std::set<std::uint8_t>, 16>> soundingByChannel() const {
        std::map<int, std::array<std::set<std::uint8_t>, 16>> out;
        for (const auto& [port, status, key] : sounding) {
            out[port][status & 0x0fU].insert(key);
        }
        for (const auto& [due, m] : clickOffs) {
            out[m.port][m.data[0] & 0x0fU].insert(m.data[1]);
        }
        return out;
    }

    /// Note-offs for every note still sounding, at `time` (seek and loop: no controller messages).
    void silence(const double time) {
        for (const auto& [port, channels] : soundingByChannel()) {
            for (std::uint8_t ch = 0; ch < 16; ++ch) {
                for (const std::uint8_t key : channels[ch]) {
                    sink->send(message3(time, port, static_cast<std::uint8_t>(0x90 | ch), key, 0));
                }
            }
        }
        clickOffs.clear();
        sounding.clear();
        offs.clear();
        handedOver.clear();
    }

    /// What stopping sends: MIDI Stop; the metronome click's note-off; per channel the sounding notes'
    /// offs and All Notes Off; then, when asked, the controller reset on every channel.
    void stopOutput(const double time, const bool zeroControllers) {
        std::set<int> ports = portsInUse(*sequence);
        if (options.midiClock) {
            for (const int port : ports) {
                sink->send(message1(time, port, 0xfc));
            }
        }
        // a metronome click still sounding ends before the channels are silenced
        for (auto& [due, m] : clickOffs) {
            m.time = time;
            sink->send(m);
        }
        clickOffs.clear();
        const auto byChannel = soundingByChannel();
        for (const auto& [port, channels] : byChannel) {
            ports.insert(port);
        }
        for (const int port : ports) {
            const auto found = byChannel.find(port);
            for (std::uint8_t ch = 0; ch < 16; ++ch) {
                if (found != byChannel.end()) {
                    for (const std::uint8_t key : found->second[ch]) {
                        sink->send(message3(time, port, static_cast<std::uint8_t>(0x90 | ch), key, 0));
                    }
                }
                sink->send(message3(time, port, static_cast<std::uint8_t>(0xb0 | ch), 123, 0));
            }
        }
        if (zeroControllers) {
            for (const int port : ports) {
                for (std::uint8_t ch = 0; ch < 16; ++ch) {
                    const auto cc = static_cast<std::uint8_t>(0xb0 | ch);
                    sink->send(message3(time, port, static_cast<std::uint8_t>(0xe0 | ch), 0, 64));
                    sink->send(message3(time, port, cc, 1, 0));
                    sink->send(message3(time, port, cc, 64, 0));
                    sink->send(message3(time, port, cc, 121, 0));
                }
            }
        }
        clickOffs.clear();
        sounding.clear();
        offs.clear();
        handedOver.clear();
    }

    void start(const Tick tick, const double at, const bool withCountIn, const bool transportStart) {
        anchorTick = tick;
        anchorClock = at;
        countInClicks.clear();
        if (withCountIn && countInEnabled()) {
            // Count-in: whole measures in the meter and tempo at the start position, before `at`.
            const MeterEntry& m = sequence->meterMap.atMeasure(sequence->meterMap.measureAt(tick));
            const Tick bt = sequence->meterMap.beatTicks(m);
            const double beatSeconds = (sequenceSeconds(tick + bt) - sequenceSeconds(tick)) / options.tempoRatio;
            const double durationSeconds =
                beatSeconds * static_cast<double>(clickTicks()) / static_cast<double>(std::max<Tick>(1, bt));
            const int beats = options.countInMeasures * m.numerator;
            anchorClock = at + beats * beatSeconds;
            for (int i = 0; i < beats; ++i) {
                countInClicks.emplace_back(at + i * beatSeconds, i % m.numerator == 0);
            }
            countInDuration = durationSeconds;
        }
        next = static_cast<std::size_t>(std::ranges::lower_bound(playlist.items, tick, {}, &Item::tick) -
                                        playlist.items.begin());
        clickTick = beatAtOrAfter(sequence->meterMap, tick).first;
        nextClock = (static_cast<std::int64_t>(tick) * 24 + sequence->ppq - 1) / sequence->ppq;
        scheduledThrough = at;
        reachedEnd = false;
        if (transportStart && options.midiClock) {
            const std::uint8_t status = tick == 0 || options.startNeverContinue ? 0xfa : 0xfb;
            for (const int port : portsInUse(*sequence)) {
                sink->send(message1(at, port, status));
            }
        }
        if (options.sendStreamParameters) {
            std::vector<MidiMessage> params;
            for (const Stream* t : streamsByNumber(*sequence)) {
                if (!t->events.empty() && audible(t->number)) {
                    appendStreamParameters(params, *t, at);
                }
            }
            for (const MidiMessage& m : params) {
                sink->send(m);
            }
        }
        if (options.chase) {
            for (const Item& it : chaseItems(*sequence, playlist, tick, [this](int s) { return audible(s); })) {
                emit(it, at);
            }
        }
    }
    double countInDuration = 0; // note length of a count-in click, in seconds

    /// Schedules the metronome up to `horizon`, but not beyond `limit` (ticks; the limit's beat plays).
    void scheduleClicks(const double horizon, const Tick limit) {
        if (!clickEnabled()) {
            return;
        }
        while (clickTick <= limit && tickToClock(clickTick) <= horizon) {
            const auto [tick, downbeat] = beatAtOrAfter(sequence->meterMap, clickTick);
            if (tick > limit) {
                clickTick = tick;
                break;
            }
            clickAt(tick);
            clickTick = beatAtOrAfter(sequence->meterMap, tick + 1).first;
        }
    }

    /// Schedules items up to `horizon` (clock time). Handles the loop range and the end.
    void schedule(const double horizon) {
        while (!countInClicks.empty() && countInClicks.front().first <= horizon) {
            click(countInClicks.front().first, countInDuration);
            countInClicks.erase(countInClicks.begin());
        }
        const bool looping = options.loopFrom && options.loopThru && *options.loopThru > *options.loopFrom;
        const Tick limit = looping ? *options.loopThru : lastTick();
        while (!reachedEnd) {
            const Tick itemTick = next < playlist.items.size() ? playlist.items[next].tick : kNever;
            const Tick offTick = offs.empty() ? kNever : offs.top().tick;
            const Tick content = std::min(itemTick, offTick);
            // Loop: when the next thing lies at or past the loop end, wrap there once that time is due.
            if (looping && content >= *options.loopThru) {
                const double wrapAt = tickToClock(*options.loopThru);
                if (wrapAt > horizon) {
                    scheduleClicks(horizon, *options.loopThru);
                    break;
                }
                scheduleClicks(horizon, *options.loopThru - 1);
                scheduleClocks(wrapAt, *options.loopThru - 1);
                silence(wrapAt);
                anchorTick = *options.loopFrom;
                anchorClock = wrapAt;
                next = static_cast<std::size_t>(std::ranges::lower_bound(playlist.items, anchorTick, {}, &Item::tick) -
                                                playlist.items.begin());
                clickTick = beatAtOrAfter(sequence->meterMap, anchorTick).first;
                nextClock = (static_cast<std::int64_t>(anchorTick) * 24 + sequence->ppq - 1) / sequence->ppq;
                continue;
            }
            if (content == kNever) {
                scheduleClicks(horizon, limit);
                scheduleClocks(horizon, limit);
                endClock = tickToClock(lastTick());
                reachedEnd = true;
                break;
            }
            const double time = tickToClock(content);
            if (time > horizon) {
                scheduleClicks(horizon, limit);
                scheduleClocks(horizon, limit);
                break;
            }
            scheduleClocks(time, content); // the clock of a tick comes first
            scheduleClicks(time, content); // clicks up to this tick keep time order
            sendClickOffs(time);
            if (offTick <= itemTick) {
                noteOff(offs.pop(), time); // due note-offs before the tick's events
                continue;
            }
            const Item& it = playlist.items[next];
            if (audible(it.stream)) {
                emit(it, time);
            }
            ++next;
        }
        sendClickOffs(horizon);
        scheduledThrough = horizon;
    }

    /// MIDI clocks up to `horizon`, but not beyond tick `limit`.
    void scheduleClocks(const double horizon, const Tick limit) {
        if (!options.midiClock) {
            return;
        }
        while (clockTicks(nextClock) <= limit && tickToClock(clockTicks(nextClock)) <= horizon) {
            const double time = tickToClock(clockTicks(nextClock));
            for (const int port : portsInUse(*sequence)) {
                sink->send(message1(time, port, 0xf8));
            }
            ++nextClock;
        }
    }

    /// MIDI clocks while stopped, at the tempo at the stop position.
    void idleClocks(const double horizon) {
        const double beat =
            (sequenceSeconds(position + sequence->ppq) - sequenceSeconds(position)) / options.tempoRatio;
        const double interval = beat / 24;
        if (interval <= 0) {
            return;
        }
        if (idleClock < 0) {
            idleClock = clock->now();
        }
        while (idleClock <= horizon) {
            for (const int port : portsInUse(*sequence)) {
                sink->send(message1(idleClock, port, 0xf8));
            }
            idleClock += interval;
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
    impl_->start(impl_->position, impl_->clock->now(), true, true);
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
    impl_->sink->discardScheduled(); // pending future note-ons would outlive the note-offs below
    impl_->rewindSounding(t);
    impl_->stopOutput(t, impl_->options.zeroControllersOnStop);
    impl_->idleClock = -1;
    impl_->sink->flush();
}

void Transport::panic() {
    if (impl_->playing) {
        const double t = impl_->clock->now();
        impl_->position = std::min(impl_->clockToTick(t), impl_->lastTick());
        impl_->playing = false;
        impl_->countInClicks.clear();
        impl_->sink->discardScheduled();
        impl_->rewindSounding(t);
        impl_->stopOutput(t, true);
    } else {
        impl_->stopOutput(impl_->clock->now(), true);
    }
    impl_->idleClock = -1;
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
    impl_->sink->discardScheduled();
    impl_->rewindSounding(t);
    impl_->silence(t);
    impl_->start(std::max<Tick>(0, tick), t, false, false);
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
    const double now = impl_->clock->now();
    if (!impl_->playing) {
        if (impl_->options.midiClock && impl_->options.clockWhileStopped) {
            impl_->idleClocks(now + impl_->options.lookAhead);
            impl_->sink->flush();
        }
        return false;
    }
    impl_->pruneHandedOver(now);
    impl_->schedule(now + impl_->options.lookAhead);
    if (impl_->options.stopAtEnd && impl_->reachedEnd && now >= impl_->endClock) {
        // The end: the same output as a stop, at the end time.
        impl_->playing = false;
        impl_->position = impl_->lastTick();
        impl_->stopOutput(impl_->endClock, impl_->options.zeroControllersOnStop);
        impl_->idleClock = -1;
        impl_->sink->flush();
        return false;
    }
    impl_->sink->flush();
    return true;
}

// ---------------------------------------------------------------- open, render

std::vector<MidiMessage> openMessages(const Sequence& sequence) {
    std::vector<MidiMessage> out;
    for (const Stream* t : streamsByNumber(sequence)) {
        if (!t->events.empty()) {
            appendStreamParameters(out, *t, 0);
        }
    }
    return out;
}

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
        std::erase_if(sink.messages, [&](const MidiMessage& m) { return m.time > endTime + 1e-9; });
        t.stop();
    }
    return sink.messages;
}

} // namespace sseq::engine
