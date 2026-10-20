// libsseq: Standard MIDI File reader and writer with Cakewalk's mapping, as observed in Cakewalk Apprentice 1.04.
//
// Terminology: "track" here is the file's MTrk chunk, as in the SMF specification; "stream" is the
// sequence's stream (TTSSEQ's word for what the Cakewalk UI shows as a track). The engine maps the
// two the way SeqSaveMidiFile does: track 0 carries the maps and markers, every non-empty stream
// becomes one track, and on load every track after the conductor track becomes one stream.
#include "sseq/smf.hpp"
#include "bytes.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <ranges>
#include <sstream>
#include <string_view>

namespace sseq::smf {

using bytes::Reader;
using bytes::Writer;

namespace {

constexpr std::uint8_t kMetaText = 0x01;
constexpr std::uint8_t kMetaTrackName = 0x03;
constexpr std::uint8_t kMetaMarker = 0x06;
constexpr std::uint8_t kMetaCue = 0x07;
constexpr std::uint8_t kMetaPort = 0x21;
constexpr std::uint8_t kMetaEnd = 0x2f;
constexpr std::uint8_t kMetaTempo = 0x51;
constexpr std::uint8_t kMetaTimeSig = 0x58;
constexpr std::uint8_t kMetaKeySig = 0x59;
constexpr std::string_view kHeaderMagic = "MThd";
constexpr std::string_view kTrackMagic = "MTrk";

/// Sort position of items at the same tick (lower first).
enum Order : int {
    kOrderTimeSig = 0,
    kOrderKeySig = 1,
    kOrderTempo = 2,
    kOrderMarker = 3,
    kOrderPort = 0,
    kOrderName = 1,
    kOrderPatch = 2,
    kOrderVolume = 3,
    kOrderPan = 4,
    kOrderNoteOff = 5,
    kOrderEvent = 10,
};

/// One scheduled output item for the writer.
struct Item {
    Tick time;
    int order;                       // tie-break 1: lower first
    std::vector<std::uint8_t> bytes; // status + data, or a complete meta event
    bool meta;
    bool explicitStatus = false; // written with a status byte and without starting running status
    Tick sub = 0;                // tie-break 2 (note-offs: minus the note's start time)
    int seq = 0;                 // tie-break 3: insertion order
};

std::vector<std::uint8_t> metaBytes(const std::uint8_t type, const std::vector<std::uint8_t>& data) {
    Writer w;
    w.u8(0xff);
    w.u8(type);
    w.vlq(bytes::u32(data.size()));
    w.bytes(data);
    return std::move(w.out);
}

std::vector<std::uint8_t> metaString(const std::uint8_t type, const std::string& s) {
    return metaBytes(type, std::vector<std::uint8_t>(s.begin(), s.end()));
}

void writeTrack(Writer& w, std::vector<Item>& items) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        items[i].seq = static_cast<int>(i);
    }
    std::ranges::sort(items, [](const Item& a, const Item& b) {
        return std::tie(a.time, a.order, a.sub, a.seq) < std::tie(b.time, b.order, b.sub, b.seq);
    });
    Writer t;
    Tick last = 0;
    int running = -1;
    for (const Item& it : items) {
        t.vlq(bytes::u32(it.time - last));
        last = it.time;
        if (it.meta) {
            t.bytes(it.bytes); // meta events do not interrupt running status in Cakewalk's files
            continue;
        }
        if (it.explicitStatus) {
            t.bytes(it.bytes);
            running = -1;
            continue;
        }
        if (it.bytes[0] == running) {
            t.bytes(it.bytes.data() + 1, it.bytes.size() - 1);
        } else {
            t.bytes(it.bytes);
            running = it.bytes[0];
        }
    }
    t.vlq(0);
    t.u8(0xff);
    t.u8(kMetaEnd);
    t.u8(0);
    w.str(std::string(kTrackMagic));
    w.be32(bytes::u32(t.size()));
    w.bytes(t.out);
}

constexpr std::uint8_t clamp7(const int v) noexcept {
    return bytes::u8(std::clamp(v, 0, 127));
}

