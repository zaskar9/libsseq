// libsseq: Standard MIDI File reader and writer with Cakewalk's mapping
// (TTSSEQ: SeqLoadMidiFile, SeqSaveMidiFile).
#pragma once

#include "sequence.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sseq::smf {

struct ReadOptions {
    /// Cakewalk takes every Program Change, CC7 and CC10 at tick 0 of a track as the stream's
    /// patch, volume and pan (the last one wins, whatever the channel) and drops them from the
    /// stream. On a track that mixes channels (a format 0 file, say) this loses the other
    /// channels' programs; Cakewalk Apprentice 1.04 does it anyway. libsseq does it only for a
    /// track whose events are all on one channel, where nothing is lost, unless this is set.
    bool absorbParametersOnMixedTracks = false;
};

/// Read a format 0 or 1 SMF into a Sequence, applying Cakewalk's import rules: Program Change /
/// CC7 / CC10 at tick 0 become the stream's patch / volume / pan (see ReadOptions); a track whose
/// events are all on one channel gets that channel as its forced channel, a mixed track none; Text
/// meta in the first track becomes a marker, in other tracks a Text event; Marker meta a marker,
/// Cue meta a SMPTE-locked marker; Port meta the stream port; tempo / time signature / key
/// signature fill the maps; note-offs pair with the most recent open note of the key. The file's
/// resolution is kept (Cakewalk rescales to its own timebase, truncating ticks).
[[nodiscard]] Sequence read(const std::vector<std::uint8_t>& bytes, const ReadOptions& options = {});
[[nodiscard]] Sequence readFile(const std::string& path, const ReadOptions& options = {});

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
