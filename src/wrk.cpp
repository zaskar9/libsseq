// libsseq: Cakewalk .WRK version 2.0 reader and writer.
//
// Layouts come from the specification documents produced by inspecting the files and by
// observing Cakewalk Apprentice 1.04 (differential saves). See DESIGN.md and the
// analysis documents. All integers are little-endian.
#include "sseq/wrk.hpp"
#include "bytes.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ranges>
#include <sstream>
#include <string_view>

namespace sseq::wrk {

using bytes::Reader;
using bytes::Writer;

namespace {

constexpr std::string_view kMagic = "CAKEWALK";
constexpr std::size_t kHeaderSize = 11; // magic, 0x1A, minor, major

constexpr bool isTrackChunk(const std::uint8_t type) noexcept {
    return type == TRACK || type == TRKNAME || type == STREAM || type == STRSTREAM || type == TRKPATCH ||
           type == TRKREPS || type == TRKOFFS || type == TRKVOL;
}

} // namespace

const char* chunkName(const std::uint8_t type) noexcept {
    switch (type) {
    case TRACK:
        return "TRACK";
    case STREAM:
        return "STREAM";
    case VARS:
        return "VARS";
    case TEMPO:
        return "TEMPO";
    case METER:
        return "METER";
    case SYSEX:
        return "SYSEX";
    case MEMRGN:
        return "MEMRGN";
    case COMMENTS:
        return "COMMENTS";
    case TRKOFFS:
        return "TRKOFFS";
    case TIMEBASE:
        return "TIMEBASE";
    case TIMEFMT:
        return "TIMEFMT";
    case TRKREPS:
        return "TRKREPS";
    case TRKPATCH:
        return "TRKPATCH";
    case NTEMPO:
        return "NTEMPO";
    case THRU:
        return "THRU";
    case METRONOME:
        return "METRONOME";
    case STRSTREAM:
        return "STRSTREAM";
    case TRKVOL:
        return "TRKVOL";
    case SYSEX2:
        return "SYSEX2";
    case MARKERS:
        return "MARKERS";
    case STRTAB:
        return "STRTAB";
    case METERKEY:
        return "METERKEY";
    case TRKNAME:
        return "TRKNAME";
    case VARIABLE:
        return "VARIABLE";
    case NTRKOFS:
        return "NTRKOFS";
    case TRKBANK:
        return "TRKBANK";
    case NTRACK:
        return "NTRACK";
    case NSYSEX:
        return "NSYSEX";
    case NSTREAM:
        return "NSTREAM";
    case SGMNT:
        return "SGMNT";
    case SOFTVER:
        return "SOFTVER";
    case END:
        return "END";
    default:
        return "UNKNOWN";
    }
}

// ---------------------------------------------------------------- reading

namespace {

/// Reads the body of a MIDI event after its status byte. In a fixed-length stream (chunk 2)
/// every event has the same three data bytes; in a variable-length stream (chunk 18) only
/// the bytes the message needs are present.
Event readMidiEvent(Reader& r, const Tick time, const std::uint8_t status, const bool fixed) {
    Event e;
    e.time = time;
    e.channel = status & 0x0fU;
    std::uint8_t d1 = 0;
    std::uint8_t d2 = 0;
    std::uint16_t dur = 0;
    if (fixed) {
        d1 = r.u8();
        d2 = r.u8();
        dur = r.u16();
    }
    switch (status & 0xf0U) {
    case 0x90:
        if (!fixed) {
            d1 = r.u8();
            d2 = r.u8();
            dur = r.u16();
        }
        e.kind = EventKind::Note;
        e.data1 = d1;
        e.data2 = d2;
        e.duration = dur;
        break;
    case 0xa0:
    case 0xb0:
        if (!fixed) {
            d1 = r.u8();
            d2 = r.u8();
        }
        e.kind = (status & 0xf0U) == 0xa0 ? EventKind::KeyAftertouch : EventKind::Controller;
        e.data1 = d1;
        e.data2 = d2;
        break;
    case 0xc0:
    case 0xd0:
        if (!fixed) {
            d1 = r.u8();
        }
        e.kind = (status & 0xf0U) == 0xc0 ? EventKind::Patch : EventKind::ChannelAftertouch;
        e.data1 = d1;
        break;
    case 0xe0:
        if (!fixed) {
            d1 = r.u8();
            d2 = r.u8();
        }
        e.kind = EventKind::PitchWheel;
        e.data1 = (((d2 & 0x7f) << 7) | (d1 & 0x7f)) - 8192;
        break;
    case 0xf0:
        if (!fixed) {
            d1 = r.u8();
        }
        e.kind = EventKind::Sysex;
        e.data1 = d1;
        e.channel = 0;
        break;
    default:
        throw Error("unknown event status " + std::to_string(status));
    }
    return e;
}

void readFixedStream(Reader& r, Stream& t) {
    const int count = r.u16();
    t.events.reserve(t.events.size() + static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const Tick time = r.u24();
        const std::uint8_t status = r.u8();
        if (status < 0x90) {
            throw Error("string event in a fixed-length stream");
        }
        t.events.push_back(readMidiEvent(r, time, status, true));
    }
}

void readVarStream(Reader& r, Stream& t, const Sequence& sequence) {
    const std::uint32_t count = r.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        const Tick time = r.u24();
        const std::uint8_t status = r.u8();
        if (status >= 0x90) {
            t.events.push_back(readMidiEvent(r, time, status, false));
            continue;
        }
        Event e;
        e.time = time;
        switch (status) {
        case kStrCodeMci:
            e.kind = EventKind::MciCommand;
            break;
        case kStrCodeWave:
            e.kind = EventKind::Wave;
            break;
        case kStrCodeText:
            e.kind = EventKind::Text;
            break;
        default: {
            const bool known =
                std::ranges::any_of(sequence.strTab, [status](const StrTabEntry& s) { return s.code == status; });
            throw Error(std::string(known ? "unsupported" : "unknown") + " string event code " +
                        std::to_string(status));
        }
        }
        const std::uint32_t len = r.u32();
        e.payload = r.bytes(len);
        t.events.push_back(std::move(e));
    }
}

} // namespace

