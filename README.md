# libsseq

A small C++20 library for Cakewalk `.WRK` (version 2.0) and Standard MIDI Files, with the
functional scope of Cakewalk's `TTSSEQ.DLL` sequencer engine. MIT licensed.

- Read and write `.WRK` losslessly (byte-identical round trip of every reference file).
- Read and write `.MID` with the same mapping Cakewalk uses (optionally without
  Cakewalk's non-standard "MIDI Port" meta event).
- Sequence model: streams, events (notes with durations), tempo map, meter and key maps, SMPTE
  time format, markers, sysex banks, metronome and MIDI-thru settings, comments.
- Time conversions: ticks ↔ seconds ↔ measure:beat:tick ↔ SMPTE frames (including drop-frame).
- Playback engine: a `Transport` (play/stop/seek, loop, mute/solo, tempo ratio, patch and
  controller chase, metronome with count-in, panic) that schedules timestamped MIDI messages
  ahead of a caller-supplied clock into a caller-supplied sink, so it runs unchanged on CoreMIDI
  or in a test; offline `render()`.
- Recording: a `Recorder` turns timestamped input into stream events (note pairing, event filter,
  punch range) and merges the take into a stream.
- Display text as Cakewalk shows it: key names (`C 5` = 60), durations (`1:080`), event kind names,
  General MIDI program names.
- Editing as the Edit menu does it, with the arithmetic taken from Cakewalk Apprentice 1.04:
  Quantize, Length, Transpose (chromatic and diatonic), Slide and Controller fill on a selected
  region, with the event filter's kinds.
- C API (`sseq/sseq.h`) over all of the above, and a Swift package (`import SSeq`).
- No dependencies beyond the C++ standard library.

See `DESIGN.md` for the architecture and the mapping from the original engine's API.

## Build

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Requires CMake 3.20+ and a C++20 compiler (Apple clang 15+, GCC 11+, Clang 14+). The build
uses `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wconversion -Wsign-conversion` and is
warning-free on GCC and Clang; `.clang-format` and `.clang-tidy` define the style and the
static checks (`clang-tidy -p build src/*.cpp`).

The tests are self-contained: `tests/data/` holds a corpus of sequences generated from
`tests/corpus/sequences.cpp` and the files Cakewalk Apprentice 1.04 saved for them, which serve as
the oracle (see `tests/data/README.md`).

## Tool

```
build/sseqtool info     SONG.WRK          # summary
build/sseqtool dump     SONG.WRK          # chunk structure (also for .mid)
build/sseqtool roundtrip *.WRK            # read, write, compare bytes
build/sseqtool convert  SONG.WRK OUT.MID  # or .mid -> .wrk
build/sseqtool convert --plain SONG.WRK OUT.MID  # .mid without the Cakewalk port meta event
build/sseqtool normalize IN.WRK OUT.WRK   # rewrite in Cakewalk's chunk order
```

## Library

```cpp
#include <sseq/sseq.hpp>

sseq::Sequence sequence = sseq::wrk::readFile("sequence.wrk");
for (const sseq::Stream& t : sequence.streams)
    std::printf("%s: %zu events\n", t.name.c_str(), t.events.size());
sequence.stream(0).events.push_back(sseq::Event::note(0, 0, 60, 100, 120));
sseq::sortEvents(sequence.stream(0).events);
sseq::wrk::writeFile(sequence, "sequence.wrk");
sseq::smf::writeFile(sequence, "sequence.mid");
```

Playback goes through the engine with your clock and MIDI output:

```cpp
class MySink : public sseq::engine::MidiSink { /* send(), sendSysex() -> CoreMIDI, ALSA, ... */ };
sseq::engine::SteadyClock clock;
MySink sink;
sseq::engine::Transport transport(sequence, clock, sink, sseq::engine::PlaybackOptions::fromSequence(sequence));
transport.play();
while (transport.pump()) { /* every 10-20 ms */ }
```

## From Swift

`Package.swift` exposes the C API as the module `SSeq`:

```swift
// Package.swift: .package(url: "https://github.com/zaskar9/libsseq.git", from: "0.3.0")
import SSeq

let sequence = sseq_sequence_read_wrk(path)          // nil on failure, see sseq_last_error()
let streams = sseq_sequence_stream_count(sequence)
sseq_sequence_free(sequence)
```

The C header is `include/sseq/sseq.h`; `tests/capi_tests.c` shows every call in use.

## Status

v0.6: model, formats, display text, engine, recorder, editing (Quantize, Length, Transpose,
Slide, Controller fill), C API, Swift package, tool, tests. Planned: cut/copy/paste with the event
filter, checked against Cakewalk Apprentice 1.04. Platform adapters (a
CoreMIDI sink and clock, for example) belong to the application using the library.
