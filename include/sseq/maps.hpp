// libsseq: tempo map, meter map, key map, markers and time conversions
// (TTSSEQ: SeqGetTempo*, SeqGetMeter*, SeqMBT2Tick, SeqTick2MBT, SeqTick2Frame, ...).
#pragma once

#include "types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sseq {

struct TempoEntry {
    Tick time = 0;
    int bpm100 = 12000;                      // beats per minute x 100 (120.00 bpm = 12000)
    std::array<std::uint8_t, 4> reserved1{}; // file bytes between time and tempo (always 0 seen)
    std::array<std::uint8_t, 8> reserved2{}; // old TEMPO chunk: cached double usec/tick; NTEMPO: 0

    [[nodiscard]] double bpm() const noexcept { return bpm100 / 100.0; }
};

/// Tempo map: sorted by time, first entry at time 0.
class TempoMap {
public:
    std::vector<TempoEntry> entries{TempoEntry{}};
    int ppq = 120; // set by the owning Sequence; needed for seconds conversions

    [[nodiscard]] const TempoEntry& at(Tick time) const noexcept; // entry in force at `time`
    void set(Tick time, int bpm100);                              // insert or replace
    void remove(Tick time);
    [[nodiscard]] double tickToSeconds(Tick tick) const noexcept;
    [[nodiscard]] Tick secondsToTick(double seconds) const noexcept;
    [[nodiscard]] double microsecondsPerTick(const TempoEntry& e) const noexcept;
};

struct MeterEntry {
    int measure = 1; // 1-based measure where the meter takes effect
    int numerator = 4;
    int denominatorLog2 = 2;                 // 2 -> quarter note
    std::array<std::uint8_t, 4> reserved1{}; // METER chunk bytes before the measure
    std::array<std::uint8_t, 4> reserved2{}; // METER chunk bytes after: cached ticks/beat, ticks/measure in old files

    [[nodiscard]] int denominator() const noexcept { return 1 << denominatorLog2; }
};

struct KeyEntry {
    int measure = 1;
    int alterations = 0; // -7..7, negative = flats
};

/// Meter map: sorted by measure, first entry at measure 1.
class MeterMap {
public:
    std::vector<MeterEntry> entries{MeterEntry{}};
    int ppq = 120;

    [[nodiscard]] const MeterEntry& atMeasure(int measure) const noexcept;
    [[nodiscard]] Tick beatTicks(const MeterEntry& m) const noexcept { return Tick{ppq} * 4 / m.denominator(); }
    [[nodiscard]] Tick measureTicks(const MeterEntry& m) const noexcept { return beatTicks(m) * m.numerator; }
    [[nodiscard]] Tick measureStart(int measure) const noexcept; // tick at which `measure` (1-based) starts
    [[nodiscard]] MBT tickToMBT(Tick tick) const noexcept;
    [[nodiscard]] Tick mbtToTick(const MBT& mbt) const noexcept;
    [[nodiscard]] int measureAt(Tick tick) const noexcept { return tickToMBT(tick).measure; }
    [[nodiscard]] Tick nextMeasure(Tick tick) const noexcept;
    [[nodiscard]] Tick prevMeasure(Tick tick) const noexcept;
};

[[nodiscard]] std::string formatMBT(const MBT& mbt); // "12:3:045"
[[nodiscard]] bool parseMBT(const std::string& s, MBT& out);

/// SMPTE conversions. Frame counts are absolute frames since 00:00:00:00.
struct Timecode {
    FrameRate rate = FrameRate::Fps30;
    std::int64_t offsetFrames = 0; // SMPTE/MTC offset (TIMEFMT chunk)

    [[nodiscard]] int nominalFps() const noexcept; // 24, 25, 30
    [[nodiscard]] bool dropFrame() const noexcept { return rate == FrameRate::Fps30Drop; }
    [[nodiscard]] std::int64_t secondsToFrames(double seconds) const noexcept; // nearest frame
    [[nodiscard]] double framesToSeconds(std::int64_t frames) const noexcept;
    [[nodiscard]] HMSF framesToHMSF(std::int64_t frames) const noexcept;
    [[nodiscard]] std::int64_t hmsfToFrames(const HMSF& t) const noexcept;
};

[[nodiscard]] std::string formatHMSF(const HMSF& t); // "00:00:05:10"
[[nodiscard]] bool parseHMSF(const std::string& s, HMSF& out);

struct Marker {
    Tick time = 0; // ticks (also for SMPTE-locked markers; see docs)
    bool smpteLocked = false;
    std::string name;
    std::uint8_t reserved0 = 0;              // byte after the lock flag
    std::array<std::uint8_t, 5> reserved1{}; // bytes between time and name length
};

} // namespace sseq
