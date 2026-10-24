// libsseq: text forms of events and values as Cakewalk's views show them (Event list, Track/Measure
// view): key names, durations, event kind names, General MIDI program names.
#pragma once

#include "event.hpp"
#include "types.hpp"

#include <string>
#include <string_view>

namespace sseq::display {

/// Key name as Cakewalk shows it: letter, sharp or space, octave, with C 5 = 60: "C 5", "G#4",
/// "C 0" = 0, "G 10" = 127. Values outside 0..127 are clamped.
[[nodiscard]] std::string keyName(int key);
/// Parses a key name ("C 5", "C5", "G#4", "Bb3" also accepted); false when the text is not a key.
[[nodiscard]] bool parseKeyName(std::string_view text, int& key);

/// A note duration as the Event list shows it: plain ticks below one quarter note ("50"),
/// otherwise quarters and three-digit ticks ("1:080"). The unit is the quarter note (ppq), not
/// the meter's beat.
[[nodiscard]] std::string durationText(Tick ticks, int ppq);
[[nodiscard]] bool parseDurationText(std::string_view text, int ppq, Tick& ticks);

/// The Event list's kind names: "Note", "KeyAft", "ChanAft", "Contrl", "Patch", "Wheel", "Sysx",
/// "Text", "MCICmd", "Wave".
[[nodiscard]] std::string_view kindName(EventKind kind);

/// General MIDI level 1 program names (0-based), from the MIDI specification's list.
[[nodiscard]] std::string_view generalMidiProgramName(int program);
/// General MIDI percussion key names for channel 10 (35..81); empty outside that range.
[[nodiscard]] std::string_view generalMidiPercussionName(int key);

/// The Event list's Chn column: 1-based channel, "n/a" for string and sysex events.
[[nodiscard]] std::string channelText(const Event& event);

/// The Event list's Values column for an event: "C 5  100  1:080", "7  100", "Reed Organ",
/// "-8192", "0" (sysex bank), or the text of a string event. Fields are separated by two spaces.
[[nodiscard]] std::string valuesText(const Event& event, int ppq);

} // namespace sseq::display
