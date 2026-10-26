// libsseq: Edit menu transformations. The rounding rules come from Cakewalk Apprentice 1.04
// oracles (tests/data/edit, tests/edit_tests.cpp).
#include "sseq/edit.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace sseq::edit {

EventFilter EventFilter::all() noexcept {
    EventFilter f;
    f.kinds.fill(true);
    return f;
}

EventFilter EventFilter::only(const EventKind kind) noexcept {
    EventFilter f;
    f.set(kind);
    return f;
}

EventFilter EventFilter::nothing() noexcept {
    EventFilter f;
    f.set(EventKind::Sysex).set(EventKind::MciCommand).set(EventKind::Wave).set(EventKind::Text);
    return f;
}

bool EventFilter::includes(const Event& e) const noexcept {
    if (!includes(e.kind)) {
        return false;
    }
    switch (e.kind) {
    case EventKind::Note:
        return noteKey.contains(e.data1) && noteVelocity.contains(e.data2) &&
               noteDuration.contains(static_cast<int>(std::min<Tick>(e.duration, 65535))) &&
               channel.contains(e.channel);
    case EventKind::KeyAftertouch:
        return keyAftertouchKey.contains(e.data1) && keyAftertouchValue.contains(e.data2) &&
               channel.contains(e.channel);
    case EventKind::Controller:
        return controllerNumber.contains(e.data1) && controllerValue.contains(e.data2) && channel.contains(e.channel);
    case EventKind::Patch:
        return patchNumber.contains(e.data1) && channel.contains(e.channel);
    case EventKind::ChannelAftertouch:
        return channelAftertouchAmount.contains(e.data1) && channel.contains(e.channel);
    case EventKind::PitchWheel:
        return pitchWheelValue.contains(e.data1) && channel.contains(e.channel);
    default:
        return true; // Sysex, MCI, Wave, Text: no ranges apply
    }
}

bool EventFilter::includes(const Event& e, const MeterMap& meter) const noexcept {
    if (!includes(e)) {
        return false;
    }
    if (!e.isMidi() || e.kind == EventKind::Sysex) {
        return true;
    }
    const MBT mbt = meter.tickToMBT(e.time);
    return beat.contains(mbt.beat) && tick.contains(mbt.tick);
}

Selection Selection::all(const Sequence& sequence) {
    Selection s;
    for (const Stream& t : sequence.streams) {
        if (!t.events.empty()) {
            s.streams.push_back(t.number);
        }
    }
    s.from = 0;
    s.thru = sequence.endTime();
    return s;
}

bool Selection::contains(const int streamNumber, const Tick time) const noexcept {
    return time >= from && time <= thru && std::ranges::find(streams, streamNumber) != streams.end();
}

Tick resolutionTicks(const Resolution resolution, const int ppq) noexcept {
    const Tick whole = static_cast<Tick>(ppq) * 4;
    switch (resolution) {
    case Resolution::Whole:
        return whole;
    case Resolution::Half:
        return whole / 2;
    case Resolution::Quarter:
        return whole / 4;
    case Resolution::Eighth:
        return whole / 8;
    case Resolution::EighthTriplet:
        return whole / 12;
    case Resolution::Sixteenth:
        return whole / 16;
    case Resolution::SixteenthTriplet:
        return whole / 24;
    case Resolution::ThirtySecond:
        return whole / 32;
    case Resolution::ThirtySecondTriplet:
        return whole / 48;
    }
    return whole / 16;
}

Tick quantizeTime(const Tick time, const Tick resolution, const int strength) noexcept {
    if (resolution <= 0 || time < 0) {
        return time;
    }
    // Nearest multiple, with exactly half (integer res / 2) rounding up: offsets 0..14 of a 30-tick
    // grid go down, 15..29 up; of a 15-tick grid, 0..7 down and 8..14 up.
    const Tick grid = (time + resolution / 2) / resolution * resolution;
    // Strength moves the event that percentage of the way, truncated toward zero (C division).
    return time + (grid - time) * strength / 100;
}

Tick quantizeDuration(const Tick duration, const Tick resolution) noexcept {
    if (resolution <= 0 || duration < 0) {
        return duration;
    }
    const Tick grid = std::max(resolution, (duration + resolution / 2) / resolution * resolution);
    return grid - 1;
}