Sequence read(const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    if (r.remaining() < kHeaderSize || r.str(kMagic.size()) != kMagic) {
        throw Error("not a Cakewalk .WRK file");
    }
    r.skip(1); // 0x1A
    Sequence sequence;
    sequence.versionMinor = r.u8();
    sequence.versionMajor = r.u8();
    sequence.layout.clear();
    bool haveTempo = false;
    bool haveMeter = false;
    bool haveKey = false;

    while (!r.atEnd()) {
        const std::uint8_t type = r.u8();
        if (type == END) {
            break;
        }
        const std::uint32_t len = r.u32();
        if (len > r.remaining()) {
            throw Error("chunk " + std::string(chunkName(type)) + " runs past the end of the file");
        }
        Reader c(r.ptr(), len);
        r.skip(len);
        LayoutEntry le{type, -1};

        switch (type) {
        case TIMEBASE:
            sequence.ppq = c.u16();
            sequence.syncPpq();
            break;
        case VARS:
            if (len != Vars::kSize) {
                le.index = static_cast<int>(sequence.unknownChunks.size());
                sequence.unknownChunks.push_back({type, c.bytes(len)});
                break;
            }
            c.array(sequence.vars.raw);
            break;
        case STRTAB: {
            sequence.strTab.clear();
            const int n = c.u16();
            for (int i = 0; i < n; ++i) {
                StrTabEntry s;
                s.name = c.pstr();
                s.code = c.u8();
                sequence.strTab.push_back(std::move(s));
            }
            break;
        }
        case TEMPO:
        case NTEMPO: {
            if (!haveTempo) {
                sequence.tempoMap.entries.clear();
                haveTempo = true;
            }
            sequence.legacyTempoChunk = (type == TEMPO);
            const int n = c.u16();
            for (int i = 0; i < n; ++i) {
                TempoEntry e;
                e.time = c.u32();
                c.array(e.reserved1);
                const int raw = c.u16();
                e.bpm100 = type == TEMPO ? raw * 100 : raw;
                c.array(e.reserved2);
                sequence.tempoMap.entries.push_back(e);
            }
            break;
        }
        case METER: {
            if (!haveMeter) {
                sequence.meterMap.entries.clear();
                haveMeter = true;
            }
            const int n = c.u16();
            for (int i = 0; i < n; ++i) {
                MeterEntry m;
                c.array(m.reserved1);
                m.measure = c.u16();
                m.numerator = c.u8();
                m.denominatorLog2 = c.u8();
                c.array(m.reserved2);
                sequence.meterMap.entries.push_back(m);
            }
            break;
        }
        case METERKEY: {
            if (!haveKey) {
                sequence.keyMap.clear();
                haveKey = true;
            }
            // One record per measure where the meter or the key changes; the key map keeps
            // only the key changes (the writer restores the union).
            const int n = c.u16();
            for (int i = 0; i < n; ++i) {
                KeyEntry k;
                k.measure = c.u16();
                c.skip(2); // numerator and denominator, duplicated from the meter map
                k.alterations = c.s8();
                if (sequence.keyMap.empty() || sequence.keyMap.back().alterations != k.alterations) {
                    sequence.keyMap.push_back(k);
                }
            }
            if (sequence.keyMap.empty()) {
                sequence.keyMap.push_back(KeyEntry{});
            }
            break;
        }
        case TIMEFMT:
            sequence.timecode.rate = static_cast<FrameRate>(c.u16());
            sequence.timecode.offsetFrames = c.u32();
            break;
        case METRONOME: {
            Metronome& m = sequence.metronome;
            m.port = c.u16();
            m.channel = c.u16();
            m.key = c.u16();
            m.velocity = c.u16();
            m.duration = c.u16();
            m.reserved[0] = c.u16();
            m.reserved[1] = c.u16();
            break;
        }
        case THRU: {
            Thru& t = sequence.thru;
            t.mode = c.u8();
            t.reserved1 = c.u8();
            t.port = c.u8();
            t.channel = c.s8();
            t.keyPlus = c.s8();
            t.velPlus = c.s8();
            c.array(t.reserved2);
            break;
        }
        case MARKERS: {
            sequence.markers.clear();
            const std::uint32_t n = c.u32();
            for (std::uint32_t i = 0; i < n; ++i) {
                Marker m;
                m.smpteLocked = c.u8() != 0;
                m.reserved0 = c.u8();
                m.time = c.u24();
                c.array(m.reserved1);
                m.name = c.pstr();
                sequence.markers.push_back(std::move(m));
            }
            break;
        }
        case TRACK: {
            const int num = c.u16();
            Stream& t = sequence.stream(num);
            le.index = num;
            // Two 15-character fields: the first 15 characters and the next 15. TRKNAME, when
            // present, carries the full name and overrides this.
            t.name = c.pstr();
            t.name += c.pstr();
            const std::uint8_t ch = c.u8();
            t.channel = ch == 255 ? std::nullopt : std::optional<std::uint8_t>(ch);
            t.keyPlus = c.s8();
            t.velPlus = c.s8();
            t.port = c.u8();
            const std::uint8_t flags = c.u8();
            t.selected = (flags & 1U) != 0;
            t.muted = (flags & 2U) != 0;
            t.loop = (flags & 4U) != 0;
            break;
        }
        case TRKNAME: {
            const int num = c.u16();
            le.index = num;
            sequence.stream(num).name = c.pstr();
            break;
        }
        case STREAM: {
            const int num = c.u16();
            le.index = num;
            readFixedStream(c, sequence.stream(num));
            break;
        }
        case STRSTREAM: {
            const int num = c.u16();
            le.index = num;
            readVarStream(c, sequence.stream(num), sequence);
            break;
        }
        case TRKPATCH: {
            const int num = c.u16();
            le.index = num;
            sequence.stream(num).patch = c.u16();
            break;
        }
        case TRKREPS: {
            const int num = c.u16();
            le.index = num;
            sequence.stream(num).reps = c.u16();
            break;
        }
        case TRKOFFS: {
            const int num = c.u16();
            le.index = num;
            sequence.stream(num).timePlus = c.s16();
            break;
        }
        case TRKVOL: {
            const int num = c.u16();
            le.index = num;
            Stream& t = sequence.stream(num);
            const int volume = c.s16(); // -1 = none (the value the UI uses for "none")
            const int pan = c.s16();
            t.volume = volume < 0 ? std::nullopt : std::optional<int>(volume);
            t.pan = pan < 0 ? std::nullopt : std::optional<int>(pan);
            break;
        }
        case COMMENTS: {
            const std::size_t n = c.u16();
            sequence.comments = c.bytes(n);
            break;
        }
        case SYSEX: {
            SysxBank b;
            b.bank = c.u8();
            const std::size_t n = c.u16();
            b.autoSend = c.u8() != 0;
            b.name = c.pstr();
            b.data = c.bytes(n);
            le.index = static_cast<int>(sequence.sysxBanks.size());
            sequence.sysxBanks.push_back(std::move(b));
            break;
        }
        default:
            le.index = static_cast<int>(sequence.unknownChunks.size());
            sequence.unknownChunks.push_back({type, c.bytes(len)});
            break;
        }
        sequence.layout.push_back(le);
    }
    for (Stream& t : sequence.streams) {
        sortEvents(t.events);
    }
    return sequence;
}

