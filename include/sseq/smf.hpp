// libsseq: Standard MIDI File reader and writer with Cakewalk's mapping
// (TTSSEQ: SeqLoadMidiFile, SeqSaveMidiFile).
#pragma once

#include "sequence.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sseq::smf {

/// Read a format 0 or 1 SMF into a Sequence, applying Cakewalk's import rules:
/// Program Change / CC7 / CC10 at time 0 become stream patch / volume / pan, Text meta
/// becomes a Text event, Marker meta a marker, Cue meta a SMPTE-locked marker,
/// Port meta the stream port, tempo / time signature / key signature fill the maps.
[[nodiscard]] Sequence read(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Sequence readFile(const std::string& path);

struct WriteOptions {
    /// Write the "MIDI Port" meta event (type 0x21) at the start of every track, as Cakewalk
    /// does. The event is a Cakewalk extension, not part of the SMF 1.0 specification; readers
    /// must skip unknown meta types, and some (FluidSynth, for one) report it. Turn it off for
    /// files meant for other programs.
    bool portMeta = true;
};

/// Write a format 1 SMF the way Cakewalk does: track 0 with TimeSig, KeySig, Tempo
/// and markers; one track per non-empty stream with Port, TrackName, patch,
/// CC7, CC10, Text meta and channel events; Key+ baked into pitches; note-offs as
/// velocity-0 note-ons with running status.
[[nodiscard]] std::vector<std::uint8_t> write(const Sequence& sequence, const WriteOptions& options = {});
void writeFile(const Sequence& sequence, const std::string& path, const WriteOptions& options = {});

[[nodiscard]] std::string dump(const std::vector<std::uint8_t>& bytes);

} // namespace sseq::smf