int quantize(Sequence& sequence, const Selection& selection, const QuantizeOptions& options) {
    int changed = 0;
    for (Stream& t : sequence.streams) {
        if (std::ranges::find(selection.streams, t.number) == selection.streams.end()) {
            continue;
        }
        bool moved = false;
        for (Event& e : t.events) {
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e, sequence.meterMap)) {
                continue;
            }
            bool touched = false;
            if (options.startTimes) {
                const Tick time = quantizeTime(e.time, options.resolution, options.strength);
                if (time != e.time) {
                    e.time = time;
                    touched = true;
                    moved = true;
                }
            }
            if (options.durations && e.kind == EventKind::Note) {
                const Tick duration = quantizeDuration(e.duration, options.resolution);
                if (duration != e.duration) {
                    e.duration = duration;
                    touched = true;
                }
            }
            if (touched) {
                ++changed;
            }
        }
        if (moved) {
            sortEvents(t.events);
        }
    }
    return changed;
}

// ---------------------------------------------------------------- Length

Tick scaleTicks(const Tick ticks, const int percent) noexcept {
    if (ticks < 0) {
        return ticks;
    }
    return (ticks * percent + 50) / 100;
}

int length(Sequence& sequence, const Selection& selection, const LengthOptions& options) {
    int changed = 0;
    for (Stream& t : sequence.streams) {
        if (std::ranges::find(selection.streams, t.number) == selection.streams.end()) {
            continue;
        }
        bool moved = false;
        for (Event& e : t.events) {
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e, sequence.meterMap)) {
                continue;
            }
            bool touched = false;
            if (options.startTimes) {
                const Tick time = selection.from + scaleTicks(e.time - selection.from, options.percent);
                if (time != e.time) {
                    e.time = time;
                    touched = true;
                    moved = true;
                }
            }
            if (options.durations && e.kind == EventKind::Note) {
                const Tick duration = scaleTicks(e.duration, options.percent);
                if (duration != e.duration) {
                    e.duration = duration;
                    touched = true;
                }
            }
            if (touched) {
                ++changed;
            }
        }
        if (moved) {
            sortEvents(t.events);
        }
    }
    return changed;
}

// ---------------------------------------------------------------- Transpose

namespace {

int foldKey(int key) noexcept {
    while (key < 0) {
        key += 12;
    }
    while (key > 127) {
        key -= 12;
    }
    return key;
}

int floorDiv(const int a, const int b) noexcept {
    return (a >= 0 ? a : a - b + 1) / b;
}

} // namespace

int transposeKey(const int key, const int halfSteps) noexcept {
    return foldKey(key + halfSteps);
}

int transposeKeyDiatonic(const int key, const int steps, const int alterations) noexcept {
    // Degrees of the major scale in half-steps above the tonic, and the degree Cakewalk assigns
    // to each of the twelve half-steps (the five non-scale tones go to 1, 2, 3, 4 and 6).
    static constexpr std::array<int, 7> kScale{0, 2, 4, 5, 7, 9, 11};
    static constexpr std::array<int, 12> kDegree{0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 6, 6};
    const int tonic = ((alterations * 7) % 12 + 12) % 12;
    const int relative = ((key - tonic) % 12 + 12) % 12;
    const int base = key - relative; // the tonic's pitch at or below `key`
    const int degree = kDegree[static_cast<std::size_t>(relative)] + steps;
    const int octave = floorDiv(degree, 7);
    const int inScale = degree - octave * 7;
    return foldKey(base + octave * 12 + kScale[static_cast<std::size_t>(inScale)]);
}

int transpose(Sequence& sequence, const Selection& selection, const TransposeOptions& options) {
    int changed = 0;
    for (Stream& t : sequence.streams) {
        if (std::ranges::find(selection.streams, t.number) == selection.streams.end()) {
            continue;
        }
        for (Event& e : t.events) {
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e, sequence.meterMap) ||
                (e.kind != EventKind::Note && e.kind != EventKind::KeyAftertouch)) {
                continue;
            }
            int key = e.data1;
            if (options.diatonic) {
                const int measure = sequence.meterMap.measureAt(e.time);
                int alterations = 0;
                for (const KeyEntry& k : sequence.keyMap) {
                    if (k.measure <= measure) {
                        alterations = k.alterations;
                    }
                }
                key = transposeKeyDiatonic(key, options.amount, alterations);
            } else {
                key = transposeKey(key, options.amount);
            }
            if (key != e.data1) {
                e.data1 = key;
                ++changed;
            }
        }
    }
    return changed;
}

// ---------------------------------------------------------------- Slide

Tick measuresToTicks(const Sequence& sequence, const Tick from, const int measures) noexcept {
    const MeterEntry& m = sequence.meterMap.atMeasure(sequence.meterMap.measureAt(from));
    return sequence.meterMap.measureTicks(m) * measures;
}

