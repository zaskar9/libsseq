# Test corpus

Reference files for the corpus test (`tests/corpus_tests.cpp`). Every file here was produced from
the song definitions in `tests/corpus/songs.cpp`; nothing in this directory comes from Cakewalk's
own sample songs or from any other third party.

## Directories

| Directory | Produced by | Role |
|---|---|---|
| `src/` | libsseq (`gencorpus`), normalized layout | the song as written by the library |
| `app/NAME.WRK` | Cakewalk Apprentice 1.04, File > Save As (Cakewalk) after loading `src/NAME.WRK` | oracle for the `.WRK` format |
| `app/NAME.MID` | Cakewalk Apprentice 1.04, File > Save As (MIDI File) of the same song | oracle for the SMF export mapping |
| `mid2wrk/NAME.WRK` | Cakewalk Apprentice 1.04, loading `app/NAME.MID` and saving as Cakewalk | oracle for the SMF import mapping |

The Apprentice files were produced by running the original program under Wine 9.0 (Linux
x86_64, Win16 subsystem) with the UI driven by xdotool; see the Ensemble project's
`analysis/wine-exploration/WINE-EXPLORATION.md`. File names keep the 8.3 form Windows 3.1 needs.

To regenerate `src/` after changing a song definition: `build/gencorpus tests/data/src`. The
Apprentice files then have to be produced again for the changed songs, otherwise the corpus test
reports the difference.

## Songs

The synthetic songs exercise the model and the formats; the realistic ones are simple
arrangements of public-domain music, transcribed for this corpus (melodies by ear and from
memory, so they are not authoritative editions).

| File | Description |
|---|---|
| `SYN_MIN.WRK` | Minimal song: one track, one note, defaults everywhere |
| `SYN_KIND.WRK` | Every event kind: note, key and channel aftertouch, controller, patch, wheel, sysex, MCI, text |
| `SYN_PARM.WRK` | Track parameters: sparse numbers, channel none, Key+/Vel+/Time+, loop, mute, patch, volume, pan |
| `SYN_MAPS.WRK` | Tempo, meter and key changes, markers, SMPTE format and offset, metronome, thru, VARS flags, comments |
| `SYN_OVLP.WRK` | Overlapping same-key notes, simultaneous note-offs, 1-tick durations, chords, large times |
| `SYN_DENS.WRK` | Dense song: 8 tracks with 1500 notes each and one track with 9000 notes (stream over 64 KB) |
| `SYN_STRS.WRK` | String events: lyric-style text on every beat, a long text, MCI commands, empty text |
| `SYN_WAVE.WRK` | A Wave event with a small payload (format unverified; kept separate on purpose) |
| `ROWBOAT.WRK` | Row, Row, Row Your Boat (traditional, 19th c.) as a four-part round in 6/8 |
| `FRERE.WRK` | Frere Jacques (traditional) as a round made with identical tracks and Time+ offsets |
| `ODETOJOY.WRK` | Ode to Joy theme (Beethoven, Symphony No. 9, 1824) with bass and chords |
| `JINGLE.WRK` | Jingle Bells, chorus (James Lord Pierpont, 1857) with bass |
| `SILENT.WRK` | Silent Night (Franz Xaver Gruber, 1818) in 6/8 with bass and SMPTE-locked markers |
| `GREENS.WRK` | Greensleeves (English traditional, 16th c.) in A minor, 6/8, with a ritardando |
| `AMAZING.WRK` | Amazing Grace (tune New Britain, 1829) in F major, 3/4, with a pickup |
| `TWINKLE.WRK` | Twinkle, Twinkle, Little Star (French traditional, 18th c.) with block chords and varied velocities |
| `FURELISE.WRK` | Fuer Elise, opening (Beethoven, WoO 59, 1810) in 3/8 for two hands |

## Limits of Cakewalk Apprentice found while building the corpus

Apprentice refuses to load a file (message "Out of memory") when a track name is empty or when a
text event is longer than 127 bytes. String events (Text, MCI command) are kept in a Windows atom
table, which is case-insensitive: two texts that differ only in case collapse into the spelling
seen first, and an empty text event is dropped. A Wave payload is saved with its length rounded
up to a multiple of 32 bytes and zero-padded (the allocated block, not the data). A SMPTE-locked
marker is converted to whole frames and back, so its tick can move by a frame's worth of ticks.
The MIDI thru port is reset to a port that exists on the machine. The TRACK chunk holds the name
split into two 15-character fields; names longer than 30 characters survive only through the
TRKNAME chunk. libsseq itself has none of these limits; the corpus stays within them so that
every song has an oracle, and the corpus test encodes the padding and the frame rounding as
expected behaviour.

## Copyright

The realistic songs use works whose authors died more than 70 years ago or that are traditional:
Beethoven (d. 1827), Gruber (d. 1863), Pierpont (d. 1893), and traditional tunes of the 16th to
19th century. The arrangements themselves are part of libsseq and are released under its MIT
license.
