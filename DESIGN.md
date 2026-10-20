# libsseq design

libsseq is a C++20 library for reading, editing, saving and (later) playing Cakewalk `.WRK`
songs, with the functional scope of `TTSSEQ.DLL`, the sequencer engine of Cakewalk Apprentice
and Cakewalk Professional for Windows (1992–93). It follows the engine's concepts, not its
Win16 interface. It is a standalone library (MIT licensed) and the core of the Ensemble project,
whose Swift app provides the user interface and CoreMIDI.

Sources of truth (in the Cakewalk analysis folder, kept next to this repository as `../Cakewalk`): `analysis/REPORT.md` (static analysis), `ALTERNATIVES-LOG.md`
and `wine-exploration/WINE-EXPLORATION.md` (observed behaviour of the program under Wine).
No code was copied from any other implementation; the GPL `QWrk` parser was used only as a
reference to compare behaviour against.

## 1. Layers

| Layer | Namespace / files | Depends on | Status |
|---|---|---|---|
| Model | `sseq::Song`, `Track`, `Event`, `TempoMap`, `MeterMap`, `Timecode`, `Marker`, `SysxBank` (`song.hpp`, `maps.hpp`, `event.hpp`, `model.cpp`) | nothing | v0.1 |
| Formats | `sseq::wrk` (`wrk.hpp/.cpp`), `sseq::smf` (`smf.hpp/.cpp`) | Model | v0.1 |
| Engine | transport, scheduler, `MidiSink` / clock interfaces | Model | v0.2 (planned) |
| Platform | CoreMIDI sink and clock, Swift package | Engine | v0.2 (planned, in the app) |
| Tools | `sseqtool` | Formats | v0.1 |

Each layer is testable without the one above. The model has no I/O and no platform code.

## 2. Concept map from TTSSEQ.DLL

| TTSSEQ exports (groups) | libsseq |
|---|---|
| `SeqCreateSeq`, `SeqDestroySeq`, `SeqSet/GetSeqParam` | `Song` value type; parameters are fields (`ppq`, `vars`, `timecode`, ...) |
| `SeqCreateStream`, `SeqDestroyStream`, `SeqEnumStreams`, `SeqSet/GetStreamParam`, `SeqSet/GetStreamName`, `SeqCloneStream`, `SeqAdoptStream`, `SeqMergeStreams`, `SeqCloneStreamParams` | `Track` in `Song::tracks`; `Song::track(n)`, `findTrack(n)`; cloning and merging are `std::vector` operations on `Track::events` |
| `SeqTell/Eos/Seek/NxtEvt/GetEvt/GetEvtCopy/PutEvt/InsEvt/DelEvt/MoveEvt`, `SeqEvtAtTimeGE/GT`, `SeqStreamLen/Dur`, `SeqMeasHasEvts` | `Track::events` is a time-sorted `std::vector<Event>`; iterators and `std::lower_bound` replace the cursor; `Track::endTime()` |
| `SeqInsertRange`, `SeqDeleteRange`, `SeqDisplaceEarlier/Later`, `SeqTemporalSort` | range edits on `Track::events` (v0.2 editing operations); `sortEvents()` |
| `SeqRegisterEventKind`, `SeqCallEventProc` (Text, MCIcmd, Wave handled by the app) | `EventKind::Text / MciCommand / Wave` with a byte payload; the app interprets them |
| `SeqGetMaxPPQ`, `SeqSet/GetPPQ`, `SeqRescaleTime` | `Song::ppq`, `syncPpq()`; rescaling is a planned model operation |
| Tempo map: `SeqGet/SetTempo`, `SeqInsTempo`, `SeqDelTempo`, `SeqEnumTempos`, `SeqTempoIxByTick`, `SeqGetTempoAtIx`, `SeqRecalcTempoMap`, `SeqGetMin/MaxTempo` | `TempoMap` (`entries`, `at()`, `set()`, `remove()`, `tickToSeconds()`, `secondsToTick()`) |
| Tempo ratios: `SeqGet/SetTempoRatio`, `SeqSelectTempoRatio`, `SeqWhichTempoRatio`, `SeqTempoRatio2Str` | `Vars` bytes 58–61 (stored); playback ratio belongs to the engine |
| Meter map: `SeqGet/SetMeter`, `SeqInsMeter`, `SeqDelMeter`, `SeqEnumMeters`, `SeqMeterIxByTick`, `SeqMBT2Tick`, `SeqTick2MBT`, `SeqTick2Meas`, `SeqTick2MBTStr`, `SeqMBTStr2MBT`, `SeqTickNext/PrevMeas`, `SeqTickNext/PrevBeat` | `MeterMap` (`entries`, `atMeasure()`, `measureStart()`, `tickToMBT()`, `mbtToTick()`, `nextMeasure()`, `prevMeasure()`), `formatMBT()`, `parseMBT()`; key signatures in `Song::keyMap` |
| SMPTE: `SeqSet/GetFps`, `SeqSet/GetFrameOfs`, `SeqTick2Frame`, `SeqFrame2TickRndOpt`, `SeqFrame2HMSF`, `SeqHMSF2Frame`, `SeqTick2HMSFStr`, `SeqHMSFStr2HMSF`, `SeqHMSFStr2MBT` | `Timecode` (`rate`, `offsetFrames`, `secondsToFrames()`, `framesToHMSF()`, `hmsfToFrames()`), `formatHMSF()`, `parseHMSF()`; tick↔frame goes through `TempoMap::tickToSeconds()` |
| Markers: `SeqNumMarkers`, `SeqMarkerForTime`, `SeqIns/DelMarker`, `SeqSet/GetMarkerName/Time`, `SeqSortMarkers` | `Song::markers` (`std::vector<Marker>`) |
| Sysex banks: `SeqCreate/Destroy/EmptySysxBank`, `SeqEnumSysxBanks`, `SeqSet/GetSysxBankParam/Name`, `SeqGetSysxBankLen`, `SeqSysxBin2Asc/Asc2Bin`, `SeqSave/LoadSysxBank`, `SeqSend/ReceiveSysxBank` | `Song::sysxBanks` (`SysxBank`); file and transfer functions planned |
| Files: `SeqLoad/SaveCakewalkFile`, `SeqLoad/SaveMidiFile`, `SeqIOSeek/Read/Write`, `SeqGetFileError` | `wrk::read/write`, `smf::read/write`; errors are `sseq::Error` exceptions |
| Transport: `SeqSetPlayingSeq`, `SeqSetStandbySeq`, `SeqSet/GetSrcClock`, `SeqPlayStart/Stop`, `SeqRecordStart/Stop/Cleanup`, `SeqRewind`, `SeqSet/GetNow`, `SeqGetEnd`, `SeqSet/GetDur`, `SeqExpired`, `SeqIdle` | engine layer (v0.2): a `Transport` owning a `Scheduler`; `Vars::now()` persists the Now time |
| Input: `SeqStepRecSetup/Params/Term`, `SeqRecordEvent`, `SeqRecCount`, `SeqTxMidiMsg`, `SeqSetMidiMacro`, `SeqClearMidiMacroShift` | engine layer (v0.2+) |
| Devices: `SeqConfigMidi`, `SeqGetPortName`, `SeqWire`, `SeqSleep/Wake` | platform layer; port numbers in the model stay 0-based indices as in the file |
| `TIMERTRAP`, `RXTRAP` | replaced by timestamped look-ahead scheduling on CoreMIDI (design note in §5) |

