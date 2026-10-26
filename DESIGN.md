# libsseq design

libsseq is a C++20 library for reading, editing, saving and (later) playing Cakewalk `.WRK`
sequences, with the functional scope of `TTSSEQ.DLL`, the sequencer engine of Cakewalk Apprentice
and Cakewalk Professional for Windows (1992–93). It follows the engine's concepts, not its
Win16 interface. It is a standalone, MIT-licensed library with no dependencies beyond the C++
standard library; applications add the platform pieces (MIDI I/O, user interface) on top.

Sources of truth: the structure of the original binaries (headers, resources, exported and imported
symbol names, strings), the text of the program's help file, and the behaviour of Cakewalk
Apprentice 1.04 observed under Wine (differential saves, screenshots). That analysis is kept outside
this repository; the oracle files it produced are in `tests/data/`. No code was copied from any
other implementation; the GPL `QWrk` parser was used only as a reference to compare behaviour against.

Naming: "Cakewalk" is the program, its formats and its behaviours; Apprentice and Professional are
editions of it built from one code base. "Cakewalk Apprentice 1.04" is written out in full where a
statement is about that build, which produced every oracle file in `tests/data/` and showed the
limits recorded there; "Cakewalk Professional" marks features that edition alone has.

Terminology follows the engine, not the application: a **sequence** (`SeqCreateSeq`) holds
**streams** (`SeqCreateStream`) of **events**; the Cakewalk UI shows each stream as a track and calls
the sequence a work (`.WRK`); an application built on libsseq can do the same. "Track" appears in this code base
only where it names something in a file format: the `TRACK` chunk of `.WRK` and the `MTrk` chunks
of a Standard MIDI File, which `SeqSaveMidiFile` maps onto streams (track 0 carries the maps and
markers, every non-empty stream becomes one track).

## 1. Layers

| Layer | Namespace / files | Depends on | Status |
|---|---|---|---|
| Model | `sseq::Sequence`, `Stream`, `Event`, `TempoMap`, `MeterMap`, `Timecode`, `Marker`, `SysxBank` (`sequence.hpp`, `maps.hpp`, `event.hpp`, `model.cpp`) | nothing | v0.1 |
| Formats | `sseq::wrk` (`wrk.hpp/.cpp`), `sseq::smf` (`smf.hpp/.cpp`) | Model | v0.1 |
| Display | `sseq::display` (`display.hpp/.cpp`): key names, durations, kind names, General MIDI names, Event-list text | Model | v0.4 |
| Engine | `sseq::engine` (`engine.hpp/.cpp`, `recorder.hpp/.cpp`): `Transport`, `Recorder`, `render`, `MidiSink` / `Clock` interfaces | Model | v0.2, v0.3 |
| C API | `sseq.h` / `capi.cpp`: flat C interface over everything above, for Swift | all | v0.3 |
| Swift package | `Package.swift`: the C API as module `SSeq` | C API | v0.3 |
| Platform | MIDI sink and clock (CoreMIDI, ALSA, ...), MIDI input, UI | C API or Swift package | in the application |
| Tools | `sseqtool` | Formats | v0.1 |

Each layer is testable without the one above. The model has no I/O and no platform code.

## 2. Concept map from TTSSEQ.DLL