Sequence readFile(const std::string& path) {
    return read(bytes::readFileBytes(path));
}

// ---------------------------------------------------------------- writing

namespace {

/// Writes a chunk header and patches the length when destroyed.
class ChunkWriter {
public:
    ChunkWriter(Writer& w, const std::uint8_t type) : w_(w), lenPos_(w.size() + 1) {
        w_.u8(type);
        w_.u32(0);
    }
    ~ChunkWriter() { w_.patchU32(lenPos_, bytes::u32(w_.size() - lenPos_ - 4)); }
    ChunkWriter(const ChunkWriter&) = delete;
    ChunkWriter(ChunkWriter&&) = delete;
    ChunkWriter& operator=(const ChunkWriter&) = delete;
    ChunkWriter& operator=(ChunkWriter&&) = delete;

private:
    Writer& w_;
    std::size_t lenPos_ = 0;
};

/// STRTAB code of a string event kind (its status byte in a variable-length stream).
constexpr std::uint8_t strCode(const EventKind kind) noexcept {
    switch (kind) {
    case EventKind::MciCommand:
        return kStrCodeMci;
    case EventKind::Wave:
        return kStrCodeWave;
    default:
        return kStrCodeText;
    }
}

void writeMidiEventBody(Writer& w, const Event& e, const bool fixed) {
    std::uint8_t status = 0;
    std::uint8_t d1 = 0;
    std::uint8_t d2 = 0;
    std::uint16_t dur = 0;
    switch (e.kind) {
    case EventKind::Note:
        status = 0x90;
        d1 = bytes::u8(e.data1);
        d2 = bytes::u8(e.data2);
        dur = bytes::u16(e.duration);
        break;
    case EventKind::KeyAftertouch:
    case EventKind::Controller:
        status = e.kind == EventKind::KeyAftertouch ? 0xa0 : 0xb0;
        d1 = bytes::u8(e.data1);
        d2 = bytes::u8(e.data2);
        break;
    case EventKind::Patch:
    case EventKind::ChannelAftertouch:
        status = e.kind == EventKind::Patch ? 0xc0 : 0xd0;
        d1 = bytes::u8(e.data1);
        break;
    case EventKind::PitchWheel: {
        const int v = e.data1 + 8192;
        status = 0xe0;
        d1 = bytes::u8(v & 0x7f);
        d2 = bytes::u8((v >> 7) & 0x7f);
        break;
    }
    case EventKind::Sysex:
        status = 0xf0;
        d1 = bytes::u8(e.data1);
        break;
    default:
        throw Error("not a MIDI event");
    }
    w.u8(status | (e.kind == EventKind::Sysex ? 0U : (e.channel & 0x0fU)));
    if (fixed) {
        w.u8(d1);
        w.u8(d2);
        w.u16(dur);
        return;
    }
    switch (e.kind) {
    case EventKind::Note:
        w.u8(d1);
        w.u8(d2);
        w.u16(dur);
        break;
    case EventKind::KeyAftertouch:
    case EventKind::Controller:
    case EventKind::PitchWheel:
        w.u8(d1);
        w.u8(d2);
        break;
    default:
        w.u8(d1);
        break;
    }
}

void writeTrackChunk(Writer& w, const Stream& t) {
    const ChunkWriter c(w, TRACK);
    w.u16(bytes::u16(t.number));
    w.pstr(t.name.substr(0, 15));
    w.pstr(t.name.size() > 15 ? t.name.substr(15, 15) : std::string());
    w.u8(t.channel ? *t.channel : 255U);
    w.s8(t.keyPlus);
    w.s8(t.velPlus);
    w.u8(t.port);
    w.u8((t.selected ? 1U : 0U) | (t.muted ? 2U : 0U) | (t.loop ? 4U : 0U));
}

void writeStream(Writer& w, const Stream& stream) {
    Stream t = stream; // events are written in time order
    sortEvents(t.events);
    if (t.hasStringEvents()) {
        const ChunkWriter c(w, STRSTREAM);
        w.u16(bytes::u16(t.number));
        w.u32(bytes::u32(t.events.size()));
        for (const Event& e : t.events) {
            w.u24(bytes::u32(e.time));
            if (e.isMidi()) {
                writeMidiEventBody(w, e, false);
                continue;
            }
            w.u8(strCode(e.kind));
            w.u32(bytes::u32(e.payload.size()));
            w.bytes(e.payload);
        }
    } else {
        const ChunkWriter c(w, STREAM);
        w.u16(bytes::u16(t.number));
        w.u16(bytes::u16(t.events.size()));
        for (const Event& e : t.events) {
            w.u24(bytes::u32(e.time));
            writeMidiEventBody(w, e, true);
        }
    }
}

void writeChunk(Writer& w, const Sequence& sequence, const std::uint8_t type, const int index) {
    switch (type) {
    case TIMEBASE: {
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(sequence.ppq));
        break;
    }
    case VARS: {
        const ChunkWriter c(w, type);
        w.array(sequence.vars.raw);
        break;
    }
    case STRTAB: {
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(sequence.strTab.size()));
        for (const StrTabEntry& s : sequence.strTab) {
            w.pstr(s.name);
            w.u8(s.code);
        }
        break;
    }
    case TEMPO:
    case NTEMPO: {
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(sequence.tempoMap.entries.size()));
        for (const TempoEntry& e : sequence.tempoMap.entries) {
            w.u32(bytes::u32(e.time));
            w.array(e.reserved1);
            w.u16(bytes::u16(type == TEMPO ? e.bpm100 / 100 : e.bpm100));
            w.array(e.reserved2);
        }
        break;
    }
    case METER: {
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(sequence.meterMap.entries.size()));
        for (const MeterEntry& m : sequence.meterMap.entries) {
            w.array(m.reserved1);
            w.u16(bytes::u16(m.measure));
            w.u8(bytes::u8(m.numerator));
            w.u8(bytes::u8(m.denominatorLog2));
            w.array(m.reserved2);
        }
        break;
    }
    case METERKEY: {
        // One record per measure where the meter or the key changes.
        std::vector<int> measures;
        measures.reserve(sequence.meterMap.entries.size() + sequence.keyMap.size());
        for (const MeterEntry& m : sequence.meterMap.entries) {
            measures.push_back(m.measure);
        }
        for (const KeyEntry& k : sequence.keyMap) {
            measures.push_back(k.measure);
        }
        std::ranges::sort(measures);
        const auto [first, last] = std::ranges::unique(measures);
        measures.erase(first, last);
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(measures.size()));
        for (const int measure : measures) {
            const MeterEntry& m = sequence.meterMap.atMeasure(measure);
            int alterations = sequence.keyMap.front().alterations;
            for (const KeyEntry& k : sequence.keyMap) {
                if (k.measure <= measure) {
                    alterations = k.alterations;
                }
            }
            w.u16(bytes::u16(measure));
            w.u8(bytes::u8(m.numerator));
            w.u8(bytes::u8(m.denominatorLog2));
            w.s8(alterations);
        }
        break;
    }
    case TIMEFMT: {
        const ChunkWriter c(w, type);
        w.u16(static_cast<std::uint16_t>(sequence.timecode.rate));
        w.u32(bytes::u32(sequence.timecode.offsetFrames));
        break;
    }
    case METRONOME: {
        const ChunkWriter c(w, type);
        const Metronome& m = sequence.metronome;
        w.u16(bytes::u16(m.port));
        w.u16(bytes::u16(m.channel));
        w.u16(bytes::u16(m.key));
        w.u16(bytes::u16(m.velocity));
        w.u16(bytes::u16(m.duration));
        w.u16(m.reserved[0]);
        w.u16(m.reserved[1]);
        break;
    }
    case THRU: {
        const ChunkWriter c(w, type);
        const Thru& t = sequence.thru;
        w.u8(bytes::u8(t.mode));
        w.u8(t.reserved1);
        w.u8(bytes::u8(t.port));
        w.s8(t.channel);
        w.s8(t.keyPlus);
        w.s8(t.velPlus);
        w.array(t.reserved2);
        break;
    }
    case MARKERS: {
        const ChunkWriter c(w, type);
        w.u32(bytes::u32(sequence.markers.size()));
        for (const Marker& m : sequence.markers) {
            w.u8(m.smpteLocked ? 1U : 0U);
            w.u8(m.reserved0);
            w.u24(bytes::u32(m.time));
            w.array(m.reserved1);
            w.pstr(m.name);
        }
        break;
    }
    case COMMENTS: {
        if (!sequence.comments) {
            break;
        }
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(sequence.comments->size()));
        w.bytes(*sequence.comments);
        break;
    }
    case SYSEX: {
        const SysxBank& b = sequence.sysxBanks.at(static_cast<std::size_t>(index));
        const ChunkWriter c(w, type);
        w.u8(bytes::u8(b.bank));
        w.u16(bytes::u16(b.data.size()));
        w.u8(b.autoSend ? 1U : 0U);
        w.pstr(b.name);
        w.bytes(b.data);
        break;
    }
    case TRACK:
        writeTrackChunk(w, *sequence.findStream(index));
        break;
    case TRKNAME: {
        const ChunkWriter c(w, type);
        const Stream& t = *sequence.findStream(index);
        w.u16(bytes::u16(t.number));
        w.pstr(t.name);
        break;
    }
    case STREAM:
    case STRSTREAM:
        writeStream(w, *sequence.findStream(index));
        break;
    case TRKPATCH: {
        const Stream& t = *sequence.findStream(index);
        if (!t.patch) {
            break;
        }
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(t.number));
        w.u16(bytes::u16(*t.patch));
        break;
    }
    case TRKREPS: {
        const Stream& t = *sequence.findStream(index);
        if (!t.reps) {
            break;
        }
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(t.number));
        w.u16(bytes::u16(*t.reps));
        break;
    }
    case TRKOFFS: {
        const Stream& t = *sequence.findStream(index);
        if (!t.timePlus) {
            break;
        }
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(t.number));
        w.s16(*t.timePlus);
        break;
    }
    case TRKVOL: {
        const Stream& t = *sequence.findStream(index);
        if (!t.volume && !t.pan) {
            break;
        }
        const ChunkWriter c(w, type);
        w.u16(bytes::u16(t.number));
        w.s16(t.volume.value_or(-1));
        w.s16(t.pan.value_or(-1));
        break;
    }
    default: {
        const UnknownChunk& u = sequence.unknownChunks.at(static_cast<std::size_t>(index));
        const ChunkWriter c(w, type);
        w.bytes(u.data);
        break;
    }
    }
}

