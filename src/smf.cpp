// libsseq: Standard MIDI File reader and writer with the Cakewalk Apprentice 1.04 mapping.
#include "sseq/smf.hpp"
#include "bytes.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <ranges>
#include <sstream>
#include <string_view>

namespace sseq::smf {

using detail::Reader;
using detail::Writer;

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
    w.vlq(detail::u32(data.size()));
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
        t.vlq(detail::u32(it.time - last));
        last = it.time;
        if (it.meta) {
            t.bytes(it.bytes); // meta events do not interrupt running status in Apprentice's files
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
    w.be32(detail::u32(t.size()));
    w.bytes(t.out);
}

constexpr std::uint8_t clamp7(const int v) noexcept {
    return detail::u8(std::clamp(v, 0, 127));
}

std::vector<std::uint8_t> channelMessage(const std::uint8_t status, const std::uint8_t d1) {
    return {status, d1};
}
std::vector<std::uint8_t> channelMessage(const std::uint8_t status, const std::uint8_t d1, const std::uint8_t d2) {
    return {status, d1, d2};
}

} // namespace

// ---------------------------------------------------------------- writing

std::vector<std::uint8_t> write(const Song& song) {
    auto nonEmpty = song.tracks | std::views::filter([](const Track& t) { return !t.events.empty(); });
    const auto trackCount = static_cast<std::size_t>(std::ranges::distance(nonEmpty));

    Writer w;
    w.str(std::string(kHeaderMagic));
    w.be32(6);
    w.be16(1);
    w.be16(detail::u32(1 + trackCount));
    w.be16(detail::u32(song.ppq));

    // Track 0: meter, key, tempo, markers.
    std::vector<Item> items;
    items.reserve(song.meterMap.entries.size() + song.keyMap.size() + song.tempoMap.entries.size() +
                  song.markers.size());
    for (const MeterEntry& m : song.meterMap.entries) {
        // MIDI clocks per metronome click: 24 per quarter note, scaled by the denominator.
        const std::uint8_t clocks = detail::u8(96 / m.denominator());
        items.push_back({song.meterMap.measureStart(m.measure), kOrderTimeSig,
                         metaBytes(kMetaTimeSig, {detail::u8(m.numerator), detail::u8(m.denominatorLog2), clocks, 8}),
                         true});
    }
    for (const KeyEntry& k : song.keyMap) {
        items.push_back({song.meterMap.measureStart(k.measure), kOrderKeySig,
                         metaBytes(kMetaKeySig, {detail::s8(k.alterations), 0}), true});
    }
    for (const TempoEntry& e : song.tempoMap.entries) {
        const auto usec = static_cast<std::uint32_t>(60.0e6 * 100.0 / e.bpm100);
        items.push_back({e.time, kOrderTempo,
                         metaBytes(kMetaTempo, {detail::u8(usec >> 16), detail::u8(usec >> 8), detail::u8(usec)}),
                         true});
    }
    for (const Marker& m : song.markers) {
        items.push_back({m.time, kOrderMarker, metaString(m.smpteLocked ? kMetaCue : kMetaMarker, m.name), true});
    }
    writeTrack(w, items);

    // Song tracks.
    for (const Track& t : nonEmpty) {
        items.clear();
        items.push_back({0, kOrderPort, metaBytes(kMetaPort, {t.port}), true});
        items.push_back({0, kOrderName, metaString(kMetaTrackName, t.name), true});
        // Parameters go on the forced channel, or on the channel of the track's first MIDI event.
        std::uint8_t forced = 0;
        if (t.channel) {
            forced = *t.channel;
        } else if (const auto first = std::ranges::find_if(t.events, &Event::isMidi); first != t.events.end()) {
            forced = first->channel;
        }
        // The track parameters are written with explicit status bytes (no running status).
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
        // note that started later comes first (observed in Apprentice's output); notes that started
        // together keep their stream order.
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
                                 channelMessage(0xe0 | ch, detail::u8(v & 0x7f), detail::u8((v >> 7) & 0x7f)), false});
                break;
            }
            case EventKind::Text:
                items.push_back({e.time, kOrderEvent, metaBytes(kMetaText, e.payload), true});
                break;
            case EventKind::Sysex:
            case EventKind::MciCommand:
            case EventKind::Wave:
                break; // not exported by Apprentice
            }
        }
        writeTrack(w, items);
    }
    return std::move(w.out);
}

void writeFile(const Song& song, const std::string& path) {
    detail::writeFileBytes(path, write(song));
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

void importTrack(Song& song, Reader& r, const int trackNumber, const bool conductor) {
    Track t;
    t.number = trackNumber;
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
                song.markers.push_back(std::move(m));
                break;
            }
            case kMetaTempo:
                if (d.size() == 3) {
                    const std::uint32_t usec = detail::le(d[2], d[1], d[0], 0);
                    if (usec != 0) {
                        song.tempoMap.set(time, static_cast<int>(std::lround(60.0e6 * 100.0 / usec)));
                    }
                }
                break;
            case kMetaTimeSig:
                if (d.size() >= 2) {
                    MeterEntry m;
                    m.measure = song.meterMap.measureAt(time);
                    m.numerator = d[0];
                    m.denominatorLog2 = d[1];
                    putByMeasure(song.meterMap.entries, m);
                }
                break;
            case kMetaKeySig:
                if (!d.empty()) {
                    KeyEntry k;
                    k.measure = song.meterMap.measureAt(time);
                    k.alterations = static_cast<std::int8_t>(d[0]);
                    putByMeasure(song.keyMap, k);
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
            status = detail::u8(running);
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
                // Apprentice pairs a note-off with the most recent open note of that key.
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
    song.tracks.push_back(std::move(t));
}

} // namespace

Song read(const std::vector<std::uint8_t>& bytes) {
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
    Song song;
    song.ppq = division;
    song.syncPpq();
    song.tempoMap.entries.clear();
    song.tempoMap.set(0, 12000);
    song.layout.clear();
    int number = 0;
    for (int i = 0; i < ntrks && !r.atEnd(); ++i) {
        if (r.str(kTrackMagic.size()) != kTrackMagic) {
            throw Error("missing MTrk chunk");
        }
        const std::uint32_t len = r.be32();
        Reader t(r.ptr(), len);
        r.skip(len);
        const std::size_t before = song.tracks.size();
        importTrack(song, t, number, format == 1 && i == 0);
        if (song.tracks.size() > before) {
            ++number;
        }
    }
    return song;
}

Song readFile(const std::string& path) {
    return read(detail::readFileBytes(path));
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
                        os << " tempo " << detail::le(d[2], d[1], d[0], 0) << " usec/qn";
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
                    status = detail::u8(running);
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