| TTSSEQ exports (groups) | libsseq |
|---|---|
| `SeqCreateSeq`, `SeqDestroySeq`, `SeqSet/GetSeqParam` | `Sequence` value type; parameters are fields (`ppq`, `vars`, `timecode`, ...) |
| `SeqCreateStream`, `SeqDestroyStream`, `SeqEnumStreams`, `SeqSet/GetStreamParam`, `SeqSet/GetStreamName`, `SeqCloneStream`, `SeqAdoptStream`, `SeqMergeStreams`, `SeqCloneStreamParams` | `Stream` in `Sequence::streams`; `Sequence::stream(n)`, `findStream(n)`; cloning and merging are `std::vector` operations on `Stream::events` |
| `SeqTell/Eos/Seek/NxtEvt/GetEvt/GetEvtCopy/PutEvt/InsEvt/DelEvt/MoveEvt`, `SeqEvtAtTimeGE/GT`, `SeqStreamLen/Dur`, `SeqMeasHasEvts` | `Stream::events` is a time-sorted `std::vector<Event>`; iterators and `std::lower_bound` replace the cursor; `Stream::endTime()` |
| `SeqInsertRange`, `SeqDeleteRange`, `SeqDisplaceEarlier/Later`, `SeqTemporalSort` | range edits on `Stream::events` (v0.2 editing operations); `sortEvents()` |
| `SeqRegisterEventKind`, `SeqCallEventProc` (Text, MCIcmd, Wave handled by the app) | `EventKind::Text / MciCommand / Wave` with a byte payload; the app interprets them |
| `SeqGetMaxPPQ`, `SeqSet/GetPPQ`, `SeqRescaleTime` | `Sequence::ppq`, `syncPpq()`; rescaling is a planned model operation |
| Tempo map: `SeqGet/SetTempo`, `SeqInsTempo`, `SeqDelTempo`, `SeqEnumTempos`, `SeqTempoIxByTick`, `SeqGetTempoAtIx`, `SeqRecalcTempoMap`, `SeqGetMin/MaxTempo` | `TempoMap` (`entries`, `at()`, `set()`, `remove()`, `tickToSeconds()`, `secondsToTick()`) |
| Tempo ratios: `SeqGet/SetTempoRatio`, `SeqSelectTempoRatio`, `SeqWhichTempoRatio`, `SeqTempoRatio2Str` | `Vars` bytes 58–61 (stored); playback ratio belongs to the engine |
| Meter map: `SeqGet/SetMeter`, `SeqInsMeter`, `SeqDelMeter`, `SeqEnumMeters`, `SeqMeterIxByTick`, `SeqMBT2Tick`, `SeqTick2MBT`, `SeqTick2Meas`, `SeqTick2MBTStr`, `SeqMBTStr2MBT`, `SeqTickNext/PrevMeas`, `SeqTickNext/PrevBeat` | `MeterMap` (`entries`, `atMeasure()`, `measureStart()`, `tickToMBT()`, `mbtToTick()`, `nextMeasure()`, `prevMeasure()`), `formatMBT()`, `parseMBT()`; key signatures in `Sequence::keyMap` |
| SMPTE: `SeqSet/GetFps`, `SeqSet/GetFrameOfs`, `SeqTick2Frame`, `SeqFrame2TickRndOpt`, `SeqFrame2HMSF`, `SeqHMSF2Frame`, `SeqTick2HMSFStr`, `SeqHMSFStr2HMSF`, `SeqHMSFStr2MBT` | `Timecode` (`rate`, `offsetFrames`, `secondsToFrames()`, `framesToHMSF()`, `hmsfToFrames()`), `formatHMSF()`, `parseHMSF()`; tick↔frame goes through `TempoMap::tickToSeconds()` |
| Markers: `SeqNumMarkers`, `SeqMarkerForTime`, `SeqIns/DelMarker`, `SeqSet/GetMarkerName/Time`, `SeqSortMarkers` | `Sequence::markers` (`std::vector<Marker>`) |
| Sysex banks: `SeqCreate/Destroy/EmptySysxBank`, `SeqEnumSysxBanks`, `SeqSet/GetSysxBankParam/Name`, `SeqGetSysxBankLen`, `SeqSysxBin2Asc/Asc2Bin`, `SeqSave/LoadSysxBank`, `SeqSend/ReceiveSysxBank` | `Sequence::sysxBanks` (`SysxBank`); file and transfer functions planned |
| Files: `SeqLoad/SaveCakewalkFile`, `SeqLoad/SaveMidiFile`, `SeqIOSeek/Read/Write`, `SeqGetFileError` | `wrk::read/write`, `smf::read/write`; errors are `sseq::Error` exceptions |
| Transport: `SeqSetPlayingSeq`, `SeqSetStandbySeq`, `SeqSet/GetSrcClock`, `SeqPlayStart/Stop`, `SeqRecordStart/Stop/Cleanup`, `SeqRewind`, `SeqSet/GetNow`, `SeqGetEnd`, `SeqSet/GetDur`, `SeqExpired`, `SeqIdle` | engine layer (v0.2): a `Transport` owning a `Scheduler`; `Vars::now()` persists the Now time |
| Input: `SeqStepRecSetup/Params/Term`, `SeqRecordEvent`, `SeqRecCount`, `SeqTxMidiMsg`, `SeqSetMidiMacro`, `SeqClearMidiMacroShift` | engine layer (v0.2+) |
| Devices: `SeqConfigMidi`, `SeqGetPortName`, `SeqWire`, `SeqSleep/Wake` | platform layer; port numbers in the model stay 0-based indices as in the file |
| `TIMERTRAP`, `RXTRAP` | replaced by timestamped look-ahead scheduling on CoreMIDI (design note in §5) |

