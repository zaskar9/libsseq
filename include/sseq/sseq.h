/* libsseq C API.
 *
 * A flat C interface over the C++ library for use from Swift, Objective-C or C. Objects are
 * opaque handles created with the _new and _read functions and released with the _free functions. Functions that can
 * fail return 0 (or NULL) and leave a message in sseq_last_error(), which is per thread.
 *
 * Threading: a sequence, a transport and a recorder may each be used from one thread at a time. The
 * usual arrangement is a sequence edited on the main thread and a transport pumped from a timer
 * thread while the sequence is not being modified; a transport takes a snapshot of the sequence's
 * events when it is created, so it is recreated after the sequence is edited.
 */
#ifndef SSEQ_H
#define SSEQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This is a C header: typedefs are the C spelling. */
/* NOLINTBEGIN(modernize-use-using) */

/* ---------------------------------------------------------------- basics */

typedef int64_t sseq_tick;

typedef struct sseq_sequence sseq_sequence;
typedef struct sseq_transport sseq_transport;
typedef struct sseq_recorder sseq_recorder;

const char* sseq_version(void); /* "0.8.0" */
/* The message of the last failure on this thread ("" when there was none). */
const char* sseq_last_error(void);

/* A byte buffer returned by the library; release with sseq_data_free. */
typedef struct {
    uint8_t* bytes;
    size_t size;
} sseq_data;
void sseq_data_free(sseq_data* data);

/* ---------------------------------------------------------------- events */

typedef enum {
    SSEQ_EVENT_NOTE = 0,
    SSEQ_EVENT_KEY_AFTERTOUCH,
    SSEQ_EVENT_CONTROLLER,
    SSEQ_EVENT_PATCH,
    SSEQ_EVENT_CHANNEL_AFTERTOUCH,
    SSEQ_EVENT_PITCH_WHEEL,
    SSEQ_EVENT_SYSEX,
    SSEQ_EVENT_MCI_COMMAND,
    SSEQ_EVENT_WAVE,
    SSEQ_EVENT_TEXT
} sseq_event_kind;

/* Note: data1 key, data2 velocity, duration ticks. Key aftertouch: key, pressure. Controller:
 * number, value. Patch: program. Channel aftertouch: pressure. Pitch wheel: data1 -8192..8191.
 * Sysex: data1 bank index. String events carry a payload (see sseq_stream_event_payload). */
typedef struct {
    sseq_tick time;
    sseq_event_kind kind;
    uint8_t channel; /* 0..15 */
    int data1;
    int data2;
    sseq_tick duration;
} sseq_event;

/* ---------------------------------------------------------------- sequence */

sseq_sequence* sseq_sequence_new(void);
sseq_sequence* sseq_sequence_copy(const sseq_sequence* sequence);
void sseq_sequence_swap(sseq_sequence* a,
                        sseq_sequence* b); /* one-level undo: copy before an edit, swap to undo or redo */
void sseq_sequence_free(sseq_sequence* sequence);

/* Files. `normalize` != 0 writes the chunk order Cakewalk uses and drops legacy chunks;
 * 0 reproduces the layout of the file that was read. `port_meta` != 0 writes Cakewalk's "MIDI
 * Port" meta event into .MID files. Readers return NULL on failure, writers 0. */
sseq_sequence* sseq_sequence_read_wrk(const char* path);
sseq_sequence* sseq_sequence_read_wrk_data(const uint8_t* bytes, size_t size);
sseq_sequence* sseq_sequence_read_mid(const char* path);
sseq_sequence* sseq_sequence_read_mid_data(const uint8_t* bytes, size_t size);
int sseq_sequence_write_wrk(const sseq_sequence* sequence, const char* path, int normalize);
int sseq_sequence_write_wrk_data(const sseq_sequence* sequence, int normalize, sseq_data* out);
int sseq_sequence_write_mid(const sseq_sequence* sequence, const char* path, int port_meta);
int sseq_sequence_write_mid_data(const sseq_sequence* sequence, int port_meta, sseq_data* out);