std::vector<LayoutEntry> normalizedLayout(const Sequence& sequence) {
    std::vector<LayoutEntry> l;
    for (const std::uint8_t t : {TIMEBASE, VARS, STRTAB, NTEMPO, METER, METERKEY, TIMEFMT, METRONOME, THRU}) {
        l.push_back({t, -1});
    }
    if (!sequence.markers.empty()) {
        l.push_back({MARKERS, -1});
    }
    for (const Stream& t : sequence.streams) {
        l.push_back({TRACK, t.number});
        l.push_back({TRKNAME, t.number});
        if (!t.events.empty()) {
            l.push_back({t.hasStringEvents() ? STRSTREAM : STREAM, t.number});
        }
        if (t.patch) {
            l.push_back({TRKPATCH, t.number});
        }
        if (t.reps) {
            l.push_back({TRKREPS, t.number});
        }
        if (t.timePlus) {
            l.push_back({TRKOFFS, t.number});
        }
        if (t.volume || t.pan) {
            l.push_back({TRKVOL, t.number});
        }
    }
    for (std::size_t i = 0; i < sequence.sysxBanks.size(); ++i) {
        l.push_back({SYSEX, static_cast<int>(i)});
    }
    if (sequence.comments) {
        l.push_back({COMMENTS, -1});
    }
    return l;
}

} // namespace

