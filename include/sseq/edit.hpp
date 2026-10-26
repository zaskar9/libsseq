// libsseq: the Edit menu's data transformations (Quantize, ...). In the original these live in
// the application, not in the engine (DESIGN.md, "Not in TTSSEQ"); the arithmetic is pinned down
// with Cakewalk Apprentice 1.04 oracles (tests/data/edit).
#pragma once

#include "event.hpp"
#include "sequence.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// ---------------------------------------------------------------- Length

struct LengthOptions {
    bool startTimes = true; // "Starting times": distances from the region's From scale
    bool durations = false; // "Durations" (notes)
    int percent = 100;      // "Percent"
    EventFilter filter = EventFilter::all();
};

/// `ticks * percent / 100`, rounded to nearest with half up (241 at 50 % = 121, 1 at 33 % = 0).
[[nodiscard]] Tick scaleTicks(Tick ticks, int percent) noexcept;

/// Edit > Length: start times move to `from + scaleTicks(time - from)`, note durations to
/// `scaleTicks(duration)` (no minimum: a duration can become 0). Returns the number of events
/// changed; streams are re-sorted.
int length(Sequence& sequence, const Selection& selection, const LengthOptions& options);

// ---------------------------------------------------------------- Transpose

struct TransposeOptions {
    int amount = 0;        // half-steps, or diatonic steps when `diatonic`
    bool diatonic = false; // "Diatonic math": steps of the major scale of the key signature in force
    EventFilter filter = EventFilter::only(EventKind::Note).set(EventKind::KeyAftertouch);
};

/// A key moved by `halfSteps`, then by octaves back into 0..127 (127 + 5 = 132 → 120).
[[nodiscard]] int transposeKey(int key, int halfSteps) noexcept;
/// A key moved by `steps` of the major scale with `alterations` sharps (negative: flats). A key
/// outside the scale counts as a degree by its distance from the tonic: 1 and 3 half-steps up as
/// the degree above, 6 and 8 as the degree below, 10 as the degree above (Cakewalk's table).
/// The result is folded into 0..127 like transposeKey.
[[nodiscard]] int transposeKeyDiatonic(int key, int steps, int alterations) noexcept;

/// Edit > Transpose on Note and KeyAft events of the region (or the filter's kinds, for those
/// that carry a key). The key signature is the sequence's at each event's measure. Returns the
/// number of events changed.
int transpose(Sequence& sequence, const Selection& selection, const TransposeOptions& options);

// ---------------------------------------------------------------- Slide

struct SlideOptions {
    Tick amount = 0; // ticks, negative = earlier; see measuresToTicks for the dialog's "Measures"
    EventFilter filter = EventFilter::all();
};

/// The "Measures" amount of the Slide dialog in ticks: `measures` lengths of the measure at `from`.
/// Verified with a constant meter only.
[[nodiscard]] Tick measuresToTicks(const Sequence& sequence, Tick from, int measures) noexcept;

/// Edit > Slide: the region's events move by `amount`. A region cannot move before tick 0: the
/// amount is clamped to `-selection.from` (Cakewalk Apprentice 1.04 does this silently; its help
/// speaks of an error message). Returns the number of events changed; streams are re-sorted.
int slide(Sequence& sequence, const Selection& selection, const SlideOptions& options);

// ---------------------------------------------------------------- Controller fill

struct ControllerFillOptions {
    std::optional<int> controller; // controller number 0..119; nullopt = pitch wheel ("What")
    std::uint8_t channel = 0;      // "MIDI Channel", 0-based here
    int begin = 0;                 // "Value range": 0..127, or -8192..8191 for the wheel
    int end = 0;
    Tick from = 0; // "Time range", both inclusive
    Tick thru = 0;
};

/// Edit > Controller fill into one stream (Cakewalk fills the current track): the stream's events
/// of the same kind, number and channel in from..thru are removed, then one event per value from
/// `begin` to `end` is inserted at `from + floor(|v - begin| * (thru - from) / |end - begin|)`,
/// skipping a value whose tick is already taken (so 0..127 over 120 ticks has no 1). The first
/// value lands on `from`, the last on `thru`. New events go before existing ones on the same tick.
/// Returns the number of events inserted.
int controllerFill(Stream& stream, const ControllerFillOptions& options);

} // namespace sseq::edit
