# Test corpus

Reference files for the corpus test (`tests/corpus_tests.cpp`). Every file here was produced from
the song definitions in `tests/corpus/sequences.cpp`; nothing in this directory comes from Cakewalk's
own sample files or from any other third party.

## Directories

| Directory | Produced by | Role |
|---|---|---|
| `src/` | libsseq (`gencorpus`), normalized layout | the sequence as written by the library |
| `app/NAME.WRK` | Cakewalk Apprentice 1.04, File > Save As (Cakewalk) after loading `src/NAME.WRK` | oracle for the `.WRK` format |
| `app/NAME.MID` | Cakewalk Apprentice 1.04, File > Save As (MIDI File) of the same sequence | oracle for the SMF export mapping |
| `mid2wrk/NAME.WRK` | Cakewalk Apprentice 1.04, loading `app/NAME.MID` and saving as Cakewalk | oracle for the SMF import mapping |
| `edit/NAME_X.WRK` | Cakewalk Apprentice 1.04, loading `src/NAME.WRK`, Mark > Select All, an Edit menu command, File > Save As | oracle for `sseq::edit` (`tests/edit_tests.cpp`) |

The Cakewalk files were produced by running the original program under Wine 9.0 (Linux
x86_64, Win16 subsystem) with the UI driven by xdotool. File names keep the 8.3 form Windows 3.1 needs.

To regenerate `src/` after changing a sequence definition: `build/gencorpus tests/data/src`. The
Cakewalk files then have to be produced again for the changed songs, otherwise the corpus test
reports the difference.

## Sequences

The synthetic sequences exercise the model and the formats; the realistic ones are simple
arrangements of public-domain music, transcribed for this corpus (melodies by ear and from
memory, so they are not authoritative editions).

| File | Description |
|---|---|
| `SYN_MIN.WRK` | Minimal sequence: one stream, one note, defaults everywhere |
| `SYN_KIND.WRK` | Every event kind: note, key and channel aftertouch, controller, patch, wheel, sysex, MCI, text |
| `SYN_PARM.WRK` | Stream parameters: sparse numbers, channel none, Key+/Vel+/Time+, loop, mute, patch, volume, pan |
| `SYN_MAPS.WRK` | Tempo, meter and key changes, markers, SMPTE format and offset, metronome, thru, VARS flags, comments |
| `SYN_OVLP.WRK` | Overlapping same-key notes, simultaneous note-offs, 1-tick durations, chords, large times |
| `SYN_DENS.WRK` | Dense sequence: 8 streams with 1500 notes each and one stream with 9000 notes (STREAM chunk over 64 KB) |
| `SYN_STRS.WRK` | String events: lyric-style text on every beat, a 127-byte text (Cakewalk Apprentice 1.04's maximum), MCI commands |
| `SYN_WAVE.WRK` | A Wave event with a small payload (format unverified; kept separate on purpose) |
| `ROWBOAT.WRK` | Row, Row, Row Your Boat (traditional, 19th c.) as a four-part round in 6/8 |
| `FRERE.WRK` | Frere Jacques (traditional) as a round made with identical streams and Time+ offsets |
| `ODETOJOY.WRK` | Ode to Joy theme (Beethoven, Symphony No. 9, 1824) with bass and chords |
| `JINGLE.WRK` | Jingle Bells, chorus (James Lord Pierpont, 1857) with bass |
| `SILENT.WRK` | Silent Night (Franz Xaver Gruber, 1818) in 6/8 with bass and SMPTE-locked markers |
| `GREENS.WRK` | Greensleeves (English traditional, 16th c.) in A minor, 6/8, with a ritardando |
| `AMAZING.WRK` | Amazing Grace (tune New Britain, 1829) in F major, 3/4, with a pickup |
| `TWINKLE.WRK` | Twinkle, Twinkle, Little Star (French traditional, 18th c.) with block chords and varied velocities |
| `FURELISE.WRK` | Fuer Elise, opening (Beethoven, WoO 59, 1810) in 3/8 for two hands |
| `QUANT.WRK` | Edit/Quantize test bed: every start offset in a half note, durations 1..75 and around multiples of a sixteenth, one event of every MIDI kind off the grid |

## Edit oracles

Each file in `edit/` is `src/QUANT.WRK` after one Edit > Quantize with all four tracks selected
(`tests/edit_tests.cpp` lists the settings): `QUANT_16` sixteenth, `QUANT_32` thirty-second,
`QUANT_8T` eighth triplet, `QUANT_ST` starting times only, `QUANT_DU` durations only, `QUANT_50`
strength 50 %, `QUANT_RG` From 2:01:000 Thru 3:01:000 only.

## Limits of Cakewalk Apprentice 1.04 found while building the corpus

Cakewalk Apprentice 1.04 refuses to load a file (message "Out of memory") when a stream name is empty or when a
text event is longer than 127 bytes. String events (Text, MCI command) are kept in a Windows atom
table, which is case-insensitive: two texts that differ only in case collapse into the spelling
seen first, and an empty text event is dropped. A Wave payload is saved with its length rounded
up to a multiple of 32 bytes and zero-padded (the allocated block, not the data). A SMPTE-locked
marker is converted to whole frames and back, so its tick can move by a frame's worth of ticks.
The MIDI thru port is reset to a port that exists on the machine. After a MIDI import, event
records carry stale bytes in the fields their kind does not use: a Patch event that survives the
import (not at tick 0, so not absorbed into the track's patch) has 1 in the 16-bit duration field,
and a Channel aftertouch event has the previous key aftertouch's pressure in data2. libsseq keeps
these fields as read so that the files round-trip, and the corpus test compares only the fields a
kind uses. The TRACK chunk holds the name
split into two 15-character fields; names longer than 30 characters survive only through the
TRKNAME chunk. libsseq itself has none of these limits; the corpus stays within them so that
every sequence has an oracle, and the corpus test encodes the padding and the frame rounding as
expected behaviour.

## Copyright

The realistic sequences use works whose authors died more than 70 years ago or that are traditional:
Beethoven (d. 1827), Gruber (d. 1863), Pierpont (d. 1893), and traditional tunes of the 16th to
19th century. The arrangements themselves are part of libsseq and are released under its MIT
license.
