// libsseq: the Edit menu's data transformations (Quantize, ...). In the original these live in
// the application, not in the engine (DESIGN.md, "Not in TTSSEQ"); the arithmetic is pinned down
// with Cakewalk Apprentice 1.04 oracles (tests/data/edit).
#pragma once

#include "event.hpp"
#include "sequence.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace sseq::edit {

/// Which event kinds an edit touches (Cakewalk's "Use event filter"). Without a filter, each
/// command has its own default (Quantize: notes only).
struct EventFilter {
    std::array<bool, 10> kinds{}; // indexed by EventKind

    [[nodiscard]] static EventFilter none() noexcept { return {}; }
    [[nodiscard]] static EventFilter all() noexcept;
    [[nodiscard]] static EventFilter only(EventKind kind) noexcept;
    [[nodiscard]] bool includes(EventKind kind) const noexcept { return kinds[static_cast<std::size_t>(kind)]; }
    [[nodiscard]] bool includes(const Event& event) const noexcept { return includes(event.kind); }
    EventFilter& set(EventKind kind, bool on = true) noexcept {
        kinds[static_cast<std::size_t>(kind)] = on;
        return *this;
    }
};

/// The selected region: the streams (by number) and the From..Thru time range. An event is in the
/// region when its start time is between `from` and `thru`, both inclusive; where it ends does
/// not matter.
struct Selection {
    std::vector<int> streams;
    Tick from = 0;
    Tick thru = 0;

    /// Every stream with events, from tick 0 through the end of the sequence (Mark > Select All).
    [[nodiscard]] static Selection all(const Sequence& sequence);
    [[nodiscard]] bool contains(int streamNumber, Tick time) const noexcept;
};

/// The Quantize dialog's resolutions. Triplets are two thirds of the straight value.
enum class Resolution {
    Whole,
    Half,
    Quarter,
    Eighth,
    EighthTriplet,
    Sixteenth,
    SixteenthTriplet,
    ThirtySecond,
    ThirtySecondTriplet,
};

/// The resolution in ticks at a given ppq (whole = 4 * ppq, eighth triplet = ppq / 3, ...).
[[nodiscard]] Tick resolutionTicks(Resolution resolution, int ppq) noexcept;

struct QuantizeOptions {
    Tick resolution = 30;   // ticks (resolutionTicks(Resolution::Sixteenth, 120))
    bool startTimes = true; // "Starting times"
    bool durations = true;  // "Durations"
    int strength = 100;     // "Percent strength", applies to start times only
    EventFilter filter = EventFilter::only(EventKind::Note);
};

/// A start time rounded to the nearest multiple of `resolution` (exactly half rounds up), then
/// moved only `strength` percent of the way (truncated toward zero).
[[nodiscard]] Tick quantizeTime(Tick time, Tick resolution, int strength = 100) noexcept;
/// A duration rounded to the nearest multiple of `resolution`, at least one multiple, minus one
/// tick so that consecutive notes do not run into each other.
[[nodiscard]] Tick quantizeDuration(Tick duration, Tick resolution) noexcept;

/// Edit > Quantize on the region. Returns the number of events changed. Streams whose events
/// moved are re-sorted (stable).
int quantize(Sequence& sequence, const Selection& selection, const QuantizeOptions& options);

} // namespace sseq::edit
