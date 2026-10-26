// libsseq: C API implementation.
#include "sseq/sseq.h"

#include "sseq/sseq.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>

// The opaque handles wrap the C++ objects.
struct sseq_sequence {
    sseq::Sequence sequence;
    mutable std::string scratch; // backs const char* results until the next call that changes the sequence
};

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): per-thread error message
thread_local std::string gLastError;

void setError(const std::string& message) {
    gLastError = message;
}

/// Runs `f`, turning exceptions into a false return and an error message.
template <typename F>
bool guarded(const F& f) {
    try {
        f();
        return true;
    } catch (const std::exception& e) {
        setError(e.what());
        return false;
    } catch (...) {
        setError("unknown error");
        return false;
    }
}

template <typename T>
T* release(std::unique_ptr<T> p) {
    return p.release();
}

const sseq::Stream* streamAt(const sseq_sequence* s, const int index) {
    if (s == nullptr || index < 0 || static_cast<std::size_t>(index) >= s->sequence.streams.size()) {
        setError("stream index out of range");
        return nullptr;
    }
    return &s->sequence.streams[static_cast<std::size_t>(index)];
}

sseq::Stream* streamAt(sseq_sequence* s, const int index) {
    return const_cast<sseq::Stream*>(streamAt(static_cast<const sseq_sequence*>(s), index)); // NOLINT
}

sseq_event toC(const sseq::Event& e) {
    sseq_event out{};
    out.time = e.time;
    out.kind = static_cast<sseq_event_kind>(e.kind);
    out.channel = e.channel;
    out.data1 = e.data1;
    out.data2 = e.data2;
    out.duration = e.duration;
    return out;
}

sseq::Event fromC(const sseq_event& e) {
    sseq::Event out;
    out.time = e.time;
    out.kind = static_cast<sseq::EventKind>(e.kind);
    out.channel = static_cast<std::uint8_t>(e.channel & 0x0f);
    out.data1 = e.data1;
    out.data2 = e.data2;
    out.duration = e.duration;
    return out;
}

sseq_midi_message toC(const sseq::engine::MidiMessage& m) {
    sseq_midi_message out{};
    out.time = m.time;
    out.port = m.port;
    out.size = m.size;
    for (std::size_t i = 0; i < m.data.size(); ++i) {
        out.data[i] = m.data[i];
    }
    return out;
}

sseq::engine::MidiMessage fromC(const sseq_midi_message& m) {
    sseq::engine::MidiMessage out;
    out.time = m.time;
    out.port = m.port;
    out.size = std::min<std::uint8_t>(m.size, 3);
    std::ranges::copy(m.data, out.data.begin());
    return out;
}

sseq_playback_options toC(const sseq::engine::PlaybackOptions& o) {
    sseq_playback_options out{};
    out.look_ahead = o.lookAhead;
    out.tempo_ratio = o.tempoRatio;
    out.send_stream_parameters = o.sendStreamParameters ? 1 : 0;
    out.chase = o.chase ? 1 : 0;
    out.zero_controllers_on_stop = o.zeroControllersOnStop ? 1 : 0;
    out.send_sysex_banks = o.sendSysexBanks ? 1 : 0;
    out.has_loop = o.loopFrom && o.loopThru ? 1 : 0;
    out.loop_from = o.loopFrom.value_or(0);
    out.loop_thru = o.loopThru.value_or(0);
    out.metronome = o.metronome ? 1 : 0;
    out.metronome_accent = o.metronomeAccent ? 1 : 0;
    out.count_in_measures = o.countInMeasures;
    out.stop_at_end = o.stopAtEnd ? 1 : 0;
    return out;
}

sseq::engine::PlaybackOptions fromC(const sseq_playback_options* o) {
    sseq::engine::PlaybackOptions out;
    if (o == nullptr) {
        return out;
    }
    out.lookAhead = o->look_ahead;
    out.tempoRatio = o->tempo_ratio > 0 ? o->tempo_ratio : 1.0;
    out.sendStreamParameters = o->send_stream_parameters != 0;
    out.chase = o->chase != 0;
    out.zeroControllersOnStop = o->zero_controllers_on_stop != 0;
    out.sendSysexBanks = o->send_sysex_banks != 0;
    if (o->has_loop != 0) {
        out.loopFrom = o->loop_from;
        out.loopThru = o->loop_thru;
    }
    out.metronome = o->metronome != 0;
    out.metronomeAccent = o->metronome_accent != 0;
    out.countInMeasures = o->count_in_measures;
    out.stopAtEnd = o->stop_at_end != 0;
    return out;
}

sseq_record_options toC(const sseq::engine::RecordOptions& o) {
    sseq_record_options out{};
    out.notes = o.notes ? 1 : 0;
    out.controllers = o.controllers ? 1 : 0;
    out.patches = o.patches ? 1 : 0;
    out.pitch_wheel = o.pitchWheel ? 1 : 0;
    out.aftertouch = o.aftertouch ? 1 : 0;
    out.has_punch = o.punchIn && o.punchOut ? 1 : 0;
    out.punch_in = o.punchIn.value_or(0);
    out.punch_out = o.punchOut.value_or(0);
    return out;
}

sseq::engine::RecordOptions fromC(const sseq_record_options* o) {
    sseq::engine::RecordOptions out;
    if (o == nullptr) {
        return out;
    }
    out.notes = o->notes != 0;
    out.controllers = o->controllers != 0;
    out.patches = o->patches != 0;
    out.pitchWheel = o->pitch_wheel != 0;
    out.aftertouch = o->aftertouch != 0;
    if (o->has_punch != 0) {
        out.punchIn = o->punch_in;
        out.punchOut = o->punch_out;
    }
    return out;
}

void copyString(const std::string& s, char* buffer, const std::size_t size) {
    if (buffer == nullptr || size == 0) {
        return;
    }
    const std::size_t n = std::min(size - 1, s.size());
    std::memcpy(buffer, s.data(), n);
    buffer[n] = '\0';
}