std::vector<std::uint8_t> channelMessage(const std::uint8_t status, const std::uint8_t d1) {
    return {status, d1};
}
std::vector<std::uint8_t> channelMessage(const std::uint8_t status, const std::uint8_t d1, const std::uint8_t d2) {
    return {status, d1, d2};
}

} // namespace

// ---------------------------------------------------------------- writing

std::vector<std::uint8_t> write(const Sequence& sequence, const WriteOptions& options) {
    auto nonEmpty = sequence.streams | std::views::filter([](const Stream& t) { return !t.events.empty(); });
    const auto trackCount = static_cast<std::size_t>(std::ranges::distance(nonEmpty));

    Writer w;
    w.str(std::string(kHeaderMagic));
    w.be32(6);
    w.be16(1);
    w.be16(bytes::u32(1 + trackCount));
    w.be16(bytes::u32(sequence.ppq));

    // Track 0 (the conductor track): meter, key, tempo, markers.
    std::vector<Item> items;
    items.reserve(sequence.meterMap.entries.size() + sequence.keyMap.size() + sequence.tempoMap.entries.size() +
                  sequence.markers.size());
    for (const MeterEntry& m : sequence.meterMap.entries) {
        // MIDI clocks per metronome click: 24 per quarter note, scaled by the denominator.
        const std::uint8_t clocks = bytes::u8(96 / m.denominator());
        items.push_back({sequence.meterMap.measureStart(m.measure), kOrderTimeSig,
                         metaBytes(kMetaTimeSig, {bytes::u8(m.numerator), bytes::u8(m.denominatorLog2), clocks, 8}),
                         true});
    }
    for (const KeyEntry& k : sequence.keyMap) {
        items.push_back({sequence.meterMap.measureStart(k.measure), kOrderKeySig,
                         metaBytes(kMetaKeySig, {bytes::s8(k.alterations), 0}), true});
    }
    for (const TempoEntry& e : sequence.tempoMap.entries) {
        const auto usec = static_cast<std::uint32_t>(60.0e6 * 100.0 / e.bpm100);
        items.push_back({e.time, kOrderTempo,
                         metaBytes(kMetaTempo, {bytes::u8(usec >> 16), bytes::u8(usec >> 8), bytes::u8(usec)}), true});
    }
    for (const Marker& m : sequence.markers) {
        items.push_back({m.time, kOrderMarker, metaString(m.smpteLocked ? kMetaCue : kMetaMarker, m.name), true});
    }
    writeTrack(w, items);

    // One track per non-empty stream.
    for (const Stream& t : nonEmpty) {
        items.clear();
        if (options.portMeta) {
            items.push_back({0, kOrderPort, metaBytes(kMetaPort, {t.port}), true});
        }
        items.push_back({0, kOrderName, metaString(kMetaTrackName, t.name), true});
        // Parameters go on the forced channel, or on the channel of the stream's first MIDI event.
        std::uint8_t forced = 0;
        if (t.channel) {
            forced = *t.channel;
        } else if (const auto first = std::ranges::find_if(t.events, &Event::isMidi); first != t.events.end()) {
            forced = first->channel;
        }
        // The stream parameters are written with explicit status bytes (no running status).
        if (t.patch) {
            items.push_back({0, kOrderPatch, channelMessage(0xc0 | forced, clamp7(*t.patch)), false, true});
        }
        if (t.volume && *t.volume >= 0) {
            items.push_back({0, kOrderVolume, channelMessage(0xb0 | forced, 7, clamp7(*t.volume)), false, true});
        }
        if (t.pan && *t.pan >= 0) {
            items.push_back({0, kOrderPan, channelMessage(0xb0 | forced, 10, clamp7(*t.pan)), false, true});
        }
        // Note-offs go before any note-on at the same tick. Among note-offs at the same tick, the
        // note that started later comes first (observed in Cakewalk's output); notes that started
        // together keep their order in the stream.
        for (const Event& e : t.events) {
            const std::uint8_t ch = t.channel ? *t.channel : e.channel;
            switch (e.kind) {
            case EventKind::Note: {
                const std::uint8_t key = clamp7(e.data1 + t.keyPlus);
                const std::uint8_t vel = clamp7(e.data2 + t.velPlus);
                items.push_back({e.end(), kOrderNoteOff, channelMessage(0x90 | ch, key, 0), false, false, -e.time});
                items.push_back({e.time, kOrderEvent, channelMessage(0x90 | ch, key, vel), false});
                break;
            }
            case EventKind::KeyAftertouch:
                items.push_back({e.time, kOrderEvent,
                                 channelMessage(0xa0 | ch, clamp7(e.data1 + t.keyPlus), clamp7(e.data2)), false});
                break;
            case EventKind::Controller:
                items.push_back(
                    {e.time, kOrderEvent, channelMessage(0xb0 | ch, clamp7(e.data1), clamp7(e.data2)), false});
                break;
            case EventKind::Patch:
                items.push_back({e.time, kOrderEvent, channelMessage(0xc0 | ch, clamp7(e.data1)), false});
                break;
            case EventKind::ChannelAftertouch:
                items.push_back({e.time, kOrderEvent, channelMessage(0xd0 | ch, clamp7(e.data1)), false});
                break;
            case EventKind::PitchWheel: {
                const int v = e.data1 + 8192;
                items.push_back({e.time, kOrderEvent,
                                 channelMessage(0xe0 | ch, bytes::u8(v & 0x7f), bytes::u8((v >> 7) & 0x7f)), false});
                break;
            }
            case EventKind::Text:
                items.push_back({e.time, kOrderEvent, metaBytes(kMetaText, e.payload), true});
                break;
            case EventKind::Sysex:
            case EventKind::MciCommand:
            case EventKind::Wave:
                break; // not exported by Cakewalk
            }
        }
        writeTrack(w, items);
    }
    return std::move(w.out);
}