int sseq_sequence_ppq(const sseq_sequence* sequence);
void sseq_sequence_set_ppq(sseq_sequence* sequence, int ppq); /* does not rescale events */
sseq_tick sseq_sequence_end_tick(const sseq_sequence* sequence);

/* Comments (COMMENTS chunk) as text; the returned pointer is valid until the sequence changes. */
const char* sseq_sequence_comments(const sseq_sequence* sequence);
void sseq_sequence_set_comments(sseq_sequence* sequence, const char* text);

/* Global settings (VARS chunk). */
typedef enum {
    SSEQ_VAR_NOW,                 /* ticks */
    SSEQ_VAR_FROM,                /* ticks */
    SSEQ_VAR_THRU,                /* ticks */
    SSEQ_VAR_METRONOME_PLAYBACK,  /* 0/1 */
    SSEQ_VAR_METRONOME_RECORDING, /* 0/1 */
    SSEQ_VAR_METRONOME_ACCENT,    /* 0/1 */
    SSEQ_VAR_COUNT_IN,            /* measures */
    SSEQ_VAR_PATCH_SEARCH_BACK,   /* 0/1 */
    SSEQ_VAR_ZERO_CONTROLLERS,    /* 0/1 */
    SSEQ_VAR_THRU_ON,             /* 0/1, read only */
    SSEQ_VAR_AUTO_RESTART,        /* 0/1, read only */
    SSEQ_VAR_TRANSMIT_MIDI_SYNC   /* 0/1 */
} sseq_var;
int64_t sseq_sequence_var(const sseq_sequence* sequence, sseq_var var);
int sseq_sequence_set_var(sseq_sequence* sequence, sseq_var var, int64_t value); /* 0 for a read-only var */

typedef struct {
    int port;     /* 0-based */
    int channel;  /* 0-based */
    int key;      /* 0..127 */
    int velocity; /* 0 disables the metronome note */
    int duration; /* ticks; 0 disables the metronome note */
} sseq_metronome;
void sseq_sequence_metronome(const sseq_sequence* sequence, sseq_metronome* out);
void sseq_sequence_set_metronome(sseq_sequence* sequence, const sseq_metronome* metronome);

typedef struct {
    int mode;    /* 1 manual, 2 auto */
    int port;    /* 0-based */
    int channel; /* -1 none, 0..15 */
    int key_plus;
    int vel_plus;
} sseq_thru;
void sseq_sequence_thru(const sseq_sequence* sequence, sseq_thru* out);
void sseq_sequence_set_thru(sseq_sequence* sequence, const sseq_thru* thru);

/* ---------------------------------------------------------------- streams */

typedef struct {
    int number;        /* 0-based slot; the UI shows number + 1 */
    int channel;       /* forced output channel 0..15, -1 none */
    int key_plus;      /* -127..127 */
    int vel_plus;      /* -127..127 */
    int port;          /* 0-based */
    int selected;      /* 0/1 */
    int muted;         /* 0/1 */
    int patch;         /* 0..127, -1 none */
    int reps;          /* loop count 1..9999 (9999 = while other streams play), 0 none */
    int has_time_plus; /* 0/1 */
    sseq_tick time_plus;
    int volume; /* 0..127, -1 none */
    int pan;    /* 0..127, -1 none */
} sseq_stream_params;

/* Streams are addressed by index in file order (0..count-1), not by their number. */
int sseq_sequence_stream_count(const sseq_sequence* sequence);
int sseq_sequence_find_stream(const sseq_sequence* sequence, int number); /* index, or -1 */
int sseq_sequence_add_stream(sseq_sequence* sequence, int number);        /* index of the (new or existing) stream */
int sseq_sequence_remove_stream(sseq_sequence* sequence, int index);

int sseq_stream_get_params(const sseq_sequence* sequence, int index, sseq_stream_params* out);
int sseq_stream_set_params(sseq_sequence* sequence, int index,
                           const sseq_stream_params* params);           /* number is not changed */