## 3. The model

- `Tick` is a 64-bit signed integer. Files store 24-bit event times and 32-bit tempo/marker times.
- `Event` carries a kind, channel, two data fields, a duration (notes) and a byte payload
  (string events). Notes are single events with a duration, as in Cakewalk; note-on/off pairs
  exist only in SMF. Pitch wheel values are signed (−8192..8191).
- `Track::events` is kept sorted by time (stable). Writers sort defensively.
- `Vars` keeps the raw 89 bytes of the VARS chunk and exposes the known fields as accessors,
  so that fields whose meaning is not yet known survive a round trip.
- Reserved bytes in tempo, meter and marker records are carried in the entries for the same
  reason (the DOS-era files store cached values there).
- `Song::layout` records the chunk sequence of the file that was read. `wrk::write` in
  `Preserve` mode replays it, serializing modelled chunks from the model and unknown chunks
  verbatim. This gives a byte-identical round trip for every reference file, including the
  DOS-era ones with legacy chunks. `Normalize` mode writes the chunk order Apprentice 1.04
  uses and drops legacy chunks, which is what Apprentice itself does on save.

## 4. File formats

The `.WRK` 2.0 layout is specified in `../Cakewalk/analysis/REPORT.md` §6 and verified in
`wine-exploration/WINE-EXPLORATION.md` §3. The SMF mapping is in `ALTERNATIVES-LOG.md`.
Two details found while matching Apprentice's SMF output byte for byte:

- The time-0 parameter messages (Program Change, CC7, CC10) are written with explicit status
  bytes; running status starts with the first stream event.
- Simultaneous note-offs are written in an order that is not consistent between cases
  (sometimes the later-started note first, sometimes the earlier one), which looks like an
  unstable sort inside Apprentice. libsseq writes note-offs before note-ons at the same tick
  and otherwise keeps a deterministic order. The corpus test therefore compares SMF output
  semantically (same events at the same ticks per track), not byte for byte.
- On import, a note-off is paired with the most recent open note of the same key (LIFO),
  which is what Apprentice does (verified against its own import of `out10.mid`).

Not yet covered: the payload of Wave events (never produced in the observed sessions), chunk 25
(a Cakewalk Pro Faders-view state; preserved verbatim), later format versions (chunks 36, 44,
45, 49, 55, 74 and others; preserved verbatim, not interpreted), sysex bank files, `.MFF`.

## 5. Engine (planned, v0.2)

- `MidiSink` interface: `send(timestamp, bytes)` for short messages and sysex; `flush()`.
- `Clock` interface: `now()` in seconds, monotonic.
- `Scheduler`: converts ticks to seconds through the tempo map and emits events with absolute
  timestamps a configurable look-ahead (e.g. 50–100 ms) ahead of the clock; tempo changes during
  playback re-anchor the conversion at the change point. Note-offs are scheduled from note
  durations; a "panic" sends all-notes-off.
- `Transport`: play/stop/rewind, Now time, loop/auto-shuttle, metronome, mute/solo, Key+/Vel+/
  Time+ applied at output as Apprentice does, patch/volume/pan sent at start and on seek
  ("patch search-back").
- CoreMIDI adapter in the Swift app: `MIDITimeStamp` from `mach_absolute_time`, one output
  port per Cakewalk port index.

## 6. Build and test

- `cmake -S . -B build && cmake --build build && ctest --test-dir build`
- `SSEQ_TEST_DATA_DIR` (default `../Cakewalk`) points to the Cakewalk folder (`expanded/*.WRK`,
  `analysis/wine-exploration/saved-files/*.wrk|*.mid`); the reference files are not part of the
  repository. Without them the corpus test is skipped.
- `sseqtool dump|info|roundtrip|convert|normalize`.
- Swift consumption: a thin C API or Swift C++ interop (Xcode 15+) wrapped in a SwiftPM
  package is the next step for the app.
