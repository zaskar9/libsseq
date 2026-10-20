// libsseq: types shared by the whole library.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace sseq {

/// Time in ticks (pulses). Files store 24- or 32-bit values; the model uses 64 bits.
using Tick = std::int64_t;

/// Error raised by file readers and writers and by invalid operations.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

/// Measure:Beat:Tick position. Measure and beat are 1-based as in the Cakewalk UI.
struct MBT {
    int measure = 1;
    int beat = 1;
    int tick = 0;
};

/// SMPTE time (hours:minutes:seconds:frames).
struct HMSF {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
};

/// SMPTE frame rates as stored in the file (TIMEFMT chunk).
enum class FrameRate : std::uint16_t {
    Fps24 = 24,
    Fps25 = 25,
    Fps30Drop = 29, // 30 fps drop-frame; value as presumed from the dialog order
    Fps30 = 30,
};

} // namespace sseq
