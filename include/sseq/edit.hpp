// libsseq: the Edit menu's data transformations (Quantize, ...). In the original these live in
// the application, not in the engine (DESIGN.md, "Not in TTSSEQ"); the arithmetic is pinned down
// with Cakewalk Apprentice 1.04 oracles (tests/data/edit).
#pragma once

#include "event.hpp"
#include "maps.hpp"
#include "sequence.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sseq::edit {

/// An inclusive value range, inverted by `exclude` (the dialog's "Not").
struct ValueRange {
    int min = 0;
    int max = 127;
    bool exclude = false;

    [[nodiscard]] bool contains(const int value) const noexcept {
        const bool inside = value >= min && value <= max;
        return exclude ? !inside : inside;
    }
};

/// Cakewalk's Event Filter dialog ("Use event filter"): which event kinds an edit touches, and for
/// the six MIDI channel kinds, ranges on their fields (all inclusive, all required). The "any
/// kind of event" ranges (channel, beat within the measure, tick within the beat) apply to those
/// six kinds as well. Sysex, Text, MCI and Wave events are not covered by the dialog: a filter
/// built from it always passes them, so `kinds` keeps them true; "Nothing" clears the six only.
/// Without the dialog, each command has its own default (Quantize: notes only).
struct EventFilter {
    std::array<bool, 10> kinds{}; // "Include", indexed by EventKind
    ValueRange noteKey{0, 127};
    ValueRange noteVelocity{0, 127};
    ValueRange noteDuration{0, 65535};
    ValueRange keyAftertouchKey{0, 127};
    ValueRange keyAftertouchValue{0, 127};
    ValueRange controllerNumber{0, 127};
    ValueRange controllerValue{0, 127};
    ValueRange patchNumber{0, 127};
    ValueRange channelAftertouchAmount{0, 127};
    ValueRange pitchWheelValue{-8192, 8191};
    ValueRange channel{0, 15}; // 0-based here (the dialog shows 1..16)
    ValueRange beat{1, 99};    // 1-based beat within the measure
    ValueRange tick{0, 1919};  // tick within the beat

    [[nodiscard]] static EventFilter none() noexcept { return {}; }
    [[nodiscard]] static EventFilter all() noexcept;
    [[nodiscard]] static EventFilter only(EventKind kind) noexcept;
    /// The dialog's "Nothing": no MIDI channel kind, the other kinds still pass.
    [[nodiscard]] static EventFilter nothing() noexcept;
    [[nodiscard]] bool includes(EventKind kind) const noexcept { return kinds[static_cast<std::size_t>(kind)]; }
    /// The kind, its field ranges and the channel; beat and tick need the meter map.
    [[nodiscard]] bool includes(const Event& event) const noexcept;
    [[nodiscard]] bool includes(const Event& event, const MeterMap& meter) const noexcept;
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

// ---------------------------------------------------------------- Cut, Copy, Paste

/// A meter and key signature at a measure of the clipboard (measure 0 = the region's first).
struct ClipMeterKey {
    int measure = 0;
    int numerator = 4;
    int denominatorLog2 = 2;
    int alterations = 0;
};

/// What Copy and Cut put on the clipboard, and what Paste reads: the region's events per source
/// stream with times relative to From, its tempo entries, and, for a region of whole measures, the
/// meter and key per measure. Cakewalk also offers it to other applications as a Standard MIDI
/// File image; `smf::write` of a sequence built from it gives the same.
struct Clipboard {
    struct Track {
        int streamNumber = 0;
        std::vector<Event> events; // times relative to From
    };
    Tick length = 0;                    // thru - from + 1: the span, and the step between repetitions
    std::vector<Track> tracks;          // streams that had events in the region ("unblank tracks")
    std::vector<TempoEntry> tempo;      // times relative to From; empty when not copied
    std::vector<ClipMeterKey> meterKey; // entry 0 restates the state at From; empty when not copied
    int measures = 0;                   // whole measures covered when meterKey was copied