int slide(Sequence& sequence, const Selection& selection, const SlideOptions& options) {
    const Tick amount = std::max(options.amount, -selection.from);
    if (amount == 0) {
        return 0;
    }
    int changed = 0;
    for (Stream& t : sequence.streams) {
        if (std::ranges::find(selection.streams, t.number) == selection.streams.end()) {
            continue;
        }
        bool moved = false;
        for (Event& e : t.events) {
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e, sequence.meterMap)) {
                continue;
            }
            e.time += amount;
            moved = true;
            ++changed;
        }
        if (moved) {
            sortEvents(t.events);
        }
    }
    return changed;
}

// ---------------------------------------------------------------- Controller fill

int controllerFill(Stream& stream, const ControllerFillOptions& options) {
    const EventKind kind = options.controller ? EventKind::Controller : EventKind::PitchWheel;
    const int number = options.controller.value_or(0);
    const Tick from = std::min(options.from, options.thru);
    const Tick thru = std::max(options.from, options.thru);
    std::erase_if(stream.events, [&](const Event& e) {
        return e.kind == kind && e.channel == options.channel && e.time >= from && e.time <= thru &&
               (kind != EventKind::Controller || e.data1 == number);
    });
    const int span = std::abs(options.end - options.begin);
    const int step = options.end >= options.begin ? 1 : -1;
    std::vector<Event> fill;
    fill.reserve(static_cast<std::size_t>(span) + 1);
    Tick taken = -1;
    for (int i = 0; i <= span; ++i) {
        const int value = options.begin + i * step;
        const Tick time = from + (span == 0 ? 0 : Tick{i} * (thru - from) / span);
        if (time == taken) {
            continue;
        }
        taken = time;
        fill.push_back(kind == EventKind::Controller ? Event::controller(time, options.channel, number, value)
                                                     : Event::pitchWheel(time, options.channel, value));
    }
    // Merge: new events go before existing ones on the same tick.
    std::vector<Event> merged;
    merged.reserve(stream.events.size() + fill.size());
    auto existing = stream.events.begin();
    for (Event& e : fill) {
        while (existing != stream.events.end() && existing->time < e.time) {
            merged.push_back(std::move(*existing));
            ++existing;
        }
        merged.push_back(std::move(e));
    }
    merged.insert(merged.end(), std::make_move_iterator(existing), std::make_move_iterator(stream.events.end()));
    stream.events = std::move(merged);
    return static_cast<int>(fill.size());
}

// ---------------------------------------------------------------- Cut, Copy, Paste

namespace {

bool selected(const Selection& sel, const int streamNumber) {
    return std::ranges::find(sel.streams, streamNumber) != sel.streams.end();
}

/// The key signature in force at a measure.
int alterationsAt(const Sequence& s, const int measure) {
    int a = 0;
    for (const KeyEntry& k : s.keyMap) {
        if (k.measure <= measure) {
            a = k.alterations;
        }
    }
    return a;
}

/// Whether From..Thru covers whole measures: From starts one and Thru is the last tick of one.
bool wholeMeasures(const Sequence& s, const Tick from, const Tick thru) {
    const MeterMap& m = s.meterMap;
    return m.measureStart(m.measureAt(from)) == from && m.measureStart(m.measureAt(thru + 1)) == thru + 1;
}

void shiftEvents(Stream& t, const Tick fromTime, const Tick by) {
    bool moved = false;
    for (Event& e : t.events) {
        if (e.time >= fromTime) {
            e.time += by;
            moved = true;
        }
    }
    if (moved) {
        sortEvents(t.events);
    }
}

void shiftTempo(TempoMap& map, const Tick fromTime, const Tick by) {
    for (TempoEntry& e : map.entries) {
        if (e.time >= fromTime && e.time > 0) {
            e.time += by;
        }
    }
    std::ranges::stable_sort(map.entries, {}, &TempoEntry::time);
}

void shiftMeterKey(Sequence& s, const int fromMeasure, const int by) {
    for (MeterEntry& m : s.meterMap.entries) {
        if (m.measure >= fromMeasure && m.measure > 1) {
            m.measure += by;
        }
    }
    for (KeyEntry& k : s.keyMap) {
        if (k.measure >= fromMeasure && k.measure > 1) {
            k.measure += by;
        }
    }
    std::ranges::stable_sort(s.meterMap.entries, {}, &MeterEntry::measure);
    std::ranges::stable_sort(s.keyMap, {}, &KeyEntry::measure);
}

/// Removes the meter and key entries of measures first..last (never measure 1).
void eraseMeterKey(Sequence& s, const int first, const int last) {
    std::erase_if(s.meterMap.entries,
                  [&](const MeterEntry& m) { return m.measure > 1 && m.measure >= first && m.measure <= last; });
    std::erase_if(s.keyMap,
                  [&](const KeyEntry& k) { return k.measure > 1 && k.measure >= first && k.measure <= last; });
}

/// Sets the meter and key of a measure (an entry each; the key entry only when it changes the key).
void setMeterKey(Sequence& s, const int measure, const ClipMeterKey& mk) {
    auto& entries = s.meterMap.entries;
    const auto it = std::ranges::find(entries, measure, &MeterEntry::measure);
    if (it != entries.end()) {
        it->numerator = mk.numerator;
        it->denominatorLog2 = mk.denominatorLog2;
    } else {
        MeterEntry m;
        m.measure = measure;
        m.numerator = mk.numerator;
        m.denominatorLog2 = mk.denominatorLog2;
        entries.push_back(m);
        std::ranges::stable_sort(entries, {}, &MeterEntry::measure);
    }
    const auto kt = std::ranges::find(s.keyMap, measure, &KeyEntry::measure);
    if (kt != s.keyMap.end()) {
        kt->alterations = mk.alterations;
    } else {
        s.keyMap.push_back(KeyEntry{measure, mk.alterations});
        std::ranges::stable_sort(s.keyMap, {}, &KeyEntry::measure);
    }
}

} // namespace