sseq_sequence* readWith(sseq::Sequence (*reader)(const std::vector<std::uint8_t>&), const std::uint8_t* bytes,
                        const std::size_t size) {
    std::unique_ptr<sseq_sequence> s;
    if (!guarded([&] {
            s = std::make_unique<sseq_sequence>();
            s->sequence = reader(std::vector<std::uint8_t>(bytes, bytes + size));
        })) {
        return nullptr;
    }
    return release(std::move(s));
}

sseq_sequence* readFileWith(sseq::Sequence (*reader)(const std::string&), const char* path) {
    std::unique_ptr<sseq_sequence> s;
    if (!guarded([&] {
            s = std::make_unique<sseq_sequence>();
            s->sequence = reader(path);
        })) {
        return nullptr;
    }
    return release(std::move(s));
}

int toData(const std::vector<std::uint8_t>& bytes, sseq_data* out) {
    if (out == nullptr) {
        return 0;
    }
    out->bytes = new std::uint8_t[bytes.size()]; // NOLINT(cppcoreguidelines-owning-memory)
    out->size = bytes.size();
    std::ranges::copy(bytes, out->bytes);
    return 1;
}

} // namespace

// ---------------------------------------------------------------- basics

extern "C" {

const char* sseq_version(void) {
    return "0.5.0";
}

const char* sseq_last_error(void) {
    return gLastError.c_str();
}

void sseq_data_free(sseq_data* data) {
    if (data != nullptr) {
        delete[] data->bytes; // NOLINT(cppcoreguidelines-owning-memory)
        data->bytes = nullptr;
        data->size = 0;
    }
}

// ---------------------------------------------------------------- sequence

sseq_sequence* sseq_sequence_new(void) {
    return new sseq_sequence{}; // NOLINT(cppcoreguidelines-owning-memory)
}

sseq_sequence* sseq_sequence_copy(const sseq_sequence* sequence) {
    if (sequence == nullptr) {
        return nullptr;
    }
    auto* s = new sseq_sequence{}; // NOLINT(cppcoreguidelines-owning-memory)
    s->sequence = sequence->sequence;
    return s;
}

void sseq_sequence_free(sseq_sequence* sequence) {
    delete sequence; // NOLINT(cppcoreguidelines-owning-memory)
}

sseq_sequence* sseq_sequence_read_wrk(const char* path) {
    return readFileWith(sseq::wrk::readFile, path);
}

sseq_sequence* sseq_sequence_read_wrk_data(const uint8_t* bytes, const size_t size) {
    return readWith(sseq::wrk::read, bytes, size);
}

sseq_sequence* sseq_sequence_read_mid(const char* path) {
    return readFileWith([](const std::string& p) { return sseq::smf::readFile(p); }, path);
}

sseq_sequence* sseq_sequence_read_mid_data(const uint8_t* bytes, const size_t size) {
    return readWith([](const std::vector<std::uint8_t>& b) { return sseq::smf::read(b); }, bytes, size);
}

int sseq_sequence_write_wrk(const sseq_sequence* sequence, const char* path, const int normalize) {
    const auto mode = normalize != 0 ? sseq::wrk::WriteMode::Normalize : sseq::wrk::WriteMode::Preserve;
    return guarded([&] { sseq::wrk::writeFile(sequence->sequence, path, mode); }) ? 1 : 0;
}

int sseq_sequence_write_wrk_data(const sseq_sequence* sequence, const int normalize, sseq_data* out) {
    const auto mode = normalize != 0 ? sseq::wrk::WriteMode::Normalize : sseq::wrk::WriteMode::Preserve;
    std::vector<std::uint8_t> bytes;
    if (!guarded([&] { bytes = sseq::wrk::write(sequence->sequence, mode); })) {
        return 0;
    }
    return toData(bytes, out);
}

int sseq_sequence_write_mid(const sseq_sequence* sequence, const char* path, const int port_meta) {
    sseq::smf::WriteOptions o;
    o.portMeta = port_meta != 0;
    return guarded([&] { sseq::smf::writeFile(sequence->sequence, path, o); }) ? 1 : 0;
}

int sseq_sequence_write_mid_data(const sseq_sequence* sequence, const int port_meta, sseq_data* out) {
    sseq::smf::WriteOptions o;
    o.portMeta = port_meta != 0;
    std::vector<std::uint8_t> bytes;
    if (!guarded([&] { bytes = sseq::smf::write(sequence->sequence, o); })) {
        return 0;
    }
    return toData(bytes, out);
}

int sseq_sequence_ppq(const sseq_sequence* sequence) {
    return sequence->sequence.ppq;
}

void sseq_sequence_set_ppq(sseq_sequence* sequence, const int ppq) {
    sequence->sequence.ppq = ppq;
    sequence->sequence.syncPpq();
}

sseq_tick sseq_sequence_end_tick(const sseq_sequence* sequence) {
    return sequence->sequence.endTime();
}

const char* sseq_sequence_comments(const sseq_sequence* sequence) {
    sequence->scratch = sequence->sequence.commentsText();
    return sequence->scratch.c_str();
}

void sseq_sequence_set_comments(sseq_sequence* sequence, const char* text) {
    sequence->sequence.setComments(text != nullptr ? text : "");
}

int64_t sseq_sequence_var(const sseq_sequence* sequence, const sseq_var var) {
    const sseq::Vars& v = sequence->sequence.vars;
    switch (var) {
    case SSEQ_VAR_NOW:
        return v.now();
    case SSEQ_VAR_FROM:
        return v.from();
    case SSEQ_VAR_THRU:
        return v.thru();
    case SSEQ_VAR_METRONOME_PLAYBACK:
        return v.metronomePlayback() ? 1 : 0;
    case SSEQ_VAR_METRONOME_RECORDING:
        return v.metronomeRecording() ? 1 : 0;
    case SSEQ_VAR_METRONOME_ACCENT:
        return v.metronomeAccent() ? 1 : 0;
    case SSEQ_VAR_COUNT_IN:
        return v.countIn();
    case SSEQ_VAR_PATCH_SEARCH_BACK:
        return v.patchSearchBack() ? 1 : 0;
    case SSEQ_VAR_ZERO_CONTROLLERS:
        return v.zeroControllers() ? 1 : 0;
    case SSEQ_VAR_THRU_ON:
        return v.thruOn() ? 1 : 0;
    case SSEQ_VAR_AUTO_RESTART:
        return v.autoRestart() ? 1 : 0;
    case SSEQ_VAR_TRANSMIT_MIDI_SYNC:
        return v.transmitMidiSync() ? 1 : 0;
    }
    return 0;
}

int sseq_sequence_set_var(sseq_sequence* sequence, const sseq_var var, const int64_t value) {
    sseq::Vars& v = sequence->sequence.vars;
    const bool b = value != 0;
    switch (var) {
    case SSEQ_VAR_NOW:
        v.setNow(value);
        return 1;
    case SSEQ_VAR_FROM:
        v.setFrom(value);
        return 1;
    case SSEQ_VAR_THRU:
        v.setThru(value);
        return 1;
    case SSEQ_VAR_METRONOME_PLAYBACK:
        v.setMetronomePlayback(b);
        return 1;
    case SSEQ_VAR_METRONOME_RECORDING:
        v.setMetronomeRecording(b);
        return 1;
    case SSEQ_VAR_METRONOME_ACCENT:
        v.setMetronomeAccent(b);
        return 1;
    case SSEQ_VAR_COUNT_IN:
        v.setCountIn(static_cast<int>(std::clamp<int64_t>(value, 0, 255)));
        return 1;
    case SSEQ_VAR_PATCH_SEARCH_BACK:
        v.setPatchSearchBack(b);
        return 1;
    case SSEQ_VAR_ZERO_CONTROLLERS:
        v.setZeroControllers(b);
        return 1;
    case SSEQ_VAR_TRANSMIT_MIDI_SYNC:
        v.setTransmitMidiSync(b);
        return 1;
    case SSEQ_VAR_THRU_ON:
    case SSEQ_VAR_AUTO_RESTART:
        break;
    }
    setError("read-only variable");
    return 0;
}

void sseq_sequence_metronome(const sseq_sequence* sequence, sseq_metronome* out) {
    const sseq::Metronome& m = sequence->sequence.metronome;
    *out = sseq_metronome{m.port, m.channel, m.key, m.velocity, m.duration};
}

void sseq_sequence_set_metronome(sseq_sequence* sequence, const sseq_metronome* metronome) {
    sseq::Metronome& m = sequence->sequence.metronome;
    m.port = metronome->port;
    m.channel = metronome->channel;
    m.key = metronome->key;
    m.velocity = metronome->velocity;
    m.duration = metronome->duration;
}

void sseq_sequence_thru(const sseq_sequence* sequence, sseq_thru* out) {
    const sseq::Thru& t = sequence->sequence.thru;
    *out = sseq_thru{t.mode, t.port, t.channel, t.keyPlus, t.velPlus};
}

void sseq_sequence_set_thru(sseq_sequence* sequence, const sseq_thru* thru) {
    sseq::Thru& t = sequence->sequence.thru;
    t.mode = thru->mode;
    t.port = thru->port;
    t.channel = thru->channel;
    t.keyPlus = thru->key_plus;
    t.velPlus = thru->vel_plus;
}

// ---------------------------------------------------------------- streams

int sseq_sequence_stream_count(const sseq_sequence* sequence) {
    return static_cast<int>(sequence->sequence.streams.size());
}

int sseq_sequence_find_stream(const sseq_sequence* sequence, const int number) {
    const auto it = std::ranges::find(sequence->sequence.streams, number, &sseq::Stream::number);
    return it == sequence->sequence.streams.end() ? -1 : static_cast<int>(it - sequence->sequence.streams.begin());
}

int sseq_sequence_add_stream(sseq_sequence* sequence, const int number) {
    (void)sequence->sequence.stream(number);
    return sseq_sequence_find_stream(sequence, number);
}

int sseq_sequence_remove_stream(sseq_sequence* sequence, const int index) {
    if (streamAt(sequence, index) == nullptr) {
        return 0;
    }
    sequence->sequence.streams.erase(sequence->sequence.streams.begin() + index);
    return 1;
}

int sseq_stream_get_params(const sseq_sequence* sequence, const int index, sseq_stream_params* out) {
    const sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr) {
        return 0;
    }
    sseq_stream_params p{};
    p.number = t->number;
    p.channel = t->channel ? *t->channel : -1;
    p.key_plus = t->keyPlus;
    p.vel_plus = t->velPlus;
    p.port = t->port;
    p.selected = t->selected ? 1 : 0;
    p.muted = t->muted ? 1 : 0;
    p.patch = t->patch.value_or(-1);
    p.reps = t->reps.value_or(0);
    p.has_time_plus = t->timePlus ? 1 : 0;
    p.time_plus = t->timePlus.value_or(0);
    p.volume = t->volume.value_or(-1);
    p.pan = t->pan.value_or(-1);
    *out = p;
    return 1;
}