const char* sseq_stream_name(const sseq_sequence* sequence, int index); /* valid until the sequence changes */
int sseq_stream_set_name(sseq_sequence* sequence, int index, const char* name);
sseq_tick sseq_stream_end_tick(const sseq_sequence* sequence, int index);

size_t sseq_stream_event_count(const sseq_sequence* sequence, int index);
int sseq_stream_event(const sseq_sequence* sequence, int index, size_t event_index, sseq_event* out);
/* Payload of a string event (MCI command, Wave, Text); NULL for other kinds. */
const uint8_t* sseq_stream_event_payload(const sseq_sequence* sequence, int index, size_t event_index, size_t* size);
/* Inserts in time order and returns the new event's index (or SIZE_MAX on failure). */
size_t sseq_stream_insert_event(sseq_sequence* sequence, int index, const sseq_event* event);
size_t sseq_stream_insert_string_event(sseq_sequence* sequence, int index, sseq_tick time, sseq_event_kind kind,
                                       const uint8_t* payload, size_t size);
/* Replaces an event; if its time changed the events are re-sorted and the new index is returned. */
size_t sseq_stream_set_event(sseq_sequence* sequence, int index, size_t event_index, const sseq_event* event);
int sseq_stream_remove_event(sseq_sequence* sequence, int index, size_t event_index);
int sseq_stream_clear_events(sseq_sequence* sequence, int index);

/* ---------------------------------------------------------------- maps and time */

typedef struct {
    sseq_tick time;
    int bpm100; /* beats per minute x 100 */
} sseq_tempo;
size_t sseq_sequence_tempo_count(const sseq_sequence* sequence);
int sseq_sequence_tempo(const sseq_sequence* sequence, size_t index, sseq_tempo* out);
void sseq_sequence_set_tempo(sseq_sequence* sequence, sseq_tick time, int bpm100); /* insert or replace */
void sseq_sequence_remove_tempo(sseq_sequence* sequence, sseq_tick time);          /* not the one at time 0 */
int sseq_sequence_tempo_at(const sseq_sequence* sequence, sseq_tick time, sseq_tempo* out);

typedef struct {
    int measure; /* 1-based */
    int numerator;
    int denominator_log2; /* 2 = quarter note */
} sseq_meter;
size_t sseq_sequence_meter_count(const sseq_sequence* sequence);
int sseq_sequence_meter(const sseq_sequence* sequence, size_t index, sseq_meter* out);
void sseq_sequence_set_meter(sseq_sequence* sequence, const sseq_meter* meter); /* insert or replace by measure */
void sseq_sequence_remove_meter(sseq_sequence* sequence, int measure);          /* not measure 1 */
int sseq_sequence_meter_at(const sseq_sequence* sequence, int measure, sseq_meter* out);

typedef struct {
    int measure;     /* 1-based */
    int alterations; /* -7..7, negative = flats */
} sseq_key;
size_t sseq_sequence_key_count(const sseq_sequence* sequence);
int sseq_sequence_key(const sseq_sequence* sequence, size_t index, sseq_key* out);
void sseq_sequence_set_key(sseq_sequence* sequence, const sseq_key* key);
void sseq_sequence_remove_key(sseq_sequence* sequence, int measure);
int sseq_sequence_key_at(const sseq_sequence* sequence, int measure, sseq_key* out);

typedef struct {
    sseq_tick time;
    int smpte_locked; /* 0/1 */
} sseq_marker;
size_t sseq_sequence_marker_count(const sseq_sequence* sequence);
int sseq_sequence_marker(const sseq_sequence* sequence, size_t index, sseq_marker* out);
const char* sseq_sequence_marker_name(const sseq_sequence* sequence, size_t index);
size_t sseq_sequence_add_marker(sseq_sequence* sequence, const sseq_marker* marker, const char* name); /* index */
int sseq_sequence_set_marker(sseq_sequence* sequence, size_t index, const sseq_marker* marker, const char* name);
int sseq_sequence_remove_marker(sseq_sequence* sequence, size_t index);