Clipboard copy(const Sequence& sequence, const Selection& selection, const CopyOptions& options) {
    Clipboard clip;
    const Tick from = std::min(selection.from, selection.thru);
    const Tick thru = std::max(selection.from, selection.thru);
    clip.length = thru - from + 1;
    if (options.events) {
        for (const Stream& t : sequence.streams) {
            if (!selected(selection, t.number)) {
                continue;
            }
            Clipboard::Track track;
            track.streamNumber = t.number;
            for (const Event& e : t.events) {
                if (e.time >= from && e.time <= thru && options.filter.includes(e, sequence.meterMap)) {
                    Event c = e;
                    c.time -= from;
                    track.events.push_back(std::move(c));
                }
            }
            if (!track.events.empty()) {
                clip.tracks.push_back(std::move(track));
            }
        }
    }
    if (options.tempo) {
        for (const TempoEntry& e : sequence.tempoMap.entries) {
            if (e.time >= from && e.time <= thru) {
                TempoEntry c = e;
                c.time -= from;
                clip.tempo.push_back(c);
            }
        }
    }
    if (options.meterKey && wholeMeasures(sequence, from, thru)) {
        const int first = sequence.meterMap.measureAt(from);
        const int last = sequence.meterMap.measureAt(thru);
        clip.measures = last - first + 1;
        for (int m = first; m <= last; ++m) {
            const MeterEntry& me = sequence.meterMap.atMeasure(m);
            const bool change = m == first || me.measure == m ||
                                std::ranges::find(sequence.keyMap, m, &KeyEntry::measure) != sequence.keyMap.end();
            if (change) {
                clip.meterKey.push_back({m - first, me.numerator, me.denominatorLog2, alterationsAt(sequence, m)});
            }
        }
    }
    return clip;
}

Clipboard cut(Sequence& sequence, const Selection& selection, const CutOptions& options) {
    CopyOptions co;
    co.events = options.events;
    co.filter = options.filter;
    co.tempo = options.tempo;
    co.meterKey = options.meterKey;
    Clipboard clip = copy(sequence, selection, co);
    const Tick from = std::min(selection.from, selection.thru);
    const Tick thru = std::max(selection.from, selection.thru);
    if (options.events) {
        for (Stream& t : sequence.streams) {
            if (!selected(selection, t.number)) {
                continue;
            }
            std::erase_if(t.events, [&](const Event& e) {
                return e.time >= from && e.time <= thru && options.filter.includes(e, sequence.meterMap);
            });
            if (options.deleteHole) {
                shiftEvents(t, thru + 1, -clip.length);
            }
        }
    }
    if (options.tempo) {
        std::erase_if(sequence.tempoMap.entries,
                      [&](const TempoEntry& e) { return e.time > 0 && e.time >= from && e.time <= thru; });
        if (options.deleteHole) {
            shiftTempo(sequence.tempoMap, thru + 1, -clip.length);
        }
    }
    if (options.meterKey && clip.measures > 0) {
        const int first = sequence.meterMap.measureAt(from);
        eraseMeterKey(sequence, first, first + clip.measures - 1);
        if (options.deleteHole) {
            shiftMeterKey(sequence, first + clip.measures, -clip.measures);
        }
    }
    return clip;
}