int sseq_stream_set_params(sseq_sequence* sequence, const int index, const sseq_stream_params* p) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr) {
        return 0;
    }
    t->channel =
        p->channel >= 0 ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(p->channel & 0x0f)) : std::nullopt;
    t->keyPlus = static_cast<std::int8_t>(std::clamp(p->key_plus, -127, 127));
    t->velPlus = static_cast<std::int8_t>(std::clamp(p->vel_plus, -127, 127));
    t->port = static_cast<std::uint8_t>(std::clamp(p->port, 0, 255));
    t->selected = p->selected != 0;
    t->muted = p->muted != 0;
    t->patch = p->patch >= 0 ? std::optional<int>(p->patch) : std::nullopt;
    t->reps = p->reps > 0 ? std::optional<int>(p->reps) : std::nullopt;
    t->loop = p->reps > 1;
    t->timePlus = p->has_time_plus != 0 ? std::optional<int>(static_cast<int>(p->time_plus)) : std::nullopt;
    t->volume = p->volume >= 0 ? std::optional<int>(p->volume) : std::nullopt;
    t->pan = p->pan >= 0 ? std::optional<int>(p->pan) : std::nullopt;
    return 1;
}

const char* sseq_stream_name(const sseq_sequence* sequence, const int index) {
    const sseq::Stream* t = streamAt(sequence, index);
    return t == nullptr ? nullptr : t->name.c_str();
}

