// libsseq: playback engine (TTSSEQ: SeqPlayStart/Stop, SeqRewind, SeqSetNow, SeqSetPlayingSeq, ...).
//
// The engine is platform-neutral. The application provides a Clock (monotonic seconds) and a
// MidiSink (delivers timestamped messages, for example through CoreMIDI) and calls
// Transport::pump() periodically. pump() schedules every event whose time falls within a
// look-ahead window, with absolute timestamps, so the caller's timing jitter does not reach the
// MIDI port.
#pragma once

#include "sequence.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace sseq::engine {

/// A short MIDI message (1 to 3 bytes) to deliver at an absolute clock time.
struct MidiMessage {
    double time = 0; // seconds on the Clock's timeline
    int port = 0;    // 0-based output port, as in the sequence
    std::uint8_t size = 0;
    std::array<std::uint8_t, 3> data{};

    [[nodiscard]] std::uint8_t status() const noexcept { return data[0]; }
    [[nodiscard]] std::uint8_t channel() const noexcept { return data[0] & 0x0fU; }
    [[nodiscard]] bool isNoteOn() const noexcept { return (data[0] & 0xf0U) == 0x90 && data[2] != 0; }
    [[nodiscard]] bool isNoteOff() const noexcept {
        return (data[0] & 0xf0U) == 0x80 || ((data[0] & 0xf0U) == 0x90 && data[2] == 0);
    }
};

/// Where messages go. Implementations must accept timestamps up to the look-ahead in the future
/// and deliver the messages at those times (CoreMIDI does this natively).
class MidiSink {
public:
    MidiSink() = default;
    virtual ~MidiSink() = default;
    MidiSink(const MidiSink&) = delete;
    MidiSink& operator=(const MidiSink&) = delete;
    MidiSink(MidiSink&&) = delete;
    MidiSink& operator=(MidiSink&&) = delete;
    virtual void send(const MidiMessage& message) = 0;
    virtual void sendSysex(double time, int port, const std::vector<std::uint8_t>& bytes) = 0;
    /// Called at the end of every pump() so batched implementations can hand their packets over.
    virtual void flush() {}
    /// Called before the transport silences its notes on stop, seek and panic: messages already
    /// handed over with a time still in the future must not be delivered (CoreMIDI:
    /// MIDIFlushOutput). Otherwise a note-on scheduled up to the look-ahead ahead would sound
    /// after the note-offs and never end.
    virtual void discardScheduled() {}
};

/// Monotonic time in seconds.
class Clock {
public:
    Clock() = default;
    virtual ~Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    [[nodiscard]] virtual double now() const = 0;
};

/// A Clock on std::chrono::steady_clock.
class SteadyClock final : public Clock {
public:
    SteadyClock();
    [[nodiscard]] double now() const override;

private:
    double origin_ = 0;
};

struct PlaybackOptions {
    double lookAhead = 0.1;           // seconds scheduled ahead of the clock by each pump()
    double tempoRatio = 1.0;          // playback speed (Cakewalk's 0.50 / 1.00 / 2.00 buttons)
    bool sendStreamParameters = true; // patch, volume, pan of each stream at start
    bool chase = true; // on start, send the last patch, pitch wheel, modulation and sustain before the position
                       // on every channel used ("patch/controller search-back")
    bool zeroControllersOnStop = false; // on stop, lift sustain, zero modulation, centre the pitch wheel and send
                                        // "reset all controllers" on every channel of every port in use
    bool sendSysexBanks = true;         // play sysex events (the bank's bytes)
    std::optional<Tick> loopFrom;       // auto-shuttle: when the position reaches loopThru, continue at loopFrom
    std::optional<Tick> loopThru;
    bool metronome = false;       // play the sequence's metronome note on every beat
    bool metronomeAccent = false; // first beat of each measure at full velocity, other beats softer
    int countInMeasures = 0;      // measures of metronome before the start position (recording)
    bool stopAtEnd = true;        // stop when the last event has played; false while recording

    /// Options taken from the sequence's own settings (VARS): chase and zero-controllers flags, metronome
    /// during playback or recording (with count-in and accent); recording runs past the end.
    [[nodiscard]] static PlaybackOptions fromSequence(const Sequence& sequence, bool recording = false);
};

/// Called when a string event (Text, MCI command, Wave) reaches its time. Runs inside pump().
using StringEventHandler = std::function<void(int streamNumber, const Event& event, double time)>;

class Transport {
public:
    /// The sequence must outlive the transport and must not be modified while playing.
    Transport(const Sequence& sequence, Clock& clock, MidiSink& sink, PlaybackOptions options = {});
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    [[nodiscard]] const PlaybackOptions& options() const noexcept;
    void setOptions(const PlaybackOptions& options); // takes effect at the next play() or seek()
    void setTempoRatio(double ratio);                // takes effect immediately
    void setStringEventHandler(StringEventHandler handler);

    void play();
    void stop();
    void panic();         // stop, then all-notes-off and controller reset on every channel of every port
    void rewind();        // stop and go to tick 0
    void seek(Tick tick); // while playing: notes are cut and playback continues from `tick`
    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] bool countingIn() const;       // playing, but still inside the count-in
    [[nodiscard]] Tick now() const;              // current position (advances with the clock while playing)
    [[nodiscard]] Tick endTick() const noexcept; // end of the last event
    /// The sequence position a clock time corresponds to, while playing and past the count-in (for
    /// recording); nullopt otherwise.
    [[nodiscard]] std::optional<Tick> tickAt(double time) const;

    void setMuted(int streamNumber, bool muted); // overrides the stream's own mute flag
    void setSolo(int streamNumber, bool solo);   // when any stream is soloed, only soloed streams play
    [[nodiscard]] bool isAudible(int streamNumber) const;

    /// Schedules everything up to clock.now() + lookAhead. Call every few milliseconds while
    /// playing (a 10-20 ms timer is fine). Returns false once playback has stopped by itself.
    bool pump();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Offline rendering: every message of the sequence between `from` and `thru` (default: the end),
/// with times in seconds from `from`, as the Transport would produce them without a clock.
[[nodiscard]] std::vector<MidiMessage> render(const Sequence& sequence, const PlaybackOptions& options = {},
                                              Tick from = 0, std::optional<Tick> thru = std::nullopt);

} // namespace sseq::engine