std::vector<std::uint8_t> write(const Sequence& sequence, const WriteMode mode) {
    Writer w;
    w.str(std::string(kMagic));
    w.u8(0x1a);
    w.u8(bytes::u8(sequence.versionMinor));
    w.u8(bytes::u8(sequence.versionMajor));
    const std::vector<LayoutEntry> layout =
        (mode == WriteMode::Preserve && !sequence.layout.empty()) ? sequence.layout : normalizedLayout(sequence);
    for (const LayoutEntry& le : layout) {
        if (isTrackChunk(le.type) && sequence.findStream(le.index) == nullptr) {
            continue; // stream was deleted after reading
        }
        writeChunk(w, sequence, le.type, le.index);
    }
    w.u8(END);
    return std::move(w.out);
}

void writeFile(const Sequence& sequence, const std::string& path, const WriteMode mode) {
    bytes::writeFileBytes(path, write(sequence, mode));
}

// ---------------------------------------------------------------- dump

namespace {

void dumpChunk(std::ostream& os, const std::uint8_t type, Reader& c, const std::uint32_t len) {
    switch (type) {
    case TIMEBASE:
        os << "  ppq=" << c.u16();
        break;
    case TRACK:
    case TRKNAME:
    case STREAM:
    case STRSTREAM:
    case TRKPATCH:
    case TRKREPS:
    case TRKOFFS:
    case TRKVOL: {
        os << "  track " << c.u16() + 1;
        if (type == TRACK || type == TRKNAME) {
            os << " name='" << c.pstr() << "'";
        } else if (type == STREAM) {
            os << " events=" << c.u16();
        } else if (type == STRSTREAM) {
            os << " events=" << c.u32();
        } else if (type == TRKPATCH) {
            os << " patch=" << c.u16();
        } else if (type == TRKREPS) {
            os << " reps=" << c.u16();
        } else if (type == TRKOFFS) {
            os << " ticks=" << c.s16();
        } else {
            os << " volume=" << c.s16();
            os << " pan=" << c.s16();
        }
        break;
    }
    case TEMPO:
    case NTEMPO: {
        const int n = c.u16();
        os << "  entries=" << n;
        for (int i = 0; i < n && i < 6; ++i) {
            const Tick t = c.u32();
            c.skip(4);
            const int v = c.u16();
            c.skip(8);
            os << " [" << t << ":" << (type == TEMPO ? v * 100 : v) / 100.0 << "]";
        }
        break;
    }
    case METER: {
        const int n = c.u16();
        os << "  entries=" << n;
        for (int i = 0; i < n && i < 6; ++i) {
            c.skip(4);
            const int m = c.u16();
            const int a = c.u8();
            const int b = c.u8();
            c.skip(4);
            os << " [m" << m << ":" << a << "/" << (1 << b) << "]";
        }
        break;
    }
    case METERKEY: {
        const int n = c.u16();
        os << "  entries=" << n;
        for (int i = 0; i < n && i < 6; ++i) {
            const int m = c.u16();
            const int a = c.u8();
            const int b = c.u8();
            const int k = c.s8();
            os << " [m" << m << ":" << a << "/" << (1 << b) << " key" << k << "]";
        }
        break;
    }
    case TIMEFMT:
        os << "  fps=" << c.u16() << " offset=" << c.u32() << " frames";
        break;
    case MARKERS: {
        const std::uint32_t n = c.u32();
        os << "  count=" << n;
        for (std::uint32_t i = 0; i < n && i < 8; ++i) {
            const int lock = c.u8();
            c.skip(1);
            const Tick t = c.u24();
            c.skip(5);
            os << " [" << t << (lock != 0 ? "L" : "") << ":'" << c.pstr() << "']";
        }
        break;
    }
    case COMMENTS: {
        const std::size_t n = c.u16();
        std::string s = c.str(n);
        std::ranges::replace(s, '\n', ' ');
        os << "  '" << s.substr(0, 60) << (s.size() > 60 ? "...'" : "'");
        break;
    }
    case STRTAB: {
        const int n = c.u16();
        for (int i = 0; i < n; ++i) {
            const std::string s = c.pstr();
            os << " " << s << "=" << static_cast<int>(c.u8());
        }
        break;
    }
    default: {
        os << "  ";
        for (std::uint32_t i = 0; i < len && i < 24; ++i) {
            std::array<char, 4> h{};
            std::snprintf(h.data(), h.size(), "%02x ", c.u8());
            os << h.data();
        }
        if (len > 24) {
            os << "...";
        }
    }
    }
}

} // namespace

std::string dump(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream os;
    Reader r(bytes);
    if (r.remaining() < kHeaderSize || r.str(kMagic.size()) != kMagic) {
        throw Error("not a Cakewalk .WRK file");
    }
    r.skip(1);
    const int minor = r.u8();
    const int major = r.u8();
    os << "CAKEWALK version " << major << "." << minor << ", " << bytes.size() << " bytes\n";
    while (!r.atEnd()) {
        const std::uint8_t type = r.u8();
        if (type == END) {
            os << "END\n";
            break;
        }
        const std::uint32_t len = r.u32();
        if (len > r.remaining()) {
            os << "truncated chunk " << chunkName(type) << "\n";
            break;
        }
        Reader c(r.ptr(), len);
        r.skip(len);
        std::array<char, 64> head{};
        std::snprintf(head.data(), head.size(), "%-10s (%3d) len %6u", chunkName(type), type, len);
        os << head.data();
        try {
            dumpChunk(os, type, c, len);
        } catch (const Error& e) {
            os << "  <" << e.what() << ">";
        }
        os << "\n";
    }
    return os.str();
}

} // namespace sseq::wrk