int sseq_stream_set_name(sseq_sequence* sequence, const int index, const char* name) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr) {
        return 0;
    }
    t->name = name != nullptr ? name : "";
    return 1;
}

sseq_tick sseq_stream_end_tick(const sseq_sequence* sequence, const int index) {
    const sseq::Stream* t = streamAt(sequence, index);
    return t == nullptr ? 0 : t->endTime();
}

size_t sseq_stream_event_count(const sseq_sequence* sequence, const int index) {
    const sseq::Stream* t = streamAt(sequence, index);
    return t == nullptr ? 0 : t->events.size();
}

int sseq_stream_event(const sseq_sequence* sequence, const int index, const size_t event_index, sseq_event* out) {
    const sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr || event_index >= t->events.size()) {
        setError("event index out of range");
        return 0;
    }
    *out = toC(t->events[event_index]);
    return 1;
}

const uint8_t* sseq_stream_event_payload(const sseq_sequence* sequence, const int index, const size_t event_index,
                                         size_t* size) {
    const sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr || event_index >= t->events.size() || !t->events[event_index].isString()) {
        if (size != nullptr) {
            *size = 0;
        }
        return nullptr;
    }
    const sseq::Event& e = t->events[event_index];
    if (size != nullptr) {
        *size = e.payload.size();
    }
    return e.payload.data();
}

namespace {

size_t insertSorted(sseq::Stream& t, sseq::Event e) {
    // Insert after every event with the same time (stable order).
    const auto pos = std::ranges::upper_bound(t.events, e.time, {}, &sseq::Event::time);
    const auto index = static_cast<std::size_t>(pos - t.events.begin());
    t.events.insert(pos, std::move(e));
    return index;
}

} // namespace

size_t sseq_stream_insert_event(sseq_sequence* sequence, const int index, const sseq_event* event) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr || event->kind > SSEQ_EVENT_SYSEX) {
        setError("invalid stream or event kind");
        return SIZE_MAX;
    }
    return insertSorted(*t, fromC(*event));
}

size_t sseq_stream_insert_string_event(sseq_sequence* sequence, const int index, const sseq_tick time,
                                       const sseq_event_kind kind, const uint8_t* payload, const size_t size) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr || kind < SSEQ_EVENT_MCI_COMMAND) {
        setError("invalid stream or event kind");
        return SIZE_MAX;
    }
    sseq::Event e;
    e.time = time;
    e.kind = static_cast<sseq::EventKind>(kind);
    e.payload.assign(payload, payload + size);
    return insertSorted(*t, std::move(e));
}

size_t sseq_stream_set_event(sseq_sequence* sequence, const int index, const size_t event_index,
                             const sseq_event* event) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr || event_index >= t->events.size()) {
        setError("event index out of range");
        return SIZE_MAX;
    }
    sseq::Event& e = t->events[event_index];
    const bool moved = e.time != event->time;
    sseq::Event replacement = fromC(*event);
    replacement.payload = e.payload;
    if (!moved) {
        e = std::move(replacement);
        return event_index;
    }
    t->events.erase(t->events.begin() + static_cast<std::ptrdiff_t>(event_index));
    return insertSorted(*t, std::move(replacement));
}

int sseq_stream_remove_event(sseq_sequence* sequence, const int index, const size_t event_index) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr || event_index >= t->events.size()) {
        setError("event index out of range");
        return 0;
    }
    t->events.erase(t->events.begin() + static_cast<std::ptrdiff_t>(event_index));
    return 1;
}

int sseq_stream_clear_events(sseq_sequence* sequence, const int index) {
    sseq::Stream* t = streamAt(sequence, index);
    if (t == nullptr) {
        return 0;
    }
    t->events.clear();
    return 1;
}

// ---------------------------------------------------------------- maps and time

size_t sseq_sequence_tempo_count(const sseq_sequence* sequence) {
    return sequence->sequence.tempoMap.entries.size();
}

int sseq_sequence_tempo(const sseq_sequence* sequence, const size_t index, sseq_tempo* out) {
    const auto& entries = sequence->sequence.tempoMap.entries;
    if (index >= entries.size()) {
        return 0;
    }
    *out = sseq_tempo{entries[index].time, entries[index].bpm100};
    return 1;
}

void sseq_sequence_set_tempo(sseq_sequence* sequence, const sseq_tick time, const int bpm100) {
    sequence->sequence.tempoMap.set(time, bpm100);
}

void sseq_sequence_remove_tempo(sseq_sequence* sequence, const sseq_tick time) {
    sequence->sequence.tempoMap.remove(time);
}

int sseq_sequence_tempo_at(const sseq_sequence* sequence, const sseq_tick time, sseq_tempo* out) {
    const sseq::TempoEntry& e = sequence->sequence.tempoMap.at(time);
    *out = sseq_tempo{e.time, e.bpm100};
    return 1;
}

size_t sseq_sequence_meter_count(const sseq_sequence* sequence) {
    return sequence->sequence.meterMap.entries.size();
}

