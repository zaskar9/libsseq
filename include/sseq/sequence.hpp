// libsseq: the sequence model (TTSSEQ "sequence" with its "streams").
#pragma once

#include "event.hpp"
#include "maps.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sseq {

/// A stream (TTSSEQ "stream"). `number` is the 0-based slot in the file; the UI shows number + 1.
struct Stream {
    int number = 0;
    std::string name; // full name (TRKNAME chunk); the TRACK chunk holds it split into two 15-character fields
    std::optional<std::uint8_t> channel; // forced output channel, nullopt = none (255 in file)
    std::int8_t keyPlus = 0;
    std::int8_t velPlus = 0;
    std::uint8_t port = 0;
    bool selected = false;       // TRACK flags bit 0
    bool muted = false;          // bit 1
    bool loop = false;           // bit 2, set when reps > 1
    std::optional<int> patch;    // TRKPATCH
    std::optional<int> reps;     // TRKREPS
    std::optional<int> timePlus; // TRKOFFS (ticks, signed)
    std::optional<int> volume;   // TRKVOL
    std::optional<int> pan;      // TRKVOL (stored together with volume)
    std::vector<Event> events;   // sorted by time

    [[nodiscard]] bool hasStringEvents() const noexcept;
    [[nodiscard]] Tick endTime() const noexcept; // end of the last event (note end for notes)
};

/// Global variables (VARS chunk, 89 bytes). The raw bytes are kept so that fields without
/// a known meaning survive a round trip; the accessors cover the known offsets.
struct Vars {
    static constexpr std::size_t kSize = 89;
    std::array<std::uint8_t, kSize> raw{};

    Vars();

