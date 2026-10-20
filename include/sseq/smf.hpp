// libsseq: Standard MIDI File reader and writer with the Cakewalk Apprentice mapping
// (TTSSEQ: SeqLoadMidiFile, SeqSaveMidiFile).
#pragma once

#include "song.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sseq::smf {

/// Read a format 0 or 1 SMF into a Song, applying the Apprentice import rules:
/// Program Change / CC7 / CC10 at time 0 become track patch / volume / pan, Text meta
/// becomes a Text event, Marker meta a marker, Cue meta a SMPTE-locked marker,
/// Port meta the track port, tempo / time signature / key signature fill the maps.
[[nodiscard]] Song read(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Song readFile(const std::string& path);

/// Write a format 1 SMF the way Apprentice does: track 0 with TimeSig, KeySig, Tempo
/// and markers; one track per non-empty song track with Port, TrackName, patch,
/// CC7, CC10, Text meta and channel events; Key+ baked into pitches; note-offs as
/// velocity-0 note-ons with running status.
[[nodiscard]] std::vector<std::uint8_t> write(const Song& song);
void writeFile(const Song& song, const std::string& path);

[[nodiscard]] std::string dump(const std::vector<std::uint8_t>& bytes);

} // namespace sseq::smf