typedef enum { SSEQ_FPS_24 = 24, SSEQ_FPS_25 = 25, SSEQ_FPS_30_DROP = 29, SSEQ_FPS_30 = 30 } sseq_frame_rate;
typedef struct {
    sseq_frame_rate rate;
    int64_t offset_frames;
} sseq_timecode;
void sseq_sequence_timecode(const sseq_sequence* sequence, sseq_timecode* out);
void sseq_sequence_set_timecode(sseq_sequence* sequence, const sseq_timecode* timecode);

typedef struct {
    int measure; /* 1-based */
    int beat;    /* 1-based */
    int tick;
} sseq_mbt;
typedef struct {
    int hours, minutes, seconds, frames;
} sseq_hmsf;

double sseq_sequence_tick_to_seconds(const sseq_sequence* sequence, sseq_tick tick);
sseq_tick sseq_sequence_seconds_to_tick(const sseq_sequence* sequence, double seconds);
void sseq_sequence_tick_to_mbt(const sseq_sequence* sequence, sseq_tick tick, sseq_mbt* out);
sseq_tick sseq_sequence_mbt_to_tick(const sseq_sequence* sequence, const sseq_mbt* mbt);
void sseq_sequence_tick_to_hmsf(const sseq_sequence* sequence, sseq_tick tick, sseq_hmsf* out);
sseq_tick sseq_sequence_hmsf_to_tick(const sseq_sequence* sequence, const sseq_hmsf* hmsf);
sseq_tick sseq_sequence_next_measure(const sseq_sequence* sequence, sseq_tick tick);
sseq_tick sseq_sequence_prev_measure(const sseq_sequence* sequence, sseq_tick tick);
sseq_tick sseq_sequence_measure_start(const sseq_sequence* sequence, int measure);
/* Text forms ("12:3:045", "00:01:05:10"); the buffer gets at most `size` bytes including the
 * terminator. Parsers return 0 when the text is not valid. */
void sseq_format_mbt(const sseq_mbt* mbt, char* buffer, size_t size);
int sseq_parse_mbt(const char* text, sseq_mbt* out);
void sseq_format_hmsf(const sseq_hmsf* hmsf, char* buffer, size_t size);
int sseq_parse_hmsf(const char* text, sseq_hmsf* out);

/* ---------------------------------------------------------------- display text */

/* Text forms as Cakewalk's views show them. Buffers get at most `size` bytes with the terminator. */
void sseq_key_name(int key, char* buffer, size_t size);                       /* "C 5" = 60, "G#4" = 56 */
int sseq_parse_key_name(const char* text, int* key);                          /* 0 when the text is not a key name */
void sseq_duration_text(sseq_tick ticks, int ppq, char* buffer, size_t size); /* "50", "1:080" */
int sseq_parse_duration_text(const char* text, int ppq, sseq_tick* ticks);
const char* sseq_event_kind_name(sseq_event_kind kind); /* "Note", "KeyAft", "Contrl", ... */
const char* sseq_gm_program_name(int program);          /* General MIDI level 1 names; "" outside 0..127 */
const char* sseq_gm_percussion_name(int key);           /* channel 10 key names 35..81; "" otherwise */
/* The Event list's Chn and Values columns ("3" or "n/a"; "C 5  100  1:080", "Reed Organ", ...). */
void sseq_event_channel_text(const sseq_event* event, char* buffer, size_t size);
void sseq_event_values_text(const sseq_event* event, const uint8_t* payload, size_t payload_size, int ppq, char* buffer,
                            size_t size);

/* ---------------------------------------------------------------- editing (Edit menu) */

/* The selected region: stream indices (file order, as elsewhere in this API; NULL with count 0
 * means every stream) and the From..Thru time range. An event is in the region when its start
 * time is between from and thru, both inclusive. */
typedef struct {
    const int* streams;
    size_t stream_count;
    sseq_tick from;
    sseq_tick thru;
} sseq_selection;

