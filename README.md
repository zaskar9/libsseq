# libsseq

A small C++20 library for Cakewalk `.WRK` (version 2.0) and Standard MIDI Files, with the
functional scope of Cakewalk's `TTSSEQ.DLL` sequencer engine. MIT licensed. Used by the
Ensemble project.

- Read and write `.WRK` losslessly (byte-identical round trip of every reference file).
- Read and write `.MID` with the same mapping Cakewalk Apprentice 1.04 uses.
- Song model: tracks, events (notes with durations), tempo map, meter and key maps, SMPTE
  time format, markers, sysex banks, metronome and MIDI-thru settings, comments.
- Time conversions: ticks ↔ seconds ↔ measure:beat:tick ↔ SMPTE frames (including drop-frame).
- No dependencies beyond the C++ standard library.

See `DESIGN.md` for the architecture and the mapping from the original engine's API.

## Build

```
cmake -S . -B build -DSSEQ_TEST_DATA_DIR=/path/to/Cakewalk
cmake --build build
ctest --test-dir build
```

Requires CMake 3.20+ and a C++20 compiler (Apple clang 15+, GCC 11+, Clang 14+). The build
uses `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wconversion -Wsign-conversion` and is
warning-free on GCC and Clang; `.clang-format` and `.clang-tidy` define the style and the
static checks (`clang-tidy -p build src/*.cpp`).

The reference files used by the corpus test (the original Cakewalk sample songs and the probe
files saved by Cakewalk Apprentice) are not part of the repository; without them the corpus
test reports "skipping" and passes.

## Tool

```
build/sseqtool info     SONG.WRK          # summary
build/sseqtool dump     SONG.WRK          # chunk structure (also for .mid)
build/sseqtool roundtrip *.WRK            # read, write, compare bytes
build/sseqtool convert  SONG.WRK OUT.MID  # or .mid -> .wrk
build/sseqtool normalize IN.WRK OUT.WRK   # rewrite in Apprentice's chunk order
```

## Library

```cpp
#include <sseq/sseq.hpp>

sseq::Song song = sseq::wrk::readFile("song.wrk");
for (const sseq::Track& t : song.tracks)
    std::printf("%s: %zu events\n", t.name.c_str(), t.events.size());
song.track(0).events.push_back(sseq::Event::note(0, 0, 60, 100, 120));
sseq::sortEvents(song.track(0).events);
sseq::wrk::writeFile(song, "song.wrk");
sseq::smf::writeFile(song, "song.mid");
```

## Status

v0.1: model, formats, tool, tests. Planned: editing operations (quantize, transpose, slide,
length, controller fill, cut/copy/paste with event filter), engine (transport, scheduler),
CoreMIDI adapter and Swift package for the app.