int sseq_sequence_meter(const sseq_sequence* sequence, const size_t index, sseq_meter* out) {
    const auto& entries = sequence->sequence.meterMap.entries;
    if (index >= entries.size()) {
        return 0;
    }
    *out = sseq_meter{entries[index].measure, entries[index].numerator, entries[index].denominatorLog2};
    return 1;
}

void sseq_sequence_set_meter(sseq_sequence* sequence, const sseq_meter* meter) {
    auto& entries = sequence->sequence.meterMap.entries;
    const auto pos = std::ranges::lower_bound(entries, meter->measure, {}, &sseq::MeterEntry::measure);
    if (pos != entries.end() && pos->measure == meter->measure) {
        pos->numerator = meter->numerator;
        pos->denominatorLog2 = meter->denominator_log2;
        return;
    }
    sseq::MeterEntry e;
    e.measure = meter->measure;
    e.numerator = meter->numerator;
    e.denominatorLog2 = meter->denominator_log2;
    entries.insert(pos, e);
}

void sseq_sequence_remove_meter(sseq_sequence* sequence, const int measure) {
    if (measure <= 1) {
        return;
    }
    std::erase_if(sequence->sequence.meterMap.entries,
                  [measure](const sseq::MeterEntry& e) { return e.measure == measure; });
}

int sseq_sequence_meter_at(const sseq_sequence* sequence, const int measure, sseq_meter* out) {
    const sseq::MeterEntry& e = sequence->sequence.meterMap.atMeasure(measure);
    *out = sseq_meter{e.measure, e.numerator, e.denominatorLog2};
    return 1;
}

size_t sseq_sequence_key_count(const sseq_sequence* sequence) {
    return sequence->sequence.keyMap.size();
}

int sseq_sequence_key(const sseq_sequence* sequence, const size_t index, sseq_key* out) {
    const auto& keys = sequence->sequence.keyMap;
    if (index >= keys.size()) {
        return 0;
    }
    *out = sseq_key{keys[index].measure, keys[index].alterations};
    return 1;
}

void sseq_sequence_set_key(sseq_sequence* sequence, const sseq_key* key) {
    auto& keys = sequence->sequence.keyMap;
    const auto pos = std::ranges::lower_bound(keys, key->measure, {}, &sseq::KeyEntry::measure);
    if (pos != keys.end() && pos->measure == key->measure) {
        pos->alterations = key->alterations;
        return;
    }
    keys.insert(pos, sseq::KeyEntry{key->measure, key->alterations});
}

void sseq_sequence_remove_key(sseq_sequence* sequence, const int measure) {
    if (measure <= 1) {
        return;
    }
    std::erase_if(sequence->sequence.keyMap, [measure](const sseq::KeyEntry& e) { return e.measure == measure; });
}

int sseq_sequence_key_at(const sseq_sequence* sequence, const int measure, sseq_key* out) {
    const auto& keys = sequence->sequence.keyMap;
    sseq_key k{1, 0};
    for (const sseq::KeyEntry& e : keys) {
        if (e.measure > measure) {
            break;
        }
        k = sseq_key{e.measure, e.alterations};
    }
    *out = k;
    return 1;
}

size_t sseq_sequence_marker_count(const sseq_sequence* sequence) {
    return sequence->sequence.markers.size();
}

int sseq_sequence_marker(const sseq_sequence* sequence, const size_t index, sseq_marker* out) {
    const auto& markers = sequence->sequence.markers;
    if (index >= markers.size()) {
        return 0;
    }
    *out = sseq_marker{markers[index].time, markers[index].smpteLocked ? 1 : 0};
    return 1;
}

const char* sseq_sequence_marker_name(const sseq_sequence* sequence, const size_t index) {
    const auto& markers = sequence->sequence.markers;
    return index < markers.size() ? markers[index].name.c_str() : nullptr;
}

size_t sseq_sequence_add_marker(sseq_sequence* sequence, const sseq_marker* marker, const char* name) {
    auto& markers = sequence->sequence.markers;
    sseq::Marker m;
    m.time = marker->time;
    m.smpteLocked = marker->smpte_locked != 0;
    m.name = name != nullptr ? name : "";
    const auto pos = std::ranges::upper_bound(markers, m.time, {}, &sseq::Marker::time);
    const auto index = static_cast<std::size_t>(pos - markers.begin());
    markers.insert(pos, std::move(m));
    return index;
}

int sseq_sequence_set_marker(sseq_sequence* sequence, const size_t index, const sseq_marker* marker, const char* name) {
    auto& markers = sequence->sequence.markers;
    if (index >= markers.size()) {
        return 0;
    }
    markers[index].time = marker->time;
    markers[index].smpteLocked = marker->smpte_locked != 0;
    if (name != nullptr) {
        markers[index].name = name;
    }
    std::ranges::stable_sort(markers, {}, &sseq::Marker::time);
    return 1;
}

int sseq_sequence_remove_marker(sseq_sequence* sequence, const size_t index) {
    auto& markers = sequence->sequence.markers;
    if (index >= markers.size()) {
        return 0;
    }
    markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    return 1;
}

void sseq_sequence_timecode(const sseq_sequence* sequence, sseq_timecode* out) {
    *out = sseq_timecode{static_cast<sseq_frame_rate>(sequence->sequence.timecode.rate),
                         sequence->sequence.timecode.offsetFrames};
}

void sseq_sequence_set_timecode(sseq_sequence* sequence, const sseq_timecode* timecode) {
    sequence->sequence.timecode.rate = static_cast<sseq::FrameRate>(timecode->rate);
    sequence->sequence.timecode.offsetFrames = timecode->offset_frames;
}

double sseq_sequence_tick_to_seconds(const sseq_sequence* sequence, const sseq_tick tick) {
    return sequence->sequence.tempoMap.tickToSeconds(tick);
}

sseq_tick sseq_sequence_seconds_to_tick(const sseq_sequence* sequence, const double seconds) {
    return sequence->sequence.tempoMap.secondsToTick(seconds);
}