/* Cakewalk's Event Filter dialog: an inclusive range per field (exclude = the dialog's "Not"),
 * all required; the channel, beat and tick ranges apply to the six MIDI channel kinds. Sysex,
 * Text, MCI and Wave events are not covered: a filter from the dialog always passes them. */
typedef struct {
    int min;
    int max;
    int exclude;
} sseq_value_range;

typedef struct {
    uint32_t kinds; /* "Include" bits (1u << sseq_event_kind) */
    sseq_value_range note_key, note_velocity, note_duration;
    sseq_value_range key_aftertouch_key, key_aftertouch_value;
    sseq_value_range controller_number, controller_value;
    sseq_value_range patch_number;
    sseq_value_range channel_aftertouch_amount;
    sseq_value_range pitch_wheel_value;
    sseq_value_range channel; /* 0-based (the dialog shows 1..16) */
    sseq_value_range beat;    /* 1-based beat within the measure */
    sseq_value_range tick;    /* tick within the beat */
} sseq_event_filter;

void sseq_event_filter_all(sseq_event_filter* out);     /* the dialog's defaults: everything passes */
void sseq_event_filter_nothing(sseq_event_filter* out); /* the dialog's "Nothing": only the uncovered kinds pass */
/* Whether the event at `event_index` of the stream passes the filter (beat and tick use the meter). */
int sseq_event_filter_includes(const sseq_event_filter* filter, const sseq_sequence* sequence, int index,
                               size_t event_index);

/* The Quantize dialog's resolutions; triplets are two thirds of the straight value. */
typedef enum {
    SSEQ_RESOLUTION_WHOLE = 0,
    SSEQ_RESOLUTION_HALF,
    SSEQ_RESOLUTION_QUARTER,
    SSEQ_RESOLUTION_EIGHTH,
    SSEQ_RESOLUTION_EIGHTH_TRIPLET,
    SSEQ_RESOLUTION_SIXTEENTH,
    SSEQ_RESOLUTION_SIXTEENTH_TRIPLET,
    SSEQ_RESOLUTION_THIRTY_SECOND,
    SSEQ_RESOLUTION_THIRTY_SECOND_TRIPLET
} sseq_resolution;
sseq_tick sseq_resolution_ticks(sseq_resolution resolution, int ppq); /* sixteenth at 120 ppq = 30 */

typedef struct {
    sseq_tick resolution;            /* ticks */
    int start_times;                 /* "Starting times" */
    int durations;                   /* "Durations" (notes) */
    int strength;                    /* "Percent strength", start times only */
    const sseq_event_filter* filter; /* NULL: notes only */
} sseq_quantize_options;

/* The arithmetic alone: nearest multiple (exactly half rounds up), moved `strength` percent of the
 * way truncated toward zero; durations to the nearest multiple, at least one, minus one tick. */
sseq_tick sseq_quantize_time(sseq_tick time, sseq_tick resolution, int strength);
sseq_tick sseq_quantize_duration(sseq_tick duration, sseq_tick resolution);
/* Edit > Quantize on the region; returns the number of events changed (-1 on a bad argument). */
int sseq_quantize(sseq_sequence* sequence, const sseq_selection* selection, const sseq_quantize_options* options);

typedef struct {
    int start_times;                 /* "Starting times": distances from the region's From scale */
    int durations;                   /* "Durations" (notes) */
    int percent;                     /* "Percent" */
    const sseq_event_filter* filter; /* NULL: every kind */
} sseq_length_options;

/* ticks * percent / 100, rounded to nearest with half up. */
sseq_tick sseq_scale_ticks(sseq_tick ticks, int percent);
/* Edit > Length on the region; returns the number of events changed (-1 on a bad argument). */
int sseq_length(sseq_sequence* sequence, const sseq_selection* selection, const sseq_length_options* options);

typedef struct {
    int amount;                      /* half-steps, or scale steps when diatonic */
    int diatonic;                    /* "Diatonic math" */
    const sseq_event_filter* filter; /* NULL: notes and key aftertouch */
} sseq_transpose_options;