### Not in TTSSEQ: application functions hosted in the library

Two groups in libsseq have no counterpart among the engine's exports; in the original they live in
the application (WCWLITE.EXE). They are kept in their own namespaces so the boundary stays visible:

| libsseq | In the original | Why it is here |
|---|---|---|
| `sseq::display` (`display.hpp`): key names, duration text, event kind names, General MIDI names, Event-list columns | the Event list and Track/Measure views, the string table, PATCHES.INI | pure functions of the model, pinned down by Event-list screenshots; the same text is needed by every view and platform |
| `sseq::edit` (`edit.hpp`, v0.5 to 0.7): Quantize, Length, Transpose, Slide, Controller fill, the event filter, Cut/Copy/Paste with a clipboard, one-level Undo | the Edit menu (`edit1.c`, `cv.c`, `evtfilt.c`); the engine offers only `SeqInsertRange`, `SeqDeleteRange`, `SeqDisplaceEarlier/Later`, `SeqTemporalSort`, `SeqRescaleTime` | pure data transformations whose exact arithmetic is established with Cakewalk Apprentice 1.04 oracles under Wine and tested here on Linux; reusable from any client |

The device layer goes the other way: `SeqConfigMidi`, `SeqGetPortName`, the timer and the
`midiIn`/`midiOut` calls are inside TTSSEQ, but in libsseq they belong to the application
(`Clock`, `MidiSink`), because the library has to build without a platform MIDI API.

## 3. The model

- `Tick` is a 64-bit signed integer. Files store 24-bit event times and 32-bit tempo/marker times.
- `Event` carries a kind, channel, two data fields, a duration (notes) and a byte payload
  (string events). Notes are single events with a duration, as in Cakewalk; note-on/off pairs
  exist only in SMF. Pitch wheel values are signed (−8192..8191).
- `Stream::events` is kept sorted by time (stable). Writers sort defensively.
- `Vars` keeps the raw 89 bytes of the VARS chunk and exposes the known fields as accessors,
  so that fields whose meaning is not yet known survive a round trip.
- Reserved bytes in tempo, meter and marker records are carried in the entries for the same
  reason (the DOS-era files store cached values there).
- `Sequence::layout` records the chunk sequence of the file that was read. `wrk::write` in
  `Preserve` mode replays it, serializing modelled chunks from the model and unknown chunks
  verbatim. This gives a byte-identical round trip for every reference file, including the
  DOS-era ones with legacy chunks. `Normalize` mode writes the chunk order Cakewalk
  uses and drops legacy chunks, which is what Cakewalk itself does on save.

