// libsseq: Edit menu transformations. The rounding rules come from Cakewalk Apprentice 1.04
// oracles (tests/data/edit, tests/edit_tests.cpp).
#include "sseq/edit.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
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
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e)) {
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
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e)) {
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
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e) ||
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
            if (e.time < selection.from || e.time > selection.thru || !options.filter.includes(e)) {
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

} // namespace sseq::edit