/* A key moved by half-steps, then by octaves back into 0..127; and by major-scale steps in the
 * key with `alterations` sharps (negative: flats), Cakewalk's way (see edit.hpp). */
int sseq_transpose_key(int key, int half_steps);
int sseq_transpose_key_diatonic(int key, int steps, int alterations);
/* Edit > Transpose on the region; returns the number of events changed (-1 on a bad argument). */
int sseq_transpose(sseq_sequence* sequence, const sseq_selection* selection, const sseq_transpose_options* options);

typedef struct {
    sseq_tick amount;                /* ticks, negative = earlier; clamped so the region cannot start before 0 */
    const sseq_event_filter* filter; /* NULL: every kind */
} sseq_slide_options;

/* The Slide dialog's "Measures" amount in ticks: lengths of the measure at `from`. */
sseq_tick sseq_measures_to_ticks(const sseq_sequence* sequence, sseq_tick from, int measures);
/* Edit > Slide on the region; returns the number of events changed (-1 on a bad argument). */
int sseq_slide(sseq_sequence* sequence, const sseq_selection* selection, const sseq_slide_options* options);

typedef struct {
    int controller;  /* 0..119, or -1 for the pitch wheel ("What") */
    uint8_t channel; /* 0-based */
    int begin;       /* "Value range": 0..127, or -8192..8191 for the wheel */
    int end;
    sseq_tick from; /* "Time range", both inclusive */
    sseq_tick thru;
} sseq_controller_fill_options;

/* Edit > Controller fill into the stream at `index` (Cakewalk fills the current track): same-kind
 * events on the channel in the range are replaced by one event per value, each at the tick the
 * line reaches it (see edit.hpp). Returns the number of events inserted (-1 on a bad argument). */
int sseq_controller_fill(sseq_sequence* sequence, int index, const sseq_controller_fill_options* options);

/* Cut, Copy, Paste. The clipboard holds the region's events per source stream (times relative
 * to From), its tempo entries and, for whole measures, the meter and key per measure. */
typedef struct sseq_clipboard sseq_clipboard;

typedef struct {
    int events;                      /* "Events" from the selected streams */
    const sseq_event_filter* filter; /* NULL: every kind */
    int tempo;                       /* "Tempo changes" */
    int meter_key;                   /* "Meter/key changes": whole measures only */
} sseq_copy_options;

typedef struct {
    int events;
    const sseq_event_filter* filter;
    int delete_hole; /* "Delete hole": what follows moves earlier on the selected streams */
    int tempo;
    int meter_key;
} sseq_cut_options;

typedef enum { SSEQ_PASTE_BLEND = 0, SSEQ_PASTE_REPLACE, SSEQ_PASTE_SLIDE_OVER } sseq_paste_mode;

typedef struct {
    sseq_tick to;    /* "To time" */
    int repetitions; /* "Repetitions" */
    sseq_paste_mode mode;
    int events; /* "Paste what" */
    int tempo;  /* with Replace or Slide over only, as in Cakewalk */
    int meter_key;
    int to_stream; /* "Paste to One track": stream index, or -1 for the source streams */
} sseq_paste_options;

sseq_clipboard* sseq_copy(const sseq_sequence* sequence, const sseq_selection* selection,
                          const sseq_copy_options* options);
sseq_clipboard* sseq_cut(sseq_sequence* sequence, const sseq_selection* selection, const sseq_cut_options* options);
/* The selection names the streams Replace clears and Slide over moves (with to_stream, only that
 * stream is affected). Returns the number of events inserted, -1 on a bad argument. */
int sseq_paste(sseq_sequence* sequence, const sseq_clipboard* clipboard, const sseq_selection* selection,
               const sseq_paste_options* options);