void writeFile(const Sequence& sequence, const std::string& path, const WriteOptions& options) {
    bytes::writeFileBytes(path, write(sequence, options));
}

// ---------------------------------------------------------------- reading

namespace {

/// Inserts or replaces the entry for `measure` in a map sorted by measure.
template <class Entry>
void putByMeasure(std::vector<Entry>& entries, const Entry& e) {
    if (const auto it = std::ranges::find(entries, e.measure, &Entry::measure); it != entries.end()) {
        *it = e;
        return;
    }
    const auto pos = std::ranges::find_if(entries, [&e](const Entry& x) { return x.measure > e.measure; });
    entries.insert(pos, e);
}

void importTrack(Sequence& sequence, Reader& r, const int streamNumber, const bool conductor) {
    Stream t;
    t.number = streamNumber;
    bool haveParams = false;
    std::optional<std::uint8_t> firstChannel;
    std::map<int, std::vector<std::size_t>> open; // (channel << 8 | key) -> indices of open notes
    Tick time = 0;
    int running = -1;
    bool ended = false;
    while (!r.atEnd() && !ended) {
        time += r.vlq();
        const std::uint8_t b = r.u8();
        if (b == 0xff) {
            const std::uint8_t type = r.u8();
            const std::uint32_t len = r.vlq();
            const std::vector<std::uint8_t> d = r.bytes(len);
            const std::string s(d.begin(), d.end());
            switch (type) {
            case kMetaEnd:
                ended = true;
                break;
            case kMetaTrackName:
                t.name = s;
                break;
            case kMetaPort:
                if (!d.empty()) {
                    t.port = d[0];
                }
                break;
            case kMetaText: {
                Event e;
                e.time = time;
                e.kind = EventKind::Text;
                e.payload = d;
                t.events.push_back(std::move(e));
                break;
            }
            case kMetaMarker:
            case kMetaCue: {
                Marker m;
                m.time = time;
                m.smpteLocked = type == kMetaCue;
                m.name = s;
                sequence.markers.push_back(std::move(m));
                break;
            }
            case kMetaTempo:
                if (d.size() == 3) {
                    const std::uint32_t usec = bytes::le(d[2], d[1], d[0], 0);
                    if (usec != 0) {
                        sequence.tempoMap.set(time, static_cast<int>(std::lround(60.0e6 * 100.0 / usec)));
                    }
                }
                break;
            case kMetaTimeSig:
                if (d.size() >= 2) {
                    MeterEntry m;
                    m.measure = sequence.meterMap.measureAt(time);
                    m.numerator = d[0];
                    m.denominatorLog2 = d[1];
                    putByMeasure(sequence.meterMap.entries, m);
                }
                break;
            case kMetaKeySig:
                if (!d.empty()) {
                    KeyEntry k;
                    k.measure = sequence.meterMap.measureAt(time);
                    k.alterations = static_cast<std::int8_t>(d[0]);
                    putByMeasure(sequence.keyMap, k);
                }
                break;
            default:
                break;
            }
            continue;
        }
        if (b == 0xf0 || b == 0xf7) { // sysex: not imported yet
            r.skip(r.vlq());
            continue;
        }
        std::uint8_t status = 0;
        std::uint8_t d1 = 0;
        if ((b & 0x80U) != 0) {
            status = b;
            running = b;
            d1 = r.u8();
        } else {
            if (running < 0) {
                throw Error("running status without status byte");
            }
            status = bytes::u8(running);
            d1 = b;
        }
        const std::uint8_t ch = status & 0x0fU;
        const std::uint8_t kind = status & 0xf0U;
        if (!firstChannel) {
            firstChannel = ch;
        }
        Event e;
        e.time = time;
        e.channel = ch;
        switch (kind) {
        case 0x80:
        case 0x90: {
            const std::uint8_t vel = r.u8();
            const int k = (ch << 8) | d1;
            if (kind == 0x90 && vel > 0) {
                e.kind = EventKind::Note;
                e.data1 = d1;
                e.data2 = vel;
                open[k].push_back(t.events.size());
                t.events.push_back(e);
            } else if (!open[k].empty()) {
                // Cakewalk pairs a note-off with the most recent open note of that key.
                Event& on = t.events[open[k].back()];
                on.duration = std::max<Tick>(1, time - on.time);
                open[k].pop_back();
            }
            continue;
        }
        case 0xa0:
            e.kind = EventKind::KeyAftertouch;
            e.data1 = d1;
            e.data2 = r.u8();
            break;
        case 0xb0: {
            e.kind = EventKind::Controller;
            e.data1 = d1;
            e.data2 = r.u8();
            if (time == 0 && !haveParams && (d1 == 7 || d1 == 10)) {
                (d1 == 7 ? t.volume : t.pan) = e.data2;
                continue;
            }
            break;
        }
        case 0xc0:
            e.kind = EventKind::Patch;
            e.data1 = d1;
            if (time == 0 && !haveParams) {
                t.patch = d1; // the last Program Change before the first other event wins
                continue;
            }
            break;
        case 0xd0:
            e.kind = EventKind::ChannelAftertouch;
            e.data1 = d1;
            break;
        case 0xe0: {
            const std::uint8_t d2 = r.u8();
            e.kind = EventKind::PitchWheel;
            e.data1 = (((d2 & 0x7f) << 7) | (d1 & 0x7f)) - 8192;
            break;
        }
        default:
            throw Error("unexpected status byte in MIDI file");
        }
        if (e.kind != EventKind::Patch && e.kind != EventKind::Controller) {
            haveParams = true; // the first "real" event ends the parameter prefix
        }
        t.events.push_back(e);
    }
    if (conductor && t.events.empty() && !firstChannel) {
        return;
    }
    t.channel = firstChannel;
    sortEvents(t.events);
    sequence.streams.push_back(std::move(t));
}

} // namespace

