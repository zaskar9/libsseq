// Test corpus: song definitions shared by the generator (gencorpus) and the corpus tests.
//
// The synthetic songs exercise every feature of the model and the formats. The realistic songs
// are arrangements of public-domain music. Each song is written by libsseq, loaded into
// Cakewalk Apprentice 1.04 and saved again by that program; the saved files are the test
// oracle (see tests/data/README.md).
#pragma once

#include "sseq/song.hpp"

#include <string>
#include <vector>

namespace sseq::corpus {

struct CorpusSong {
    std::string name;        // 8.3-compatible base name (upper case, at most 8 characters)
    std::string description; // one line, also used for the data README
    Song song;
};

/// All corpus songs, in a fixed order.
[[nodiscard]] std::vector<CorpusSong> allSongs();

/// Adds a melody written in a small text notation to `track`, starting at `start`, and returns
/// the tick after the last note. Tokens are separated by spaces:
///   pitch: letter A..G, optional # or b, octave digit (Cakewalk numbering: C5 = 60)
///   duration: w h q e s (whole .. sixteenth), optional '.' for dotted, optional 't' for triplet
///   rest: r followed by a duration
/// Example: "E5q E5q F5q G5q | G5q F5q E5q D5q" (bars '|' are ignored).
Tick addMelody(Track& track, Tick start, const std::string& notation, std::uint8_t channel, int velocity, int ppq = 120,
               double gate = 1.0);

/// Parses a pitch name in Cakewalk numbering ("C5" = 60, "A#3", "Bb4").
[[nodiscard]] int pitch(const std::string& name);

} // namespace sseq::corpus