void sseq_sequence_tick_to_mbt(const sseq_sequence* sequence, const sseq_tick tick, sseq_mbt* out) {
    const sseq::MBT m = sequence->sequence.meterMap.tickToMBT(tick);
    *out = sseq_mbt{m.measure, m.beat, m.tick};
}

sseq_tick sseq_sequence_mbt_to_tick(const sseq_sequence* sequence, const sseq_mbt* mbt) {
    return sequence->sequence.meterMap.mbtToTick(sseq::MBT{mbt->measure, mbt->beat, mbt->tick});
}

void sseq_sequence_tick_to_hmsf(const sseq_sequence* sequence, const sseq_tick tick, sseq_hmsf* out) {
    const sseq::Timecode& tc = sequence->sequence.timecode;
    const sseq::HMSF h =
        tc.framesToHMSF(tc.secondsToFrames(sequence->sequence.tempoMap.tickToSeconds(tick)) + tc.offsetFrames);
    *out = sseq_hmsf{h.hours, h.minutes, h.seconds, h.frames};
}

sseq_tick sseq_sequence_hmsf_to_tick(const sseq_sequence* sequence, const sseq_hmsf* hmsf) {
    const sseq::Timecode& tc = sequence->sequence.timecode;
    const std::int64_t frames =
        tc.hmsfToFrames(sseq::HMSF{hmsf->hours, hmsf->minutes, hmsf->seconds, hmsf->frames}) - tc.offsetFrames;
    return sequence->sequence.tempoMap.secondsToTick(tc.framesToSeconds(frames));
}

sseq_tick sseq_sequence_next_measure(const sseq_sequence* sequence, const sseq_tick tick) {
    return sequence->sequence.meterMap.nextMeasure(tick);
}

sseq_tick sseq_sequence_prev_measure(const sseq_sequence* sequence, const sseq_tick tick) {
    return sequence->sequence.meterMap.prevMeasure(tick);
}

sseq_tick sseq_sequence_measure_start(const sseq_sequence* sequence, const int measure) {
    return sequence->sequence.meterMap.measureStart(measure);
}

void sseq_format_mbt(const sseq_mbt* mbt, char* buffer, const size_t size) {
    copyString(sseq::formatMBT(sseq::MBT{mbt->measure, mbt->beat, mbt->tick}), buffer, size);
}

int sseq_parse_mbt(const char* text, sseq_mbt* out) {
    sseq::MBT m;
    if (text == nullptr || !sseq::parseMBT(text, m)) {
        return 0;
    }
    *out = sseq_mbt{m.measure, m.beat, m.tick};
    return 1;
}

void sseq_format_hmsf(const sseq_hmsf* hmsf, char* buffer, const size_t size) {
    copyString(sseq::formatHMSF(sseq::HMSF{hmsf->hours, hmsf->minutes, hmsf->seconds, hmsf->frames}), buffer, size);
}

int sseq_parse_hmsf(const char* text, sseq_hmsf* out) {
    sseq::HMSF h;
    if (text == nullptr || !sseq::parseHMSF(text, h)) {
        return 0;
    }
    *out = sseq_hmsf{h.hours, h.minutes, h.seconds, h.frames};
    return 1;
}

// ---------------------------------------------------------------- display text

void sseq_key_name(const int key, char* buffer, const size_t size) {
    copyString(sseq::display::keyName(key), buffer, size);
}

int sseq_parse_key_name(const char* text, int* key) {
    int k = 0;
    if (text == nullptr || key == nullptr || !sseq::display::parseKeyName(text, k)) {
        return 0;
    }
    *key = k;
    return 1;
}

void sseq_duration_text(const sseq_tick ticks, const int ppq, char* buffer, const size_t size) {
    copyString(sseq::display::durationText(ticks, ppq), buffer, size);
}

int sseq_parse_duration_text(const char* text, const int ppq, sseq_tick* ticks) {
    sseq::Tick t = 0;
    if (text == nullptr || ticks == nullptr || !sseq::display::parseDurationText(text, ppq, t)) {
        return 0;
    }
    *ticks = t;
    return 1;
}

const char* sseq_event_kind_name(const sseq_event_kind kind) {
    return sseq::display::kindName(static_cast<sseq::EventKind>(kind)).data();
}

const char* sseq_gm_program_name(const int program) {
    return sseq::display::generalMidiProgramName(program).data();
}

const char* sseq_gm_percussion_name(const int key) {
    return sseq::display::generalMidiPercussionName(key).data();
}

void sseq_event_channel_text(const sseq_event* event, char* buffer, const size_t size) {
    copyString(sseq::display::channelText(fromC(*event)), buffer, size);
}

void sseq_event_values_text(const sseq_event* event, const uint8_t* payload, const size_t payload_size, const int ppq,
                            char* buffer, const size_t size) {
    sseq::Event e = fromC(*event);
    if (payload != nullptr) {
        e.payload.assign(payload, payload + payload_size);
    }
    copyString(sseq::display::valuesText(e, ppq), buffer, size);
}

// ---------------------------------------------------------------- editing

namespace {

/// Turns the C selection (stream indices) into the library's (stream numbers).
bool toSelection(const sseq_sequence* s, const sseq_selection* selection, sseq::edit::Selection& out) {
    if (s == nullptr || selection == nullptr) {
        setError("null argument");
        return false;
    }
    out.from = selection->from;
    out.thru = selection->thru;
    out.streams.clear();
    if (selection->streams == nullptr) {
        for (const sseq::Stream& t : s->sequence.streams) {
            out.streams.push_back(t.number);
        }
        return true;
    }
    for (std::size_t i = 0; i < selection->stream_count; ++i) {
        const sseq::Stream* t = streamAt(s, selection->streams[i]);
        if (t == nullptr) {
            return false;
        }
        out.streams.push_back(t->number);
    }
    return true;
}

/// Applies the C kind mask to `filter`; 0 keeps the command's default.
void applyKinds(const uint32_t kinds, sseq::edit::EventFilter& filter) {
    if (kinds == 0) {
        return;
    }
    for (std::size_t k = 0; k < filter.kinds.size(); ++k) {
        filter.kinds[k] = (kinds & (1U << k)) != 0;
    }
}

} // namespace

