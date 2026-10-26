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

/// A short MIDI message (1 to 3 bytes, 1 for the real-time sync messages) to deliver at an absolute
/// clock time.
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

/// What the transport sends, as observed on the MIDI output of the original engine:
///  - Start: the patch, volume and pan of every audible stream with events, in stream-number order;
///    then, with `chase`, for every audible stream that still has events at or after the start
///    position, the last patch, pitch wheel, modulation (CC 1), sustain (CC 64) and volume (CC 7)
///    before it, in that order.
///  - Events at one tick: due note-offs first, then the streams' events in stream-number order and,
///    within a stream, in stored order; sysex after the short messages of the tick.
///  - Note-offs wait in a binary min-heap keyed by time; equal times leave the heap in the heap's own
///    order (which is not the order the notes started in). A note-on for a key that is sounding on
///    the same port and channel first sends a note-off; a note-off for a key that is not sounding is
///    dropped, so the first note-off due for a key ends it, whichever note it belonged to.
///  - Stop (and the end of the sequence): MIDI Stop when sync is on; then for each channel 0..15 the
///    note-offs of its sounding notes in key order followed by All Notes Off (CC 123); then, with
///    `zeroControllersOnStop`, for each channel: pitch wheel centre, CC 1 = 0, CC 64 = 0, CC 121.
///  - Key+ outside 0..127 folds the key by octaves into range; Vel+ clamps to 1..127; Time+ that
///    moves an event before the start plays it at tick 0.
///  - Metronome: the click's duration counts MIDI clocks (1/24 quarter note); it ends two clocks
///    before that count (1 wraps round to 23), at the latest one clock before the next measure.
///  - Auto-shuttle with auto-restart: playback runs to the stop time, whose events do not play, and
///    continues at the rewind time without a stop (no All Notes Off at the wrap).
///  - No Song Position Pointer is ever sent.
/// The engine generations differ in a few points; see EngineVersion.

/// Which generation of the original engine to imitate. The three share everything described
/// above, except:
///  - v1_01: an event that Time+ moves before tick 0 does not play; no MIDI sync out.
///  - v3_00: search-back also chases pan (CC 10, after volume), and also for streams whose events
///    all lie before the start position; the metronome click ends one MIDI clock before its
///    duration (not two, so a duration of 1 ends at once); with the accent on, the first beat of a
///    measure clicks with velocity 127.
enum class EngineVersion : std::uint8_t {
    v1_01, // TTSSEQ.DLL 1.01, Cakewalk Professional for Windows demo (1992)
    v2_10, // TTSSEQ.DLL 2.10, Cakewalk Apprentice 1.04 (1993)
    v3_00, // TTSSEQ30.DLL 3.00, Cakewalk Professional 2.00
};

/// Which program's use of the engine to imitate. The engine is the same; the programs differ in
/// what they take from the file.
///  - Apprentice ignores the file's auto-shuttle settings and does not send auto-send sysex banks.
///  - Professional applies auto-stop, auto-rewind and auto-restart (PlaybackOptions::fromSequence)
///    and sends the auto-send sysex banks when the file is opened (autoSendBanks()).
enum class Program : std::uint8_t {
    Apprentice,
    Professional,
};

struct PlaybackOptions {
    double lookAhead = 0.1;           // seconds scheduled ahead of the clock by each pump()
    double tempoRatio = 1.0;          // playback speed (Cakewalk's 0.50 / 1.00 / 2.00 buttons)
    bool sendStreamParameters = true; // patch, volume, pan of each stream at start
    bool chase = true; // on start, send the last patch, pitch wheel, modulation, sustain and volume before the
                       // position ("patch/wheel/controller search-back")
    bool zeroControllersOnStop = false; // on stop, centre the pitch wheel, zero modulation, lift sustain and send
                                        // "reset all controllers" on every channel of every port in use
    bool sendSysexBanks = true;         // play sysex events (the bank's bytes)
    EngineVersion engine = EngineVersion::v2_10; // which generation's differences to reproduce
    std::optional<Tick> loopFrom;    // auto-shuttle with restart: when the position reaches loopThru (the end of the
    std::optional<Tick> loopThru;    // sequence when unset), continue at loopFrom; only when playback starts before it
    std::optional<Tick> stopAt;      // auto-stop: stop when the position reaches this tick (its events do not play);
                                     // only when playback starts before it
    std::optional<Tick> rewindTo;    // auto-rewind: when playback stops by itself (end or stopAt), Now goes here
    bool metronome = false;          // play the sequence's metronome note on every beat
    bool metronomeAccent = false;    // engine 3.00: the first beat of a measure clicks with velocity 127; the older
                                     // engines' MIDI click does not change with it (PC-speaker click only)
    int countInMeasures = 0;         // measures of metronome before the start position (recording)
    bool stopAtEnd = true;           // stop when the last event has played; false while recording
    bool midiClock = false;          // MIDI sync out: Start or Continue on play, Clock 24 per quarter note, Stop
                                     // (ignored by EngineVersion::v1_01)
    bool startNeverContinue = false; // always Start, also when playback does not begin at tick 0
    bool clockWhileStopped = true;   // with midiClock: keep sending Clock at the tempo while stopped (pump())

    /// Options taken from the sequence's own settings (VARS): chase and zero-controllers flags, metronome
    /// during playback or recording (with count-in and accent), MIDI sync; recording runs past the end.
    /// With Program::Professional also the auto-shuttle: auto-stop, auto-rewind and auto-restart. With all
    /// three (the case observed) or with rewind and restart, the range loops (loopFrom, loopThru); otherwise
    /// stopAt and rewindTo. Restart without rewind has nothing to restart into and is ignored.
    [[nodiscard]] static PlaybackOptions fromSequence(const Sequence& sequence, bool recording = false,
                                                      Program program = Program::Apprentice);
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
    /// playing (a 10-20 ms timer is fine). Returns false once playback has stopped by itself. While
    /// stopped it returns false and only sends the MIDI clock (midiClock and clockWhileStopped).
    bool pump();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// What loading the sequence sends: the patch, volume and pan of every stream with events, in
/// stream-number order, muted streams included (time 0).
[[nodiscard]] std::vector<MidiMessage> openMessages(const Sequence& sequence);

/// The sysex banks marked auto-send, as indexes into sequence.sysxBanks in bank-number order.
/// Cakewalk Professional sends them (port 0) when it opens the file; Apprentice does not. Whether
/// they go before or after openMessages() is not known: no capture has both.
[[nodiscard]] std::vector<std::size_t> autoSendBanks(const Sequence& sequence);

/// Offline rendering: every message of the sequence between `from` and `thru` (default: the end),
/// with times in seconds from `from`, as the Transport would produce them without a clock. The
/// output ends with what the transport sends when it stops at the end (or at `thru`).
[[nodiscard]] std::vector<MidiMessage> render(const Sequence& sequence, const PlaybackOptions& options = {},
                                              Tick from = 0, std::optional<Tick> thru = std::nullopt);

} // namespace sseq::engine