void sseq_clipboard_free(sseq_clipboard* clipboard);
sseq_tick sseq_clipboard_length(const sseq_clipboard* clipboard);
int sseq_clipboard_track_count(const sseq_clipboard* clipboard); /* streams with events */
int sseq_clipboard_tempo_count(const sseq_clipboard* clipboard);
int sseq_clipboard_measures(const sseq_clipboard* clipboard); /* whole measures with meter/key, or 0 */
/* The clipboard as a Standard MIDI File image (Cakewalk's interchange format), valid until the
 * next call on the clipboard; and a clipboard from such an image (NULL on failure). */
const uint8_t* sseq_clipboard_smf(const sseq_clipboard* clipboard, int ppq, size_t* size);
sseq_clipboard* sseq_clipboard_from_smf(const uint8_t* bytes, size_t size);

/* ---------------------------------------------------------------- sysex banks */

size_t sseq_sequence_sysex_bank_count(const sseq_sequence* sequence);
const char* sseq_sequence_sysex_bank_name(const sseq_sequence* sequence, size_t index);
const uint8_t* sseq_sequence_sysex_bank_data(const sseq_sequence* sequence, size_t index, size_t* size);
int sseq_sequence_sysex_bank_auto_send(const sseq_sequence* sequence, size_t index);
size_t sseq_sequence_set_sysex_bank(sseq_sequence* sequence, size_t index, const char* name, int auto_send,
                                    const uint8_t* data, size_t size); /* index == count appends */

/* ---------------------------------------------------------------- playback */

/* A short MIDI message (1 to 3 bytes; 1 for MIDI sync) at an absolute time on the application's clock. */
typedef struct {
    double time;
    int port; /* 0-based output port as in the sequence */
    uint8_t size;
    uint8_t data[3];
} sseq_midi_message;

typedef struct {
    double look_ahead;          /* seconds scheduled ahead of the clock by each pump (0.1) */
    double tempo_ratio;         /* playback speed (1.0) */
    int send_stream_parameters; /* patch, volume, pan of each stream at start (1) */
    int chase;                  /* patch/controller search-back at start (1) */
    int zero_controllers_on_stop;
    int send_sysex_banks;
    int has_loop; /* auto-shuttle between loop_from and loop_thru */
    sseq_tick loop_from;
    sseq_tick loop_thru;
    int metronome;
    int metronome_accent;
    int count_in_measures;
    int stop_at_end;          /* 0 while recording */
    int midi_clock;           /* MIDI sync out: Start/Continue, Clock (24 per quarter note), Stop (0) */
    int start_never_continue; /* always Start, never Continue (0) */
    int clock_while_stopped;  /* with midi_clock: Clock also while stopped, from pump (1) */
} sseq_playback_options;
void sseq_playback_options_default(sseq_playback_options* out);
/* Options from the sequence's own settings; `recording` != 0 picks the recording metronome, count-in
 * and running past the end. */
void sseq_playback_options_from_sequence(const sseq_sequence* sequence, int recording, sseq_playback_options* out);

/* The application's clock and MIDI output. `now` returns monotonic seconds; `send` and `send_sysex`
 * receive messages with timestamps up to look_ahead in the future and must deliver them at those
 * times; `flush` (optional) is called at the end of every pump; `string_event` (optional) receives
 * Text, MCI command and Wave events when their time comes. All run on the thread that pumps. */
typedef struct {
    void* context;
    double (*now)(void* context);
    void (*send)(void* context, const sseq_midi_message* message);
    void (*send_sysex)(void* context, double time, int port, const uint8_t* bytes, size_t size);
    void (*flush)(void* context);
    void (*string_event)(void* context, int stream_number, const sseq_event* event, const uint8_t* payload,
                         size_t payload_size, double time);
    /* Optional: drop messages already handed over whose time is still in the future (called before the
     * transport silences its notes on stop, seek and panic). */
    void (*discard_scheduled)(void* context);
} sseq_transport_callbacks;

/* The sequence must outlive the transport and must not be modified while the transport plays. The
 * transport takes a snapshot of the events when it is created, so after an edit a new transport is
 * needed to hear the change. `options` may be NULL for the defaults. */
