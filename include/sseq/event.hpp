// libsseq: events (TTSSEQ "events" inside a "stream").
#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sseq {

/// Event kinds. The first seven are MIDI; the last three are Cakewalk's "special" string events.
enum class EventKind : std::uint8_t {
    Note,              // data1 = key, data2 = velocity, duration in ticks
    KeyAftertouch,     // data1 = key, data2 = pressure
    Controller,        // data1 = controller number, data2 = value
    Patch,             // data1 = program number
    ChannelAftertouch, // data1 = pressure
    PitchWheel,        // data1 = value -8192..8191
    Sysex,             // data1 = sysex bank index
    MciCommand,        // payload = command string
    Wave,              // payload = wave data (encoding not yet verified)
    Text,              // payload = text
};

/// The code Cakewalk uses for a string event kind in the STRTAB chunk and as the
/// status byte of such events in a variable-length stream.
inline constexpr std::uint8_t kStrCodeMci = 1;
inline constexpr std::uint8_t kStrCodeWave = 2;
inline constexpr std::uint8_t kStrCodeText = 3;

struct Event {
    Tick time = 0;
    EventKind kind = EventKind::Note;
    std::uint8_t channel = 0; // 0..15, MIDI channel events only
    int data1 = 0;
    int data2 = 0;
    Tick duration = 0;                 // notes only
    std::vector<std::uint8_t> payload; // string events only

    [[nodiscard]] bool isMidi() const noexcept { return kind <= EventKind::Sysex; }
    [[nodiscard]] bool isString() const noexcept { return kind >= EventKind::MciCommand; }
    [[nodiscard]] std::string text() const { return {payload.begin(), payload.end()}; }
    /// End of the event: start plus duration for notes, the start itself otherwise.
    [[nodiscard]] Tick end() const noexcept { return kind == EventKind::Note ? time + duration : time; }

    [[nodiscard]] static Event note(Tick time, std::uint8_t channel, int key, int velocity, Tick duration);
    [[nodiscard]] static Event keyAftertouch(Tick time, std::uint8_t channel, int key, int pressure);
    [[nodiscard]] static Event controller(Tick time, std::uint8_t channel, int number, int value);
    [[nodiscard]] static Event patch(Tick time, std::uint8_t channel, int program);
    [[nodiscard]] static Event channelAftertouch(Tick time, std::uint8_t channel, int pressure);
    [[nodiscard]] static Event pitchWheel(Tick time, std::uint8_t channel, int value);
    [[nodiscard]] static Event sysex(Tick time, int bank);
    [[nodiscard]] static Event textEvent(Tick time, const std::string& text);
    [[nodiscard]] static Event mciCommand(Tick time, const std::string& command);
};

/// Stable ordering by time; events at the same time keep their relative order.
void sortEvents(std::vector<Event>& events);

} // namespace sseq