    [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept;
    void setU32(std::size_t off, std::uint32_t v) noexcept;
    [[nodiscard]] bool flag(std::size_t off) const noexcept { return raw[off] != 0; }
    void setFlag(std::size_t off, bool b) noexcept { raw[off] = b ? 1 : 0; }

    [[nodiscard]] Tick now() const noexcept { return u32(0); }
    void setNow(Tick t) noexcept { setU32(0, static_cast<std::uint32_t>(t)); }
    [[nodiscard]] Tick from() const noexcept { return u32(4); }
    void setFrom(Tick t) noexcept { setU32(4, static_cast<std::uint32_t>(t)); }
    [[nodiscard]] Tick thru() const noexcept { return u32(8); }
    void setThru(Tick t) noexcept { setU32(8, static_cast<std::uint32_t>(t)); }
    [[nodiscard]] int keySignature() const noexcept { return static_cast<std::int8_t>(raw[12]); }
    [[nodiscard]] int clockSource() const noexcept {
        return raw[13];
    } // 0 internal, 1 MIDI sync, 2 SMPTE/MTC (per QWrk)
    [[nodiscard]] int autoSaveMinutes() const noexcept { return raw[14]; }
    [[nodiscard]] int sppLocateDelay() const noexcept { return raw[15]; }
    void setSppLocateDelay(int v) noexcept { raw[15] = static_cast<std::uint8_t>(v); }
    [[nodiscard]] bool zeroControllers() const noexcept { return flag(17); }
    void setZeroControllers(bool b) noexcept { setFlag(17, b); }
    [[nodiscard]] bool sendSpp() const noexcept { return flag(18); }
    void setSendSpp(bool b) noexcept { setFlag(18, b); }
    [[nodiscard]] bool useContinue() const noexcept { return flag(19); }
    void setUseContinue(bool b) noexcept { setFlag(19, b); }
    [[nodiscard]] bool patchSearchBack() const noexcept { return flag(20); }
    void setPatchSearchBack(bool b) noexcept { setFlag(20, b); }
    [[nodiscard]] bool autoStop() const noexcept { return flag(21); }
    [[nodiscard]] Tick stopTime() const noexcept { return u32(22); }
    [[nodiscard]] bool autoRewind() const noexcept { return flag(26); }
    [[nodiscard]] Tick rewindTime() const noexcept { return u32(27); }
    [[nodiscard]] bool metronomePlayback() const noexcept { return flag(31); }
    void setMetronomePlayback(bool b) noexcept { setFlag(31, b); }
    [[nodiscard]] bool metronomeRecording() const noexcept { return flag(32); }
    void setMetronomeRecording(bool b) noexcept { setFlag(32, b); }
    [[nodiscard]] bool metronomeAccent() const noexcept { return flag(33); }
    void setMetronomeAccent(bool b) noexcept { setFlag(33, b); }
    [[nodiscard]] int countIn() const noexcept { return raw[34]; }
    void setCountIn(int v) noexcept { raw[34] = static_cast<std::uint8_t>(v); }
    [[nodiscard]] bool thruOn() const noexcept { return flag(37); }
    [[nodiscard]] bool autoRestart() const noexcept { return flag(57); }
    [[nodiscard]] bool punchEnabled() const noexcept { return flag(64); }
    [[nodiscard]] Tick punchIn() const noexcept { return u32(65); }
    [[nodiscard]] Tick punchOut() const noexcept { return u32(69); }
    [[nodiscard]] Tick endAllTime() const noexcept { return u32(73); }
    [[nodiscard]] bool transmitMidiSync() const noexcept { return raw[77] == 255; }
    void setTransmitMidiSync(bool b) noexcept { raw[77] = b ? 255 : 254; }
};

/// Metronome MIDI note (chunk 17).
struct Metronome {
    int port = 0;    // 0-based
    int channel = 9; // 0-based (9 = MIDI channel 10)
    int key = 42;
    int velocity = 110;
    int duration = 1;
    std::array<std::uint16_t, 2> reserved{};
};

/// MIDI Thru settings (THRU chunk, 12 bytes).
struct Thru {
    int mode = 2; // 1 Manual, 2 Auto (0 presumably None)
    std::uint8_t reserved1 = 0;
    int port = 0;
    int channel = -1; // -1 = none, 0..15
    int keyPlus = 0;
    int velPlus = 0;
    std::array<std::uint8_t, 6> reserved2{0xff, 0xff, 0, 0, 0, 0};
};

/// System exclusive bank (SYSEX chunk 6).
struct SysxBank {
    int bank = 0;
    bool autoSend = false;
    std::string name;
    std::vector<std::uint8_t> data;
};

/// Names of the string event kinds (STRTAB chunk).
struct StrTabEntry {
    std::string name;
    std::uint8_t code = 0;
};

/// A chunk the library does not model; kept verbatim.
struct UnknownChunk {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> data;
};

/// One entry of the file layout, recorded on read so that a write can reproduce the
/// original chunk order. `index` is the stream number (stream-scoped chunks) or the index
/// into the sysex-bank or unknown-chunk list; -1 otherwise.
struct LayoutEntry {
    std::uint8_t type = 0;
    int index = -1;
};

struct Sequence {
    // Header
    int versionMajor = 2;
    int versionMinor = 0;

    // Global data
    int ppq = 120;
    Vars vars;
    std::vector<StrTabEntry> strTab{{"MCICmd", kStrCodeMci}, {"Wave", kStrCodeWave}, {"Text", kStrCodeText}};
    TempoMap tempoMap;
    bool legacyTempoChunk = false; // true when read from an old TEMPO (4) chunk
    MeterMap meterMap;
    std::vector<KeyEntry> keyMap{KeyEntry{}};
    Timecode timecode;
    Metronome metronome;
    Thru thru;
    std::vector<Marker> markers;
    std::vector<Stream> streams;                       // in file order
    std::optional<std::vector<std::uint8_t>> comments; // raw bytes of the COMMENTS chunk body
    std::vector<SysxBank> sysxBanks;
    std::vector<UnknownChunk> unknownChunks;
    std::vector<LayoutEntry> layout; // chunk order as read; empty for a new sequence

    Sequence();

    [[nodiscard]] Stream* findStream(int number) noexcept;
    [[nodiscard]] const Stream* findStream(int number) const noexcept;
    [[nodiscard]] Stream& stream(int number); // creates the stream if missing, keeps file order
    [[nodiscard]] std::string commentsText() const;
    void setComments(const std::string& text);
    [[nodiscard]] Tick endTime() const noexcept; // last event end over all streams

    /// Keep the maps' ppq in sync after changing `ppq`.
    void syncPpq() noexcept {
        tempoMap.ppq = ppq;
        meterMap.ppq = ppq;
    }
};

} // namespace sseq