sseq_transport* sseq_transport_new(const sseq_sequence* sequence, const sseq_transport_callbacks* callbacks,
                                   const sseq_playback_options* options);
void sseq_transport_free(sseq_transport* transport);

void sseq_transport_options(const sseq_transport* transport, sseq_playback_options* out);
void sseq_transport_set_options(sseq_transport* transport, const sseq_playback_options* options);
void sseq_transport_set_tempo_ratio(sseq_transport* transport, double ratio);

void sseq_transport_play(sseq_transport* transport);
void sseq_transport_stop(sseq_transport* transport);
void sseq_transport_panic(sseq_transport* transport);
void sseq_transport_rewind(sseq_transport* transport);
void sseq_transport_seek(sseq_transport* transport, sseq_tick tick);
int sseq_transport_playing(const sseq_transport* transport);
int sseq_transport_counting_in(const sseq_transport* transport);
sseq_tick sseq_transport_now(const sseq_transport* transport);
sseq_tick sseq_transport_end_tick(const sseq_transport* transport);
/* Sequence position of a clock time while playing (past the count-in); 0 and *valid = 0 otherwise. */
sseq_tick sseq_transport_tick_at(const sseq_transport* transport, double time, int* valid);

void sseq_transport_set_muted(sseq_transport* transport, int stream_number, int muted);
void sseq_transport_set_solo(sseq_transport* transport, int stream_number, int solo);
int sseq_transport_audible(const sseq_transport* transport, int stream_number);

/* Schedules everything up to now + look_ahead. Call every 10-20 ms while playing. Returns 0 once
 * playback has stopped by itself. */
int sseq_transport_pump(sseq_transport* transport);

/* What loading the sequence sends: patch, volume and pan of every stream with events, in stream-number
 * order, muted streams included. Release the result with sseq_render_free. */
int sseq_open_messages(const sseq_sequence* sequence, sseq_midi_message** out, size_t* count);

/* Offline rendering of [from, thru) (thru < 0: the end) with times in seconds from `from`.
 * Release the result with sseq_render_free. `options` may be NULL. */
int sseq_render(const sseq_sequence* sequence, const sseq_playback_options* options, sseq_tick from, sseq_tick thru,
                sseq_midi_message** out, size_t* count);
void sseq_render_free(sseq_midi_message* messages);

/* ---------------------------------------------------------------- recording */

typedef struct {
    int notes;
    int controllers;
    int patches;
    int pitch_wheel;
    int aftertouch;
    int has_punch; /* keep only events in [punch_in, punch_out) */
    sseq_tick punch_in;
    sseq_tick punch_out;
} sseq_record_options;
void sseq_record_options_default(sseq_record_options* out);

/* The transport must outlive the recorder. `options` may be NULL. */
sseq_recorder* sseq_recorder_new(const sseq_transport* transport, const sseq_record_options* options);
void sseq_recorder_free(sseq_recorder* recorder);
void sseq_recorder_options(const sseq_recorder* recorder, sseq_record_options* out);
void sseq_recorder_set_options(sseq_recorder* recorder, const sseq_record_options* options);

/* Input with its arrival time on the transport's clock; only input received while the transport
 * plays (past the count-in) is recorded. */
void sseq_recorder_receive(sseq_recorder* recorder, const sseq_midi_message* message);
void sseq_recorder_finish(sseq_recorder* recorder, double time); /* closes held notes */
size_t sseq_recorder_count(const sseq_recorder* recorder);
int sseq_recorder_event(const sseq_recorder* recorder, size_t index, sseq_event* out);
void sseq_recorder_clear(sseq_recorder* recorder);
/* Merges the take into a stream of the sequence (sound on sound) and clears it; with `replace` != 0 the
 * stream's events starting in [from, thru) are removed first. */
int sseq_recorder_commit(sseq_recorder* recorder, sseq_sequence* sequence, int stream_index, int replace,
                         sseq_tick from, sseq_tick thru);

/* NOLINTEND(modernize-use-using) */

#ifdef __cplusplus
}
#endif

#endif /* SSEQ_H */