Key signatures: Cakewalk keeps meter and key in one list (the METERKEY chunk), so in its model a
key change is also a meter entry restating the meter in force. libsseq keeps `meterMap` and
`keyMap` apart, but follows that rule where the maps come from Cakewalk: the WRK reader takes the
meter entries from the METER chunk (which has one at every key change), the SMF import adds a
meter entry at every key signature, and the SMF export writes a time signature only where the
meter actually changes, as Cakewalk's does. A sequence built in code should define a meter entry
at each key change to round-trip unchanged (the corpus does).

## 4. File formats

The `.WRK` 2.0 layout is specified in `../Cakewalk/analysis/REPORT.md` §6 and verified in
`wine-exploration/WINE-EXPLORATION.md` §3. The SMF mapping is in `ALTERNATIVES-LOG.md`.
Two details found while matching Cakewalk's SMF output byte for byte:

- The time-0 parameter messages (Program Change, CC7, CC10) are written with explicit status
  bytes; running status starts with the first stream event.
- Simultaneous note-offs are written in an order that is not consistent between cases
  (sometimes the later-started note first, sometimes the earlier one), which looks like an
  unstable sort inside Cakewalk. libsseq writes note-offs before note-ons at the same tick
  and otherwise keeps a deterministic order. The corpus test therefore compares SMF output
  semantically (same events at the same ticks per stream), not byte for byte.
- On import, a note-off is paired with the most recent open note of the same key (LIFO),
  which is what Cakewalk does (verified against Cakewalk Apprentice 1.04's own import of `out10.mid`).
- Import of a track that mixes channels (a format 0 file; verified with Cakewalk Apprentice 1.04
  importing the classic General MIDI demo `canyon.mid`): the stream gets no forced channel (a
  single-channel track gets its channel), text meta events in the first track become markers,
  and the resolution is kept (Apprentice rescales to its 120 ppq timebase, truncating ticks).
  Apprentice takes every Program Change, CC7 and CC10 at tick 0 as the stream's patch, volume
  and pan, last one wins whatever the channel, and drops them, which loses the other channels'
  programs on a mixed track; libsseq does that only for single-channel tracks and keeps the
  events otherwise (`ReadOptions::absorbParametersOnMixedTracks` restores Apprentice's
  behaviour).

Not yet covered: the payload of Wave events (never produced in the observed sessions), chunk 25
(a Cakewalk Professional Faders-view state; preserved verbatim), later format versions (chunks 36, 44,
45, 49, 55, 74 and others; preserved verbatim, not interpreted), sysex bank files, `.MFF`.

## 4a. Display text (v0.4)

`display.hpp` holds the text forms Cakewalk's views use, observed in Cakewalk Apprentice 1.04's
Event list: key names with C 5 = 60 (`C 5`, `G#4`, `G 10`), note durations as plain ticks below one
quarter note and `quarters:ticks` above (`1:080`; the unit is the PPQ quarter, not the meter beat),
the kind names (`Note`, `KeyAft`, `ChanAft`, `Contrl`, `Patch`, `Wheel`, `Sysx`, `Text`, `MCICmd`,
`Wave`), the Chn column (`n/a` for sysex and string events) and the Values column per kind. Patch
names come from the General MIDI level 1 list in the MIDI specification, not from Cakewalk's
PATCHES.INI. SMPTE display rounds to the nearest frame, as the Event list does.

## 4b. Editing (v0.5 to v0.7)

`sseq::edit` holds the Edit menu's commands as pure transformations of a `Sequence`. Each takes a
`Selection` (stream numbers and a From..Thru range; an event is selected by its start time, both
ends inclusive, as the oracle with From 2:01:000 Thru 3:01:000 showed) and an `EventFilter` (which
kinds to touch; each command has its own default). Streams whose events moved are re-sorted with
a stable sort, so events that land on the same tick keep their order.

Quantize, as established with Cakewalk Apprentice 1.04 at 120 ppq (`tests/data/edit`):

- Resolutions are whole .. thirty-second and the eighth, sixteenth and thirty-second triplets
  (two thirds of the straight value): `resolutionTicks()`.
- Starting times: `floor((t + res / 2) / res) * res` with integer `res / 2`, so exactly half rounds
  up (offset 15 of 30 goes up; of 15, offset 7 goes down and 8 up). Strength moves the event that
  percentage of the distance, truncated toward zero (723 → 722, 3615 → 3622 at 50 %).
- Durations: `max(res, floor((d + res / 2) / res) * res) - 1`, so a note is never shorter than
  `res - 1` and never exactly a multiple ("so that notes do not run into each other", the help
  says). Strength does not apply to durations.
- Without the event filter only notes are touched; other kinds keep their times.

Event filter (toward v0.7): Cakewalk's "Use event filter" dialog has an Include box and
inclusive field ranges, each with a "Not", for the six MIDI channel kinds (Note: key, velocity,
duration; KeyAft: key, value; Control: number, value; Patch: number; ChanAft: amount; Wheel:
value), plus channel, beat-within-measure and tick-within-beat ranges for "any kind of event".
All ranges of a kind must hold. Sysex, Text, MCI and Wave events are not covered and always pass
a filter made in the dialog, even with "Nothing" or a tick range. `EventFilter` holds all of
this; `includes(event, meterMap)` evaluates it. Each command keeps its own default filter when
the dialog is not used.

Order within a tick: after an edit, Cakewalk's order among events on the same tick is not the
file order (its sort is evidently not stable), so the edit tests compare same-tick events as a
set. libsseq keeps the file order (stable sort).

Length (v0.6): start times become `from + scale(t - from)` and note durations `scale(d)`, with
`scale(x) = (x * percent + 50) / 100` (nearest, half up; no minimum, so 1 at 33 % is 0). Every
event kind is affected by default. Events after Thru do not move.

Transpose (v0.6): Note and KeyAft keys move by half-steps; a result outside 0..127 is moved by
octaves back in (127 + 5 = 120, 0 - 7 = 5). With "Diatonic math" the amount is in steps of the
major scale of the key signature at the event's measure: the half-step distance from the tonic
(`alterations * 7 mod 12`) maps to a degree by Cakewalk's table `0 1 1 2 2 3 3 4 4 5 6 6` (the
non-scale tones at 1 and 3 half-steps go to the degree above, 6 and 8 to the one below, 10 to the
one above), the steps are added, and the degree becomes a key again; the same folding applies.

