// libsseq: recording (TTSSEQ: SeqRecordStart/Stop/Cleanup, SeqRecordEvent, SeqRecCount).
//
// The platform layer receives MIDI input with timestamps on the same Clock the Transport uses and
// hands the messages to a Recorder. The Recorder converts times to ticks through the Transport,
// pairs note-ons with note-offs, and collects the take. When recording stops, the take is merged
// into a stream ("Keep take?") or discarded.
#pragma once

#include "engine.hpp"
#include "sequence.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace sseq::engine {

struct RecordOptions {
    bool notes = true;
    bool controllers = true;
    bool patches = true;
    bool pitchWheel = true;
    bool aftertouch = true;       // key and channel aftertouch
    std::optional<Tick> punchIn;  // keep only events starting at or after punchIn ...
    std::optional<Tick> punchOut; // ... and before punchOut
};

class Recorder {
public:
    /// The transport must outlive the recorder. Only input received while the transport is playing
    /// (and past the count-in) is recorded.
    explicit Recorder(const Transport& transport, RecordOptions options = {});
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) = delete;
    Recorder& operator=(Recorder&&) = delete;

    [[nodiscard]] const RecordOptions& options() const noexcept;
    void setOptions(const RecordOptions& options);

    /// A received channel message (1 to 3 bytes) with its arrival time on the transport's clock.
    void receive(const MidiMessage& message);
    /// Ends the take: notes still held are closed at `time`.
    void finish(double time);

    [[nodiscard]] const std::vector<Event>& events() const noexcept; // the take so far, in time order
    [[nodiscard]] std::size_t count() const noexcept;                // events recorded (SeqRecCount)
    [[nodiscard]] bool empty() const noexcept;
    void clear();

    /// Merges the take into a stream (sound on sound) and clears it.
    void commit(Stream& stream);
    /// Removes the stream's events that start in [from, thru) first (overwrite), then merges.
    void commitReplacing(Stream& stream, Tick from, Tick thru);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sseq::engine