sseq_tick sseq_resolution_ticks(const sseq_resolution resolution, const int ppq) {
    return sseq::edit::resolutionTicks(static_cast<sseq::edit::Resolution>(resolution), ppq);
}

sseq_tick sseq_quantize_time(const sseq_tick time, const sseq_tick resolution, const int strength) {
    return sseq::edit::quantizeTime(time, resolution, strength);
}

sseq_tick sseq_quantize_duration(const sseq_tick duration, const sseq_tick resolution) {
    return sseq::edit::quantizeDuration(duration, resolution);
}

int sseq_quantize(sseq_sequence* sequence, const sseq_selection* selection, const sseq_quantize_options* options) {
    sseq::edit::Selection sel;
    if (options == nullptr || !toSelection(sequence, selection, sel)) {
        setError("null argument");
        return -1;
    }
    sseq::edit::QuantizeOptions o;
    o.resolution = options->resolution;
    o.startTimes = options->start_times != 0;
    o.durations = options->durations != 0;
    o.strength = options->strength;
    applyKinds(options->kinds, o.filter);
    return sseq::edit::quantize(sequence->sequence, sel, o);
}

// ---------------------------------------------------------------- sysex banks

size_t sseq_sequence_sysex_bank_count(const sseq_sequence* sequence) {
    return sequence->sequence.sysxBanks.size();
}

const char* sseq_sequence_sysex_bank_name(const sseq_sequence* sequence, const size_t index) {
    const auto& banks = sequence->sequence.sysxBanks;
    return index < banks.size() ? banks[index].name.c_str() : nullptr;
}

const uint8_t* sseq_sequence_sysex_bank_data(const sseq_sequence* sequence, const size_t index, size_t* size) {
    const auto& banks = sequence->sequence.sysxBanks;
    if (index >= banks.size()) {
        if (size != nullptr) {
            *size = 0;
        }
        return nullptr;
    }
    if (size != nullptr) {
        *size = banks[index].data.size();
    }
    return banks[index].data.data();
}

int sseq_sequence_sysex_bank_auto_send(const sseq_sequence* sequence, const size_t index) {
    const auto& banks = sequence->sequence.sysxBanks;
    return index < banks.size() && banks[index].autoSend ? 1 : 0;
}

size_t sseq_sequence_set_sysex_bank(sseq_sequence* sequence, const size_t index, const char* name, const int auto_send,
                                    const uint8_t* data, const size_t size) {
    auto& banks = sequence->sequence.sysxBanks;
    if (index > banks.size()) {
        setError("sysex bank index out of range");
        return SIZE_MAX;
    }
    if (index == banks.size()) {
        sseq::SysxBank b;
        b.bank = static_cast<int>(index);
        banks.push_back(std::move(b));
    }
    sseq::SysxBank& b = banks[index];
    b.name = name != nullptr ? name : "";
    b.autoSend = auto_send != 0;
    b.data.assign(data, data + size);
    return index;
}

// ---------------------------------------------------------------- playback

void sseq_playback_options_default(sseq_playback_options* out) {
    *out = toC(sseq::engine::PlaybackOptions{});
}

void sseq_playback_options_from_sequence(const sseq_sequence* sequence, const int recording,
                                         sseq_playback_options* out) {
    *out = toC(sseq::engine::PlaybackOptions::fromSequence(sequence->sequence, recording != 0));
}

} // extern "C"

namespace {

class CallbackClock final : public sseq::engine::Clock {
public:
    explicit CallbackClock(const sseq_transport_callbacks& cb) : cb_(cb) {}
    [[nodiscard]] double now() const override { return cb_.now != nullptr ? cb_.now(cb_.context) : 0.0; }

private:
    sseq_transport_callbacks cb_;
};

class CallbackSink final : public sseq::engine::MidiSink {
public:
    explicit CallbackSink(const sseq_transport_callbacks& cb) : cb_(cb) {}
    void send(const sseq::engine::MidiMessage& message) override {
        if (cb_.send != nullptr) {
            const sseq_midi_message m = toC(message);
            cb_.send(cb_.context, &m);
        }
    }
    void sendSysex(const double time, const int port, const std::vector<std::uint8_t>& bytes) override {
        if (cb_.send_sysex != nullptr) {
            cb_.send_sysex(cb_.context, time, port, bytes.data(), bytes.size());
        }
    }
    void flush() override {
        if (cb_.flush != nullptr) {
            cb_.flush(cb_.context);
        }
    }
    void discardScheduled() override {
        if (cb_.discard_scheduled != nullptr) {
            cb_.discard_scheduled(cb_.context);
        }
    }

private:
    sseq_transport_callbacks cb_;
};

} // namespace

struct sseq_transport {
    sseq_transport_callbacks callbacks;
    CallbackClock clock;
    CallbackSink sink;
    sseq::engine::Transport transport;

    sseq_transport(const sseq::Sequence& sequence, const sseq_transport_callbacks& cb,
                   const sseq::engine::PlaybackOptions& o)
        : callbacks(cb), clock(cb), sink(cb), transport(sequence, clock, sink, o) {
        if (callbacks.string_event != nullptr) {
            transport.setStringEventHandler([this](const int stream, const sseq::Event& e, const double time) {
                const sseq_event ce = toC(e);
                callbacks.string_event(callbacks.context, stream, &ce, e.payload.data(), e.payload.size(), time);
            });
        }
    }
};

struct sseq_recorder {
    sseq::engine::Recorder recorder;
    sseq_recorder(const sseq::engine::Transport& t, const sseq::engine::RecordOptions& o) : recorder(t, o) {}
};