Slide (v0.6): the region's events move by the amount; "Measures" means lengths of the measure at
From (`measuresToTicks`, verified with a constant meter only). The amount is clamped to `-from`,
so a region never moves before 1:1:000; Cakewalk Apprentice 1.04 does this silently, although its
help promises an error message.

Controller fill (v0.6): into one stream (the current track in Cakewalk). Events of the same kind,
controller number and channel in From..Thru are removed. Then the values from Begin to End are
walked one by one and each is placed at `from + floor(|v - begin| * (thru - from) / |end - begin|)`;
a value whose tick is already taken is skipped. So 0..127 over 120 ticks gives 121 events (no 1,
no 19, ...), 50..60 over 30 ticks gives 11 events three ticks apart, and -8192..8191 over 480
ticks gives one wheel event per tick. New events go before existing ones on the same tick.
Cakewalk writes 1 into the unused duration field of the events it inserts (as its MIDI import
does); libsseq writes 0 and the comparison ignores the field.

Cut, Copy, Paste (v0.7): the `Clipboard` holds the region's events per source stream with times
relative to From, its tempo entries, and, for a region of whole measures (From starts one, Thru
ends one; the dialog greys the box otherwise), the meter and key per measure, entry 0 restating
the state at From. Its `length` is Thru - From + 1, which is also the step between repetitions.
Copy takes events by start time (a note crossing into the region is not taken, one crossing out
keeps its duration). Paste puts each repetition's events on their source streams (or the one
stream of Paste to One track) at To + offset; the destination range is `To .. To + length *
repetitions - 1` on the affected streams (the selected ones, or the destination of Paste to One
track): Replace removes its events and tempo entries first, Slide over moves everything from To
on later by the span (the tempo and meter/key maps too when they are pasted), Blend does nothing.
Cakewalk allows "Tempo changes" with Replace and Slide over only. Pasted meter/key entries go to
the measure of To onward and replace what is there. Cut is Copy plus removal; with "Delete hole"
the selected streams' later events, and the maps when cut, move earlier by the length (Cakewalk
Apprentice 1.04 moves the tempo map by one tick less; libsseq moves it by the length). The
clipboard converts to a sequence (`toSequence`) for the Standard MIDI File image Cakewalk shares
with other applications, and back (`fromSequence`).

