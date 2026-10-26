// libsseq: Edit menu transformations. The rounding rules come from Cakewalk Apprentice 1.04
// oracles (tests/data/edit, tests/edit_tests.cpp).
#include "sseq/edit.hpp"

#include <algorithm>

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

} // namespace sseq::edit
