// libsseq: playback engine (TTSSEQ: SeqPlayStart/Stop, SeqRewind, SeqSetNow, SeqSetPlayingSeq, ...).
//
// The engine is platform-neutral. The application provides a Clock (monotonic seconds) and a
// MidiSink (delivers timestamped messages, for example through CoreMIDI) and calls
// Transport::pump() periodically. pump() schedules every event whose time falls within a
// look-ahead window, with absolute timestamps, so the caller's timing jitter does not reach the
// MIDI port.
#pragma once

#include "song.hpp"
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
    int port = 0;    // 0-based output port, as in the song
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
    double lookAhead = 0.1;          // seconds scheduled ahead of the clock by each pump()
    double tempoRatio = 1.0;         // playback speed (Cakewalk's 0.50 / 1.00 / 2.00 buttons)
    bool sendTrackParameters = true; // patch, volume, pan of each track at start
    bool chase = true; // on start, send the last patch/controller/wheel before the position ("search-back")
    bool zeroControllersOnStop = false; // on stop, reset mod wheel, sustain, pitch wheel, channel pressure
    bool sendSysexBanks = true;         // play sysex events (the bank's bytes)
    std::optional<Tick> loopFrom;       // auto-shuttle: when the position reaches loopThru, continue at loopFrom
    std::optional<Tick> loopThru;

    /// Options taken from the song's own settings (VARS): chase and zero-controllers flags.
    [[nodiscard]] static PlaybackOptions fromSong(const Song& song);
};

/// Called when a string event (Text, MCI command, Wave) reaches its time. Runs inside pump().
using StringEventHandler = std::function<void(int trackNumber, const Event& event, double time)>;

class Transport {
public:
    /// The song must outlive the transport and must not be modified while playing.
    Transport(const Song& song, Clock& clock, MidiSink& sink, PlaybackOptions options = {});
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
    void rewind();        // stop and go to tick 0
    void seek(Tick tick); // while playing: notes are cut and playback continues from `tick`
    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] Tick now() const;              // current position (advances with the clock while playing)
    [[nodiscard]] Tick endTick() const noexcept; // end of the last event

    void setMuted(int trackNumber, bool muted); // overrides the track's own mute flag
    void setSolo(int trackNumber, bool solo);   // when any track is soloed, only soloed tracks play
    [[nodiscard]] bool isAudible(int trackNumber) const;

    /// Schedules everything up to clock.now() + lookAhead. Call every few milliseconds while
    /// playing (a 10-20 ms timer is fine). Returns false once playback has stopped by itself.
    bool pump();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Offline rendering: every message of the song between `from` and `thru` (default: the end),
/// with times in seconds from `from`, as the Transport would produce them without a clock.
[[nodiscard]] std::vector<MidiMessage> render(const Song& song, const PlaybackOptions& options = {}, Tick from = 0,
                                              std::optional<Tick> thru = std::nullopt);

} // namespace sseq::engine
