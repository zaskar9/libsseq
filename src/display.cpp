// libsseq: display helpers.
#include "sseq/display.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace sseq::display {

namespace {

constexpr std::array<std::string_view, 12> kNoteLetters = {"C ", "C#", "D ", "D#", "E ", "F ",
                                                           "F#", "G ", "G#", "A ", "A#", "B "};

constexpr std::array<std::string_view, 128> kGmPrograms = {
    // Piano
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1",
    "Electric Piano 2", "Harpsichord", "Clavi",
    // Chromatic percussion
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    // Organ
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion",
    // Guitar
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)",
    "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar harmonics",
    // Bass
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2",
    "Synth Bass 1", "Synth Bass 2",
    // Strings
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    // Ensemble
    "String Ensemble 1", "String Ensemble 2", "SynthStrings 1", "SynthStrings 2", "Choir Aahs", "Voice Oohs",
    "Synth Voice", "Orchestra Hit",
    // Brass
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "SynthBrass 1", "SynthBrass 2",
    // Reed
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    // Pipe
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    // Synth lead
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)",
    "Lead 7 (fifths)", "Lead 8 (bass + lead)",
    // Synth pad
    "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
    "Pad 7 (halo)", "Pad 8 (sweep)",
    // Synth effects
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)",
    "FX 7 (echoes)", "FX 8 (sci-fi)",
    // Ethnic
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag pipe", "Fiddle", "Shanai",
    // Percussive
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    // Sound effects
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
    "Gunshot"};

constexpr int kFirstPercussionKey = 35;
constexpr std::array<std::string_view, 47> kGmPercussion = {
    "Acoustic Bass Drum", "Bass Drum 1",   "Side Stick",     "Acoustic Snare", "Hand Clap",      "Electric Snare",
    "Low Floor Tom",      "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat",   "Low Tom",        "Open Hi-Hat",
    "Low-Mid Tom",        "Hi-Mid Tom",    "Crash Cymbal 1", "High Tom",       "Ride Cymbal 1",  "Chinese Cymbal",
    "Ride Bell",          "Tambourine",    "Splash Cymbal",  "Cowbell",        "Crash Cymbal 2", "Vibraslap",
    "Ride Cymbal 2",      "Hi Bongo",      "Low Bongo",      "Mute Hi Conga",  "Open Hi Conga",  "Low Conga",
    "High Timbale",       "Low Timbale",   "High Agogo",     "Low Agogo",      "Cabasa",         "Maracas",
    "Short Whistle",      "Long Whistle",  "Short Guiro",    "Long Guiro",     "Claves",         "Hi Wood Block",
    "Low Wood Block",     "Mute Cuica",    "Open Cuica",     "Mute Triangle",  "Open Triangle"};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

bool parseInt(std::string_view s, int& out) {
    s = trim(s);
    if (s.empty()) {
        return false;
    }
    const auto result = std::from_chars(s.data(), s.data() + s.size(), out);
    return result.ec == std::errc() && result.ptr == s.data() + s.size();
}

} // namespace

std::string keyName(const int key) {
    const int k = std::clamp(key, 0, 127);
    return std::string(kNoteLetters[static_cast<std::size_t>(k % 12)]) + std::to_string(k / 12);
}

bool parseKeyName(std::string_view text, int& key) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    static constexpr std::string_view kLetters = "C D EF G A B";
    const auto pos = kLetters.find(letter);
    if (pos == std::string_view::npos || letter == ' ') {
        return false;
    }
    int semitone = static_cast<int>(pos);
    text.remove_prefix(1);
    if (!text.empty() && text.front() == '#') {
        ++semitone;
        text.remove_prefix(1);
    } else if (!text.empty() && (text.front() == 'b' || text.front() == 'B') && text.size() > 1) {
        --semitone;
        text.remove_prefix(1);
    }
    int octave = 0;
    if (!parseInt(text, octave)) {
        return false;
    }
    const int k = octave * 12 + semitone;
    if (k < 0 || k > 127) {
        return false;
    }
    key = k;
    return true;
}

std::string durationText(const Tick ticks, const int ppq) {
    const Tick t = std::max<Tick>(0, ticks);
    if (ppq <= 0 || t < ppq) {
        return std::to_string(t);
    }
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%lld:%03lld", static_cast<long long>(t / ppq),
                  static_cast<long long>(t % ppq));
    return buf.data();
}

bool parseDurationText(std::string_view text, const int ppq, Tick& ticks) {
    text = trim(text);
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) {
        int t = 0;
        if (!parseInt(text, t) || t < 0) {
            return false;
        }
        ticks = t;
        return true;
    }
    int quarters = 0;
    int rest = 0;
    if (!parseInt(text.substr(0, colon), quarters) || !parseInt(text.substr(colon + 1), rest) || quarters < 0 ||
        rest < 0) {
        return false;
    }
    ticks = static_cast<Tick>(quarters) * ppq + rest;
    return true;
}

std::string_view kindName(const EventKind kind) {
    switch (kind) {
    case EventKind::Note:
        return "Note";
    case EventKind::KeyAftertouch:
        return "KeyAft";
    case EventKind::Controller:
        return "Contrl";
    case EventKind::Patch:
        return "Patch";
    case EventKind::ChannelAftertouch:
        return "ChanAft";
    case EventKind::PitchWheel:
        return "Wheel";
    case EventKind::Sysex:
        return "Sysx";
    case EventKind::MciCommand:
        return "MCICmd";
    case EventKind::Wave:
        return "Wave";
    case EventKind::Text:
        return "Text";
    }
    return "";
}

std::string_view generalMidiProgramName(const int program) {
    if (program < 0 || program >= static_cast<int>(kGmPrograms.size())) {
        return "";
    }
    return kGmPrograms[static_cast<std::size_t>(program)];
}

std::string_view generalMidiPercussionName(const int key) {
    const int i = key - kFirstPercussionKey;
    if (i < 0 || i >= static_cast<int>(kGmPercussion.size())) {
        return "";
    }
    return kGmPercussion[static_cast<std::size_t>(i)];
}

std::string channelText(const Event& event) {
    return event.isMidi() && event.kind != EventKind::Sysex ? std::to_string(event.channel + 1) : "n/a";
}

std::string valuesText(const Event& event, const int ppq) {
    switch (event.kind) {
    case EventKind::Note:
        return keyName(event.data1) + "  " + std::to_string(event.data2) + "  " + durationText(event.duration, ppq);
    case EventKind::KeyAftertouch:
        return keyName(event.data1) + "  " + std::to_string(event.data2);
    case EventKind::Controller:
        return std::to_string(event.data1) + "  " + std::to_string(event.data2);
    case EventKind::Patch: {
        const std::string_view name = generalMidiProgramName(event.data1);
        return name.empty() ? std::to_string(event.data1) : std::string(name);
    }
    case EventKind::ChannelAftertouch:
    case EventKind::PitchWheel:
    case EventKind::Sysex:
        return std::to_string(event.data1);
    case EventKind::MciCommand:
    case EventKind::Wave:
    case EventKind::Text:
        return event.text();
    }
    return "";
}

} // namespace sseq::display
