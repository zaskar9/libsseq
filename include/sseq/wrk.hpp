// libsseq: Cakewalk .WRK (version 2.0) reader and writer
// (TTSSEQ: SeqLoadCakewalkFile, SeqSaveCakewalkFile).
#pragma once

#include "song.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sseq::wrk {

/// Chunk type codes.
enum ChunkType : std::uint8_t {
    TRACK = 1,
    STREAM = 2,
    VARS = 3,
    TEMPO = 4,
    METER = 5,
    SYSEX = 6,
    MEMRGN = 7,
    COMMENTS = 8,
    TRKOFFS = 9,
    TIMEBASE = 10,
    TIMEFMT = 11,
    TRKREPS = 12,
    TRKPATCH = 14,
    NTEMPO = 15,
    THRU = 16,
    METRONOME = 17,
    STRSTREAM = 18,
    TRKVOL = 19,
    SYSEX2 = 20,
    MARKERS = 21,
    STRTAB = 22,
    METERKEY = 23,
    TRKNAME = 24,
    VARIABLE = 26,
    NTRKOFS = 27,
    TRKBANK = 30,
    NTRACK = 36,
    NSYSEX = 44,
    NSTREAM = 45,
    SGMNT = 49,
    SOFTVER = 74,
    END = 255,
};

[[nodiscard]] const char* chunkName(std::uint8_t type) noexcept;

enum class WriteMode {
    /// Reproduce the chunk order recorded in `song.layout` (byte-identical round trip
    /// for files read by this library). Falls back to Normalize when the layout is empty.
    Preserve,
    /// Write the chunks in the order Cakewalk Apprentice 1.04 uses, dropping legacy
    /// chunks (old TEMPO becomes NTEMPO; MEMRGN and unknown chunks are omitted).
    Normalize,
};

[[nodiscard]] Song read(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Song readFile(const std::string& path);

[[nodiscard]] std::vector<std::uint8_t> write(const Song& song, WriteMode mode = WriteMode::Preserve);
void writeFile(const Song& song, const std::string& path, WriteMode mode = WriteMode::Preserve);

/// Human-readable dump of the chunk structure (for sseqtool).
[[nodiscard]] std::string dump(const std::vector<std::uint8_t>& bytes);

} // namespace sseq::wrk