Sequence read(const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    if (r.remaining() < 14 || r.str(kHeaderMagic.size()) != kHeaderMagic) {
        throw Error("not a Standard MIDI File");
    }
    const std::uint32_t hlen = r.be32();
    const int format = r.be16();
    const int ntrks = r.be16();
    const int division = r.be16();
    if (hlen > 6) {
        r.skip(hlen - 6);
    }
    if (format != 0 && format != 1) {
        throw Error("only SMF format 0 and 1 are supported");
    }
    if ((division & 0x8000) != 0) {
        throw Error("SMPTE time division is not supported");
    }
    Sequence sequence;
    sequence.ppq = division;
    sequence.syncPpq();
    sequence.tempoMap.entries.clear();
    sequence.tempoMap.set(0, 12000);
    sequence.layout.clear();
    int number = 0;
    for (int i = 0; i < ntrks && !r.atEnd(); ++i) {
        if (r.str(kTrackMagic.size()) != kTrackMagic) {
            throw Error("missing MTrk chunk");
        }
        const std::uint32_t len = r.be32();
        Reader t(r.ptr(), len);
        r.skip(len);
        const std::size_t before = sequence.streams.size();
        importTrack(sequence, t, number, format == 1 && i == 0);
        if (sequence.streams.size() > before) {
            ++number;
        }
    }
    return sequence;
}