Undo (v0.7): `Undo` keeps the sequence as it was before the last command and swaps it back on
`apply()`; a second `apply()` redoes, and `label()` is the menu text ("Undo Quantize", "Redo
Quantize"). One level, as in Cakewalk. The C API does the same with `sseq_sequence_copy` and
`sseq_sequence_swap`.

## 5. Engine (v0.2, matched to captured output in v0.8)

`include/sseq/engine.hpp`, namespace `sseq::engine`. Platform-neutral: the application supplies
the two things that differ between platforms and the engine does everything else.

- `Clock`: monotonic `now()` in seconds. `SteadyClock` wraps `std::chrono::steady_clock`; tests
  use a fake clock they advance by hand.
- `MidiSink`: receives `MidiMessage{time, port, size, data[3]}` and `sendSysex(time, port,
  bytes)` with absolute timestamps up to the look-ahead in the future, plus `flush()` at the end
  of each pump. A CoreMIDI sink converts `time` to a `MIDITimeStamp`; a test sink records.
- `Transport(sequence, clock, sink, options)`: `play`/`stop`/`rewind`/`seek`, `now()` in ticks,
  `setMuted`/`setSolo` (mute, solo), `setTempoRatio` (the 0.50/1.00/2.00 buttons), an optional
  loop range (auto-shuttle), and a string-event handler for Text/MCI/Wave events. The caller
  calls `pump()` every 10–20 ms; each pump schedules everything up to `clock.now() + lookAhead`
  so the caller's timer jitter does not reach the MIDI port.
- Playlist: on construction the streams are flattened into one list of outputs sorted by tick;
  at one tick the streams come in stream-number order and each stream's events in stored order,
  with sysex after the short messages. Key+ (folded by octaves into 0..127), Vel+ (clamped to
  1..127), Time+ (an event moved before the start plays at tick 0), forced channel, port and stream
  loop repetitions (`reps`, period = whole measures) are applied here, as Cakewalk applies them at
  output time rather than in the data.
- Note-offs (v0.8): a note-on puts its note-off into a binary min-heap on time that reproduces the
  original engine's queue, including the order of equal times; due note-offs go before the
  events of their tick. A note-on for a sounding key sends a note-off first; a note-off for a key
  that is no longer sounding is dropped.
- Tick ↔ clock: `tempoMap.tickToSeconds` / `secondsToTick` scaled by the tempo ratio, anchored
  at the last play/seek/ratio change, so tempo changes inside the sequence are exact and ratio
  changes take effect immediately.
- Start: stream parameters (patch, volume, pan) for every audible stream with events, in
  stream-number order; then, with `chase` (Cakewalk's search-back), for every audible stream that
  still has events at or after the position, its last patch, pitch wheel, CC 1, CC 64 and CC 7
  before it. `openMessages()` is what loading a file sends: the stream parameters of all streams.
- Stop and the end of the sequence: MIDI Stop, the metronome click's note-off, then per channel
  the sounding notes' offs in key order and All Notes Off; with `zeroControllersOnStop` then per
  channel pitch wheel centre, CC 1 = 0, CC 64 = 0, CC 121 (`panic()` always adds that reset). Seek
  and the loop wrap only end the sounding notes. Messages handed to the sink ahead of the clock
  and dropped by `discardScheduled()` are taken into account, so a dropped note-on gets no
  note-off and a note whose note-off was dropped gets one.
- MIDI sync out (v0.8, `midiClock`): Start (Continue when not at tick 0, unless
  `startNeverContinue`), Clock every 1/24 quarter note before the events of its tick, Stop; with
  `clockWhileStopped`, `pump()` keeps sending Clock at the tempo while stopped. No Song Position
  Pointer, as in the original.
- All of the above was checked against captures of Cakewalk Apprentice 1.04's MIDI output
  (`tests/data/engine/README.md`): same messages, same order, times within 25 ms.
- Engine generations (v0.8.1, `engine`): `EngineVersion::v2_10` (Apprentice, the default),
  `v3_00` (Cakewalk Professional 2.00: search-back also chases pan and the streams that have
  ended; the click ends one clock before its duration; the accent clicks the first beat at
  velocity 127) and `v1_01` (the 1992 Professional demo: an event moved before tick 0 is dropped;
  no MIDI sync). Each is checked against its own captures.
- Program behaviour (v0.8.1): `PlaybackOptions::fromSequence(sequence, recording,
  Program::Professional)` applies the file's auto-stop, auto-rewind and auto-restart, which
  Apprentice ignores. All three (or rewind and restart) give the loop range: play to `loopThru`
  (the end when unset), whose events do not play, then continue at `loopFrom` with no stop
  output. Otherwise `stopAt` stops there and `rewindTo` moves Now after stopping by itself. A
  shuttle applies only when playback starts before its stop time. `autoSendBanks()` lists the
  banks Professional sends on File > Open.
- `render(sequence, options, from, thru)`: the same scheduling offline, returning the messages with
  times in seconds from `from`; this is what the tests check and what an export-to-audio or
  SMF-0 writer would consume.
- Metronome (v0.3): with `metronome` on, the sequence's metronome note (port, channel, key,
  velocity, duration from the METRONOME chunk) plays on every beat of the meter map while
  playing, the end beat included, interleaved in time order with the sequence's messages; a
  velocity or duration of 0 disables it, as Cakewalk's help says. The MIDI click has the same
  velocity on every beat (`metronomeAccent` does not change it; Cakewalk's accent only shows on
  its PC-speaker click), except on engine 3.00. The duration counts MIDI clocks (1/24 quarter note): the click ends two
  clocks before that count (duration 1 wraps round to 23 clocks), never later than one clock
  before the next measure, and a click still sounding at the next beat ends right before it. `countInMeasures` plays whole measures of
  clicks before the start position, in the meter and tempo at that position; `now()` stays at
  the start and `countingIn()` is true until the sequence begins. `PlaybackOptions::fromSequence(sequence,
  recording)` takes the playback or the recording metronome flag, the accent flag, the sync flags,
  and for recording the count-in and `stopAtEnd = false` (recording runs past the end of the
  material).
- Loop count 9999 means "keep looping as long as the non-looping streams play" (help: Track/Loop):
  the stream repeats until the end of the longest stream with loop count 1 and is cut there.
- `panic()`: stop, with All Notes Off and the controller reset on every channel of every port
  (the control bar's panic button).
- `Recorder(transport, options)` (`recorder.hpp`): the platform layer hands it timestamped input
  messages; it converts their times to ticks through `Transport::tickAt()` (so input during the
  count-in or while stopped is ignored), pairs note-offs with the most recent open note of the
  same key (LIFO, as Cakewalk does on import), applies an event-kind filter and an optional
  punch range, closes held notes on `finish()`, and merges the take into a stream with `commit()`
  (sound on sound, Cakewalk's "Keep take?") or `commitReplacing()` (overwrite). Cakewalk Apprentice 1.04
  itself has no punch-in or multi-take (help: those are Cakewalk Professional features), so the
  punch range is a library extension.
- Not in the engine: MIDI thru (low-latency routing of input to output belongs to the platform
  layer; the settings are in the model), step record (a dialog on top of the model).

## 5a. C API and Swift package (v0.3)

`include/sseq/sseq.h` is a flat C interface over the model, the formats, the engine and the
recorder, implemented in `src/capi.cpp`, so that Swift (or any other language) can use the
library without C++ interop. Design points:

- Opaque handles `sseq_sequence`, `sseq_transport`, `sseq_recorder` with `_new`/`_read*` and `_free`.
  Functions that can fail return 0 or NULL and set a per-thread message read with
  `sseq_last_error()`. Exceptions never cross the boundary.
- Plain structs for events, stream parameters, map entries and options, with `-1`/`0` for the
  model's `std::optional` fields and `has_*` flags where a sentinel would be ambiguous. Streams
  are addressed by index in file order; events by index within a stream and kept in time order by
  the insert/set functions. String payloads and names are returned as pointers valid until the
  sequence changes.
- Files are read and written both by path and in memory (`sseq_data`), for document-based apps.
- The transport takes `sseq_transport_callbacks`: `now`, `send`, `send_sysex`, `flush` and
  `string_event` function pointers with a context. The C++ `Clock` and `MidiSink` adapters live
  inside `capi.cpp`.
- `tests/capi_tests.c` is compiled as C11 with the full warning set, so the header stays usable
  from C.

`Package.swift` compiles `src/` as the SwiftPM target `SSeq` with `swift/include/module.modulemap`
and the umbrella header `swift/include/SSeq.h`, which includes `sseq.h`. The C++ headers stay out
of the module. A Swift application depends on the package and adds its MIDI sink and clock (CoreMIDI),
the input path and whatever wrapper it wants on its side.

## 6. Build and test

- `cmake -S . -B build && cmake --build build && ctest --test-dir build`
- Unit tests (`tests/unit_tests.cpp`): model and time conversions, write/read of a new sequence,
  SMF round trip, a hand-built DOS-era file fixture (old TEMPO, MEMRGN, chunk 25) and malformed
  input.
- Engine tests (`tests/engine_tests.cpp`): offline rendering of hand-built sequences (ordering,
  Key+/Vel+/Time+, mute/solo, tempo changes and ratio, loops, stream repetitions including 9999,
  sysex and string events, metronome and accent), and a `Transport` driven by a fake clock and a
  recording sink (look-ahead windows, stop cutting notes and zeroing controllers, panic, resume,
  seek with chase, count-in, `tickAt`); the `Recorder` with note pairing, filter, punch range
  and both commit modes; finally every corpus sequence is rendered and its message count checked
  against the model.
- C API tests (`tests/capi_tests.c`, compiled as C11): model editing, maps and conversions,
  settings, memory and file round trips through both formats, transport with callbacks,
  rendering and recording through the C interface.
- Corpus tests (`tests/corpus_tests.cpp`, data in `tests/data/`): the sequences defined in
  `tests/corpus/sequences.cpp` were written by libsseq, loaded into Cakewalk Apprentice 1.04 under
  Wine and saved by it as `.WRK` and `.MID`, and the `.MID` was imported and saved again. The test
  checks the generator is reproducible, the Cakewalk files round-trip byte for byte, the model
  read from them equals the sequence they were generated from, and export and import agree with
  Cakewalk's. `tests/data/README.md` has the provenance; `gencorpus` regenerates `src/`.
- Edit tests (`tests/edit_tests.cpp`, data in `tests/data/edit/`): the arithmetic and selection
  rules of every Edit command on hand-built sequences, then every oracle (`QUANT.WRK` or
  `XPOSE.WRK` after Cakewalk's own command with the listed settings) compared event by event
  with ours.
- `sseqtool dump|info|roundtrip|convert|normalize`.
- Swift consumption: a thin C API or Swift C++ interop (Xcode 15+) wrapped in a SwiftPM
  package is the next step for the app.
