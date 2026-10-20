// libsseq: recording implementation.
#include "sseq/recorder.hpp"

#include <algorithm>
#include <ranges>

namespace sseq::engine {

struct Recorder::Impl {
    const Transport* transport;
    RecordOptions options;
    std::vector<Event> take;
    struct OpenNote {
        std::uint8_t channel;
        int key;
        std::size_t index; // into take
    };
    std::vector<OpenNote> open; // in order of arrival; a note-off closes the most recent match (LIFO)

    Impl(const Transport& t, const RecordOptions& o) : transport(&t), options(o) {}

    [[nodiscard]] bool inRange(const Tick tick) const {
        return (!options.punchIn || tick >= *options.punchIn) && (!options.punchOut || tick < *options.punchOut);
    }

    void closeNote(const std::uint8_t channel, const int key, const Tick at) {
        const auto found = std::ranges::find_if(
            open | std::views::reverse, [&](const OpenNote& n) { return n.channel == channel && n.key == key; });
        if (found == open.rend()) {
            return;
        }
        Event& note = take[found->index];
        note.duration = std::max<Tick>(1, at - note.time);
        open.erase(std::next(found).base());
    }

    /// Closes every held note at `at` (duration 1 when the time is unknown) and sorts the take.
    void closeAll(const std::optional<Tick> at) {
        for (const OpenNote& n : open) {
            Event& note = take[n.index];
            note.duration = at ? std::max<Tick>(1, *at - note.time) : 1;
        }
        open.clear();
        sortEvents(take);
    }
};

Recorder::Recorder(const Transport& transport, RecordOptions options)
    : impl_(std::make_unique<Impl>(transport, options)) {}

Recorder::~Recorder() = default;

const RecordOptions& Recorder::options() const noexcept {
    return impl_->options;
}

void Recorder::setOptions(const RecordOptions& options) {
    impl_->options = options;
}

void Recorder::receive(const MidiMessage& m) {
    if (m.size == 0 || m.status() >= 0xf0) {
        return; // system messages are not recorded
    }
    const std::optional<Tick> at = impl_->transport->tickAt(m.time);
    if (!at) {
        return;
    }
    const Tick tick = *at;
    const std::uint8_t ch = m.channel();
    const RecordOptions& o = impl_->options;
    switch (m.status() & 0xf0U) {
    case 0x90:
        if (m.data[2] != 0) {
            if (o.notes && impl_->inRange(tick)) {
                impl_->take.push_back(Event::note(tick, ch, m.data[1], m.data[2], 1));
                impl_->open.push_back({ch, m.data[1], impl_->take.size() - 1});
            }
            break;
        }
        [[fallthrough]];
    case 0x80:
        impl_->closeNote(ch, m.data[1], tick);
        break;
    case 0xa0:
        if (o.aftertouch && impl_->inRange(tick)) {
            impl_->take.push_back(Event::keyAftertouch(tick, ch, m.data[1], m.data[2]));
        }
        break;
    case 0xb0:
        if (o.controllers && impl_->inRange(tick)) {
            impl_->take.push_back(Event::controller(tick, ch, m.data[1], m.data[2]));
        }
        break;
    case 0xc0:
        if (o.patches && impl_->inRange(tick)) {
            impl_->take.push_back(Event::patch(tick, ch, m.data[1]));
        }
        break;
    case 0xd0:
        if (o.aftertouch && impl_->inRange(tick)) {
            impl_->take.push_back(Event::channelAftertouch(tick, ch, m.data[1]));
        }
        break;
    case 0xe0:
        if (o.pitchWheel && impl_->inRange(tick)) {
            impl_->take.push_back(Event::pitchWheel(tick, ch, (m.data[1] | (m.data[2] << 7)) - 8192));
        }
        break;
    default:
        break;
    }
}

void Recorder::finish(const double time) {
    impl_->closeAll(impl_->transport->tickAt(time));
}

const std::vector<Event>& Recorder::events() const noexcept {
    return impl_->take;
}

std::size_t Recorder::count() const noexcept {
    return impl_->take.size();
}

bool Recorder::empty() const noexcept {
    return impl_->take.empty();
}

void Recorder::clear() {
    impl_->take.clear();
    impl_->open.clear();
}

void Recorder::commit(Stream& stream) {
    impl_->closeAll(std::nullopt);
    stream.events.insert(stream.events.end(), impl_->take.begin(), impl_->take.end());
    sortEvents(stream.events);
    clear();
}

void Recorder::commitReplacing(Stream& stream, const Tick from, const Tick thru) {
    std::erase_if(stream.events, [&](const Event& e) { return e.time >= from && e.time < thru; });
    commit(stream);
}

} // namespace sseq::engine