Sequence readFile(const std::string& path) {
    return read(bytes::readFileBytes(path));
}

// ---------------------------------------------------------------- dump

std::string dump(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream os;
    Reader r(bytes);
    if (r.remaining() < 14 || r.str(kHeaderMagic.size()) != kHeaderMagic) {
        throw Error("not a Standard MIDI File");
    }
    const std::uint32_t hlen = r.be32();
    const int format = r.be16();
    const int ntrks = r.be16();
    const int division = r.be16();
    if (hlen > 6) {
        r.skip(hlen - 6);
    }
    os << "SMF format " << format << ", " << ntrks << " tracks, division " << division << "\n";
    for (int i = 0; i < ntrks && !r.atEnd(); ++i) {
        if (r.str(kTrackMagic.size()) != kTrackMagic) {
            os << "missing MTrk\n";
            break;
        }
        const std::uint32_t len = r.be32();
        Reader t(r.ptr(), len);
        r.skip(len);
        os << "track " << i << " (" << len << " bytes)\n";
        Tick time = 0;
        int running = -1;
        std::map<int, int> counts;
        int shown = 0;
        try {
            while (!t.atEnd()) {
                time += t.vlq();
                const std::uint8_t b = t.u8();
                if (b == 0xff) {
                    const std::uint8_t type = t.u8();
                    const std::uint32_t n = t.vlq();
                    const std::vector<std::uint8_t> d = t.bytes(n);
                    os << "  t=" << time << " meta " << static_cast<int>(type);
                    if (type == kMetaTempo && n == 3) {
                        os << " tempo " << bytes::le(d[2], d[1], d[0], 0) << " usec/qn";
                    } else if (type == kMetaTimeSig && n >= 2) {
                        os << " timesig " << static_cast<int>(d[0]) << "/" << (1 << d[1]);
                    } else if (type == kMetaKeySig && n >= 2) {
                        os << " keysig " << static_cast<int>(static_cast<std::int8_t>(d[0]))
                           << (d[1] != 0 ? " minor" : " major");
                    } else if (type == kMetaEnd) {
                        os << " end of track";
                    } else if (type == kMetaPort && n >= 1) {
                        os << " port " << static_cast<int>(d[0]);
                    } else {
                        os << " '" << std::string(d.begin(), d.end()) << "'";
                    }
                    os << "\n";
                    if (type == kMetaEnd) {
                        break;
                    }
                    continue;
                }
                if (b == 0xf0 || b == 0xf7) {
                    const std::uint32_t n = t.vlq();
                    t.skip(n);
                    os << "  t=" << time << " sysex " << n << " bytes\n";
                    continue;
                }
                std::uint8_t status = 0;
                std::uint8_t d1 = 0;
                if ((b & 0x80U) != 0) {
                    status = b;
                    running = b;
                    d1 = t.u8();
                } else {
                    status = bytes::u8(running);
                    d1 = b;
                }
                const int kind = status & 0xf0;
                const int d2 = (kind == 0xc0 || kind == 0xd0) ? -1 : t.u8();
                counts[kind]++;
                if (shown < 6) {
                    os << "  t=" << time << " " << std::hex << static_cast<int>(status) << std::dec << " "
                       << static_cast<int>(d1);
                    if (d2 >= 0) {
                        os << " " << d2;
                    }
                    os << "\n";
                    ++shown;
                }
            }
        } catch (const Error& e) {
            os << "  <" << e.what() << ">\n";
        }
        os << "  channel events:";
        for (const auto& [k, v] : counts) {
            os << " " << std::hex << k << std::dec << ":" << v;
        }
        os << "\n";
    }
    return os.str();
}

} // namespace sseq::smf