int paste(Sequence& sequence, const Clipboard& clipboard, const PasteOptions& options) {
    const int reps = std::max(1, options.repetitions);
    const Tick span = clipboard.length * reps;
    const Tick to = options.to;
    const int toMeasure = sequence.meterMap.measureAt(to);
    const bool withTempo = options.tempo && !clipboard.tempo.empty();
    const bool withMeterKey = options.meterKey && clipboard.measures > 0;
    std::vector<int> affected = options.streams;
    if (options.toStream) {
        affected = {*options.toStream};
    }
    const auto isAffected = [&](const int n) { return std::ranges::find(affected, n) != affected.end(); };

    if (options.mode == PasteMode::Replace) {
        for (Stream& t : sequence.streams) {
            if (isAffected(t.number)) {
                std::erase_if(t.events, [&](const Event& e) { return e.time >= to && e.time < to + span; });
            }
        }
        if (withTempo) {
            std::erase_if(sequence.tempoMap.entries,
                          [&](const TempoEntry& e) { return e.time > 0 && e.time >= to && e.time < to + span; });
        }
        if (withMeterKey) {
            eraseMeterKey(sequence, toMeasure, toMeasure + clipboard.measures * reps - 1);
        }
    } else if (options.mode == PasteMode::SlideOver) {
        for (Stream& t : sequence.streams) {
            if (isAffected(t.number)) {
                shiftEvents(t, to, span);
            }
        }
        if (withTempo) {
            shiftTempo(sequence.tempoMap, to, span);
        }
        if (withMeterKey) {
            shiftMeterKey(sequence, toMeasure, clipboard.measures * reps);
        }
    }

    int inserted = 0;
    for (int r = 0; r < reps; ++r) {
        const Tick base = to + clipboard.length * r;
        if (options.events) {
            for (const Clipboard::Track& track : clipboard.tracks) {
                Stream& t = sequence.stream(options.toStream.value_or(track.streamNumber));
                for (const Event& e : track.events) {
                    Event c = e;
                    c.time += base;
                    t.events.push_back(std::move(c));
                    ++inserted;
                }
            }
        }
        if (withTempo) {
            for (const TempoEntry& e : clipboard.tempo) {
                sequence.tempoMap.set(base + e.time, e.bpm100);
            }
        }
        if (withMeterKey) {
            for (const ClipMeterKey& mk : clipboard.meterKey) {
                setMeterKey(sequence, toMeasure + clipboard.measures * r + mk.measure, mk);
            }
        }
    }
    if (inserted > 0) {
        for (Stream& t : sequence.streams) {
            sortEvents(t.events);
        }
    }
    return inserted;
}

Sequence toSequence(const Clipboard& clipboard, const int ppq) {
    Sequence s;
    s.ppq = ppq;
    s.syncPpq();
    for (const Clipboard::Track& track : clipboard.tracks) {
        s.stream(track.streamNumber).events = track.events;
    }
    for (const TempoEntry& e : clipboard.tempo) {
        s.tempoMap.set(e.time, e.bpm100);
    }
    for (const ClipMeterKey& mk : clipboard.meterKey) {
        setMeterKey(s, 1 + mk.measure, mk);
    }
    return s;
}

Clipboard fromSequence(const Sequence& sequence) {
    Clipboard clip;
    clip.length = sequence.endTime() + 1;
    for (const Stream& t : sequence.streams) {
        if (!t.events.empty()) {
            clip.tracks.push_back({t.number, t.events});
        }
    }
    clip.tempo = sequence.tempoMap.entries;
    const int measures = sequence.meterMap.measureAt(std::max<Tick>(0, clip.length - 1));
    clip.measures = measures;
    for (int m = 1; m <= measures; ++m) {
        const MeterEntry& me = sequence.meterMap.atMeasure(m);
        if (m == 1 || me.measure == m ||
            std::ranges::find(sequence.keyMap, m, &KeyEntry::measure) != sequence.keyMap.end()) {
            clip.meterKey.push_back({m - 1, me.numerator, me.denominatorLog2, alterationsAt(sequence, m)});
        }
    }
    return clip;
}

// ---------------------------------------------------------------- Undo

void Undo::before(const Sequence& sequence, std::string command) {
    saved_ = sequence;
    command_ = std::move(command);
    undone_ = false;
}

std::string Undo::label() const {
    if (!saved_) {
        return "Undo";
    }
    return (undone_ ? "Redo " : "Undo ") + command_;
}

bool Undo::apply(Sequence& sequence) {
    if (!saved_) {
        return false;
    }
    std::swap(*saved_, sequence);
    undone_ = !undone_;
    return true;
}

void Undo::clear() noexcept {
    saved_.reset();
    command_.clear();
    undone_ = false;
}

} // namespace sseq::edit