extern "C" {

sseq_transport* sseq_transport_new(const sseq_sequence* sequence, const sseq_transport_callbacks* callbacks,
                                   const sseq_playback_options* options) {
    if (sequence == nullptr || callbacks == nullptr) {
        setError("sequence and callbacks are required");
        return nullptr;
    }
    std::unique_ptr<sseq_transport> t;
    if (!guarded([&] { t = std::make_unique<sseq_transport>(sequence->sequence, *callbacks, fromC(options)); })) {
        return nullptr;
    }
    return release(std::move(t));
}

void sseq_transport_free(sseq_transport* transport) {
    delete transport; // NOLINT(cppcoreguidelines-owning-memory)
}

void sseq_transport_options(const sseq_transport* transport, sseq_playback_options* out) {
    *out = toC(transport->transport.options());
}

void sseq_transport_set_options(sseq_transport* transport, const sseq_playback_options* options) {
    transport->transport.setOptions(fromC(options));
}

void sseq_transport_set_tempo_ratio(sseq_transport* transport, const double ratio) {
    transport->transport.setTempoRatio(ratio > 0 ? ratio : 1.0);
}

void sseq_transport_play(sseq_transport* transport) {
    transport->transport.play();
}

void sseq_transport_stop(sseq_transport* transport) {
    transport->transport.stop();
}

void sseq_transport_panic(sseq_transport* transport) {
    transport->transport.panic();
}

void sseq_transport_rewind(sseq_transport* transport) {
    transport->transport.rewind();
}

void sseq_transport_seek(sseq_transport* transport, const sseq_tick tick) {
    transport->transport.seek(tick);
}

int sseq_transport_playing(const sseq_transport* transport) {
    return transport->transport.playing() ? 1 : 0;
}

int sseq_transport_counting_in(const sseq_transport* transport) {
    return transport->transport.countingIn() ? 1 : 0;
}

sseq_tick sseq_transport_now(const sseq_transport* transport) {
    return transport->transport.now();
}

sseq_tick sseq_transport_end_tick(const sseq_transport* transport) {
    return transport->transport.endTick();
}

sseq_tick sseq_transport_tick_at(const sseq_transport* transport, const double time, int* valid) {
    const std::optional<sseq::Tick> t = transport->transport.tickAt(time);
    if (valid != nullptr) {
        *valid = t ? 1 : 0;
    }
    return t.value_or(0);
}

void sseq_transport_set_muted(sseq_transport* transport, const int stream_number, const int muted) {
    transport->transport.setMuted(stream_number, muted != 0);
}

void sseq_transport_set_solo(sseq_transport* transport, const int stream_number, const int solo) {
    transport->transport.setSolo(stream_number, solo != 0);
}

int sseq_transport_audible(const sseq_transport* transport, const int stream_number) {
    return transport->transport.isAudible(stream_number) ? 1 : 0;
}

int sseq_transport_pump(sseq_transport* transport) {
    return transport->transport.pump() ? 1 : 0;
}

int sseq_render(const sseq_sequence* sequence, const sseq_playback_options* options, const sseq_tick from,
                const sseq_tick thru, sseq_midi_message** out, size_t* count) {
    std::vector<sseq::engine::MidiMessage> messages;
    if (!guarded([&] {
            messages = sseq::engine::render(sequence->sequence, fromC(options), from,
                                            thru < 0 ? std::nullopt : std::optional<sseq::Tick>(thru));
        })) {
        return 0;
    }
    *count = messages.size();
    *out = new sseq_midi_message[messages.size()]; // NOLINT(cppcoreguidelines-owning-memory)
    for (std::size_t i = 0; i < messages.size(); ++i) {
        (*out)[i] = toC(messages[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    return 1;
}

void sseq_render_free(sseq_midi_message* messages) {
    delete[] messages; // NOLINT(cppcoreguidelines-owning-memory)
}

// ---------------------------------------------------------------- recording

void sseq_record_options_default(sseq_record_options* out) {
    *out = toC(sseq::engine::RecordOptions{});
}

sseq_recorder* sseq_recorder_new(const sseq_transport* transport, const sseq_record_options* options) {
    if (transport == nullptr) {
        setError("transport is required");
        return nullptr;
    }
    return new sseq_recorder(transport->transport, fromC(options)); // NOLINT(cppcoreguidelines-owning-memory)
}

void sseq_recorder_free(sseq_recorder* recorder) {
    delete recorder; // NOLINT(cppcoreguidelines-owning-memory)
}

void sseq_recorder_options(const sseq_recorder* recorder, sseq_record_options* out) {
    *out = toC(recorder->recorder.options());
}

void sseq_recorder_set_options(sseq_recorder* recorder, const sseq_record_options* options) {
    recorder->recorder.setOptions(fromC(options));
}

void sseq_recorder_receive(sseq_recorder* recorder, const sseq_midi_message* message) {
    recorder->recorder.receive(fromC(*message));
}

void sseq_recorder_finish(sseq_recorder* recorder, const double time) {
    recorder->recorder.finish(time);
}

size_t sseq_recorder_count(const sseq_recorder* recorder) {
    return recorder->recorder.count();
}

int sseq_recorder_event(const sseq_recorder* recorder, const size_t index, sseq_event* out) {
    const auto& events = recorder->recorder.events();
    if (index >= events.size()) {
        return 0;
    }
    *out = toC(events[index]);
    return 1;
}

void sseq_recorder_clear(sseq_recorder* recorder) {
    recorder->recorder.clear();
}

int sseq_recorder_commit(sseq_recorder* recorder, sseq_sequence* sequence, const int stream_index, const int replace,
                         const sseq_tick from, const sseq_tick thru) {
    sseq::Stream* t = streamAt(sequence, stream_index);
    if (t == nullptr) {
        return 0;
    }
    if (replace != 0) {
        recorder->recorder.commitReplacing(*t, from, thru);
    } else {
        recorder->recorder.commit(*t);
    }
    return 1;
}

} // extern "C"