    [[nodiscard]] bool empty() const noexcept { return tracks.empty() && tempo.empty() && meterKey.empty(); }
};

struct CopyOptions {
    bool events = true; // "Events": the selected streams' events in From..Thru (start time)
    EventFilter filter = EventFilter::all();
    bool tempo = false;    // "Tempo changes" in From..Thru
    bool meterKey = false; // "Meter/key changes": only when From..Thru is whole measures
};

/// Edit > Copy. Meter and key are copied only when From starts a measure and Thru ends one
/// (Cakewalk greys the box otherwise).
[[nodiscard]] Clipboard copy(const Sequence& sequence, const Selection& selection, const CopyOptions& options);

struct CutOptions {
    bool events = true;
    EventFilter filter = EventFilter::all(); // Cakewalk: not together with deleteHole
    bool deleteHole = false; // move what follows the region earlier by its length, on the selected streams
    bool tempo = false;
    bool meterKey = false;
};

/// Edit > Cut: copy(), then removal. With deleteHole the selected streams' later events move
/// earlier by the region's length; so do the tempo map (when tempo is cut) and the meter/key map
/// by the region's measures (when they are cut). Cakewalk Apprentice 1.04 moves the tempo map by
/// one tick less than the events in that case; libsseq moves both by the same amount.
Clipboard cut(Sequence& sequence, const Selection& selection, const CutOptions& options);

enum class PasteMode {
    Blend,    // "Blend old and new material"
    Replace,  // "Replace old material with new": clear the destination range first
    SlideOver // "Slide over existing material to make room": move the destination later first
};

struct PasteOptions {
    Tick to = 0;         // "To time"
    int repetitions = 1; // "Repetitions", one after another
    PasteMode mode = PasteMode::Blend;
    bool events = true; // "Paste what"
    bool tempo = false; // Cakewalk allows this with Replace and SlideOver only
    bool meterKey = false;
    std::optional<int> toStream; // "Paste to One track": every clipboard track goes to this stream
    std::vector<int> streams;    // the selected streams: Replace clears and SlideOver moves these
                                 // (ignored with toStream, where only that stream is affected)
};

/// Edit > Paste / Paste to One track. The destination range is `to .. to + length * repetitions - 1`
/// on the affected streams: Replace removes its events (and tempo entries, when tempo is pasted),
/// SlideOver moves everything from `to` on later by that span (tempo and meter/key maps too when
/// pasted). Then each repetition's events go to their source streams (or `toStream`), tempo entries
/// are set at their times, and meter/key entries at the measure of `to` onward. Returns the number
/// of events inserted.
int paste(Sequence& sequence, const Clipboard& clipboard, const PasteOptions& options);

/// The clipboard as a sequence (times from 0, the tracks as streams with their numbers, tempo and
/// meter/key maps), for a Standard MIDI File image on a system pasteboard: `smf::write(toSequence(clip, ppq))`.
[[nodiscard]] Sequence toSequence(const Clipboard& clipboard, int ppq);
/// A clipboard from a sequence read from such an image: every stream, the whole tempo and
/// meter/key maps, length = the end of the last event + 1.
[[nodiscard]] Clipboard fromSequence(const Sequence& sequence);

// ---------------------------------------------------------------- Undo

/// Edit > Undo, one level as in Cakewalk: the sequence as it was before the last Edit command.
/// After undo() the same entry serves as "Redo <command>"; a new before() replaces it.
class Undo {
public:
    void before(const Sequence& sequence, std::string command); // call before applying an Edit command
    [[nodiscard]] bool available() const noexcept { return saved_.has_value(); }
    /// "Undo Quantize", "Redo Quantize", or "Undo" when nothing is saved (the menu item's text).
    [[nodiscard]] std::string label() const;
    /// Swaps the saved state with the sequence: undo, or redo after an undo. False when nothing is saved.
    bool apply(Sequence& sequence);
    void clear() noexcept;

private:
    std::optional<Sequence> saved_;
    std::string command_;
    bool undone_ = false;
};

} // namespace sseq::edit
