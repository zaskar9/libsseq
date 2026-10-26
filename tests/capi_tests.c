/* C API tests, compiled as C11: the header must be usable from plain C (and so from Swift). */
#include "sseq/sseq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
        }                                                                                                              \
    } while (0)

/* ---- a fake clock and a recording sink */

typedef struct {
    double time;
    sseq_midi_message messages[256];
    size_t count;
    int flushes;
    int strings;
    int discards;
} Fake;

static double fake_now(void* ctx) {
    return ((Fake*)ctx)->time;
}

static void fake_send(void* ctx, const sseq_midi_message* m) {
    Fake* f = (Fake*)ctx;
    if (f->count < 256) {
        f->messages[f->count++] = *m;
    }
}

static void fake_sysex(void* ctx, double time, int port, const uint8_t* bytes, size_t size) {
    (void)ctx;
    (void)time;
    (void)port;
    (void)bytes;
    (void)size;
}

static void fake_flush(void* ctx) {
    ((Fake*)ctx)->flushes++;
}

static void fake_discard(void* ctx) {
    ((Fake*)ctx)->discards++;
}

static void fake_string(void* ctx, int stream, const sseq_event* e, const uint8_t* payload, size_t size, double time) {
    (void)stream;
    (void)time;
    Fake* f = (Fake*)ctx;
    if (e->kind == SSEQ_EVENT_TEXT && size == 5 && memcmp(payload, "hello", 5) == 0) {
        f->strings++;
    }
}

static sseq_event note(sseq_tick time, uint8_t channel, int key, int vel, sseq_tick duration) {
    sseq_event e;
    memset(&e, 0, sizeof e);
    e.time = time;
    e.kind = SSEQ_EVENT_NOTE;
    e.channel = channel;
    e.data1 = key;
    e.data2 = vel;
    e.duration = duration;
    return e;
}

static void test_sequence_and_files(const char* dir) {
    sseq_sequence* s = sseq_sequence_new();
    CHECK(s != NULL);
    CHECK(sseq_sequence_ppq(s) == 120);
    CHECK(sseq_sequence_stream_count(s) == 0);

    int t = sseq_sequence_add_stream(s, 3);
    CHECK(t == 0);
    CHECK(sseq_sequence_find_stream(s, 3) == 0);
    CHECK(sseq_stream_set_name(s, t, "Lead"));
    CHECK(strcmp(sseq_stream_name(s, t), "Lead") == 0);
    sseq_stream_params p;
    CHECK(sseq_stream_get_params(s, t, &p));
    CHECK(p.number == 3 && p.channel == -1 && p.patch == -1 && p.reps == 0 && p.volume == -1);
    p.channel = 2;
    p.patch = 40;
    p.volume = 100;
    p.pan = 64;
    p.key_plus = 12;
    CHECK(sseq_stream_set_params(s, t, &p));
    CHECK(sseq_stream_get_params(s, t, &p));
    CHECK(p.channel == 2 && p.patch == 40 && p.volume == 100 && p.key_plus == 12);

    /* Events are kept in time order. */
    sseq_event e = note(240, 2, 64, 80, 120);
    CHECK(sseq_stream_insert_event(s, t, &e) == 0);
    e = note(0, 2, 60, 100, 120);
    CHECK(sseq_stream_insert_event(s, t, &e) == 0);
    e = note(120, 2, 62, 90, 120);
    CHECK(sseq_stream_insert_event(s, t, &e) == 1);
    CHECK(sseq_stream_event_count(s, t) == 3);
    CHECK(sseq_stream_insert_string_event(s, t, 60, SSEQ_EVENT_TEXT, (const uint8_t*)"hello", 5) == 1);
    size_t size = 0;
    const uint8_t* payload = sseq_stream_event_payload(s, t, 1, &size);
    CHECK(payload != NULL && size == 5 && memcmp(payload, "hello", 5) == 0);
    CHECK(sseq_stream_event_payload(s, t, 0, &size) == NULL && size == 0);
    CHECK(sseq_stream_event(s, t, 3, &e));
    CHECK(e.time == 240 && e.data1 == 64);
    e.time = 30; /* moves before the text event */
    CHECK(sseq_stream_set_event(s, t, 3, &e) == 1);
    CHECK(sseq_stream_event(s, t, 1, &e) && e.time == 30 && e.data1 == 64);
    CHECK(sseq_stream_remove_event(s, t, 1));
    CHECK(sseq_stream_event_count(s, t) == 3);
    CHECK(sseq_stream_end_tick(s, t) == 240);
    CHECK(sseq_sequence_end_tick(s) == 240);
    CHECK(!sseq_stream_event(s, t, 99, &e));
    CHECK(strlen(sseq_last_error()) > 0);

    /* Maps and conversions. */
    sseq_sequence_set_tempo(s, 240, 6000);
    CHECK(sseq_sequence_tempo_count(s) == 2);
    sseq_tempo tempo;
    CHECK(sseq_sequence_tempo_at(s, 300, &tempo) && tempo.bpm100 == 6000);
    CHECK(fabs(sseq_sequence_tick_to_seconds(s, 240) - 1.0) < 1e-9);
    CHECK(fabs(sseq_sequence_tick_to_seconds(s, 360) - 2.0) < 1e-9); /* 60 bpm after tick 240 */
    CHECK(sseq_sequence_seconds_to_tick(s, 2.0) == 360);
    sseq_meter meter = {3, 3, 2};
    sseq_sequence_set_meter(s, &meter);
    CHECK(sseq_sequence_meter_count(s) == 2);
    CHECK(sseq_sequence_measure_start(s, 3) == 960);
    CHECK(sseq_sequence_measure_start(s, 4) == 960 + 360);
    sseq_mbt mbt;
    sseq_sequence_tick_to_mbt(s, 960 + 120 + 5, &mbt);
    CHECK(mbt.measure == 3 && mbt.beat == 2 && mbt.tick == 5);
    CHECK(sseq_sequence_mbt_to_tick(s, &mbt) == 960 + 125);
    char buf[32];
    sseq_format_mbt(&mbt, buf, sizeof buf);
    CHECK(strcmp(buf, "3:2:005") == 0);
    CHECK(sseq_parse_mbt("12:3:045", &mbt) && mbt.measure == 12 && mbt.beat == 3 && mbt.tick == 45);
    CHECK(!sseq_parse_mbt("nonsense", &mbt));
    sseq_key key = {2, -3};
    sseq_sequence_set_key(s, &key);
    CHECK(sseq_sequence_key_at(s, 5, &key) && key.alterations == -3);
    CHECK(sseq_sequence_key_at(s, 1, &key) && key.alterations == 0);
    sseq_marker marker = {240, 0};
    CHECK(sseq_sequence_add_marker(s, &marker, "Verse") == 0);
    marker.time = 0;
    CHECK(sseq_sequence_add_marker(s, &marker, "Intro") == 0);
    CHECK(strcmp(sseq_sequence_marker_name(s, 1), "Verse") == 0);
    sseq_hmsf hmsf;
    sseq_sequence_tick_to_hmsf(s, 240, &hmsf);
    CHECK(hmsf.hours == 0 && hmsf.minutes == 0 && hmsf.seconds == 1 && hmsf.frames == 0);
    CHECK(sseq_sequence_hmsf_to_tick(s, &hmsf) == 240);
    sseq_format_hmsf(&hmsf, buf, sizeof buf);
    CHECK(strcmp(buf, "00:00:01:00") == 0);

    /* Settings. */
    CHECK(sseq_sequence_set_var(s, SSEQ_VAR_METRONOME_RECORDING, 1));
    CHECK(sseq_sequence_var(s, SSEQ_VAR_METRONOME_RECORDING) == 1);
    CHECK(!sseq_sequence_set_var(s, SSEQ_VAR_THRU_ON, 1));
    sseq_metronome met;
    sseq_sequence_metronome(s, &met);
    CHECK(met.channel == 9 && met.key == 42);
    met.velocity = 0;
    sseq_sequence_set_metronome(s, &met);
    sseq_sequence_set_comments(s, "A comment");
    CHECK(strcmp(sseq_sequence_comments(s), "A comment") == 0);
    CHECK(sseq_sequence_set_sysex_bank(s, 0, "Reset", 1, (const uint8_t*)"\xf0\x7e\x7f\x09\x01\xf7", 6) == 0);
    CHECK(sseq_sequence_sysex_bank_count(s) == 1 && sseq_sequence_sysex_bank_auto_send(s, 0));

    /* Files: memory round trip through both formats, then disk. */
    sseq_data data;
    CHECK(sseq_sequence_write_wrk_data(s, 1, &data));
    CHECK(data.size > 0 && memcmp(data.bytes, "CAKEWALK", 8) == 0);
    sseq_sequence* back = sseq_sequence_read_wrk_data(data.bytes, data.size);
    CHECK(back != NULL);
    sseq_data_free(&data);
    CHECK(data.bytes == NULL);
    CHECK(sseq_sequence_stream_count(back) == 1);
    CHECK(sseq_stream_event_count(back, 0) == 3);
    CHECK(strcmp(sseq_stream_name(back, 0), "Lead") == 0);
    CHECK(sseq_sequence_tempo_count(back) == 2 && sseq_sequence_meter_count(back) == 2);
    CHECK(strcmp(sseq_sequence_comments(back), "A comment") == 0);
    CHECK(sseq_sequence_var(back, SSEQ_VAR_METRONOME_RECORDING) == 1);
    sseq_sequence_free(back);

    CHECK(sseq_sequence_write_mid_data(s, 0, &data));
    CHECK(memcmp(data.bytes, "MThd", 4) == 0);
    back = sseq_sequence_read_mid_data(data.bytes, data.size);
    CHECK(back != NULL);
    sseq_data_free(&data);
    CHECK(sseq_stream_event_count(back, 0) == 3); /* two notes (Key+ baked in) and the text meta */
    CHECK(sseq_stream_event(back, 0, 0, &e) && e.data1 == 72);
    CHECK(sseq_stream_event(back, 0, 1, &e) && e.kind == SSEQ_EVENT_TEXT);
    sseq_sequence_free(back);

    char path[1024];
    snprintf(path, sizeof path, "%s/capi_test.wrk", dir);
    CHECK(sseq_sequence_write_wrk(s, path, 0));
    back = sseq_sequence_read_wrk(path);
    CHECK(back != NULL && sseq_sequence_stream_count(back) == 1);
    sseq_sequence_free(back);
    remove(path);
    CHECK(sseq_sequence_read_wrk("/nonexistent/file.wrk") == NULL);
    CHECK(strlen(sseq_last_error()) > 0);

    sseq_sequence* copy = sseq_sequence_copy(s);
    CHECK(sseq_sequence_stream_count(copy) == 1);
    CHECK(sseq_sequence_remove_stream(copy, 0));
    CHECK(sseq_sequence_stream_count(copy) == 0 && sseq_sequence_stream_count(s) == 1);
    sseq_sequence_free(copy);
    sseq_sequence_free(s);
}

static void test_transport_and_recorder(void) {
    sseq_sequence* s = sseq_sequence_new();
    int t = sseq_sequence_add_stream(s, 0);
    sseq_stream_params p;
    sseq_stream_get_params(s, t, &p);
    p.channel = 2;
    sseq_stream_set_params(s, t, &p);
    sseq_event e = note(0, 2, 60, 100, 120);
    sseq_stream_insert_event(s, t, &e);
    e = note(120, 2, 62, 90, 120);
    sseq_stream_insert_event(s, t, &e);
    sseq_stream_insert_string_event(s, t, 60, SSEQ_EVENT_TEXT, (const uint8_t*)"hello", 5);

    /* Offline render: 2 notes = 4 messages, at 240 ticks per second. */
    sseq_playback_options o;
    sseq_playback_options_default(&o);
    CHECK(o.look_ahead > 0 && o.tempo_ratio == 1.0 && o.chase && o.stop_at_end);
    o.send_stream_parameters = 0;
    sseq_midi_message* out = NULL;
    size_t count = 0;
    CHECK(sseq_render(s, &o, 0, -1, &out, &count));
    CHECK(count == 4);
    CHECK(out[0].size == 3 && out[0].data[0] == 0x92 && out[0].data[1] == 60 && out[0].data[2] == 100);
    CHECK(fabs(out[3].time - 1.0) < 1e-9);
    sseq_render_free(out);
    sseq_playback_options_from_sequence(s, 1, &o);
    CHECK(!o.stop_at_end);

    /* Transport with callbacks. */
    Fake fake;
    memset(&fake, 0, sizeof fake);
    sseq_transport_callbacks cb = {&fake, fake_now, fake_send, fake_sysex, fake_flush, fake_string, fake_discard};
    sseq_playback_options_default(&o);
    o.send_stream_parameters = 0;
    o.look_ahead = 0.05;
    sseq_transport* tr = sseq_transport_new(s, &cb, &o);
    CHECK(tr != NULL);
    CHECK(sseq_transport_end_tick(tr) == 240);
    CHECK(!sseq_transport_playing(tr));
    fake.time = 1.0;
    sseq_transport_play(tr);
    CHECK(sseq_transport_playing(tr));
    CHECK(fake.count == 1); /* first note-on */
    fake.time = 1.3;
    CHECK(sseq_transport_pump(tr));
    CHECK(fake.count == 1);
    CHECK(fake.strings == 1); /* text event at tick 60 = 0.25 s */
    CHECK(sseq_transport_now(tr) == 72);
    int valid = 0;
    CHECK(sseq_transport_tick_at(tr, 1.5, &valid) == 120 && valid);
    CHECK(sseq_transport_tick_at(tr, 0.5, &valid) == 0 && !valid);
    fake.time = 1.5;
    CHECK(sseq_transport_pump(tr));
    CHECK(fake.count == 3); /* note-off and second note-on at 1.5 */
    CHECK(fake.flushes >= 2);
    sseq_transport_set_muted(tr, 0, 1);
    CHECK(!sseq_transport_audible(tr, 0));
    sseq_transport_set_muted(tr, 0, 0);
    fake.time = 2.5;
    CHECK(!sseq_transport_pump(tr)); /* ended */
    CHECK(!sseq_transport_playing(tr));
    CHECK(fake.count == 4);
    sseq_transport_rewind(tr);
    CHECK(sseq_transport_now(tr) == 0);
    CHECK(fake.discards == 0); /* the transport ended by itself: nothing pending to discard */

    /* Recording into the sequence: play from 10.0 s, record two notes, keep the take. */
    sseq_record_options ro;
    sseq_record_options_default(&ro);
    CHECK(ro.notes && !ro.has_punch);
    sseq_recorder* rec = sseq_recorder_new(tr, &ro);
    CHECK(rec != NULL);
    sseq_transport_options(tr, &o);
    o.stop_at_end = 0; /* recording runs past the end of the existing material */
    sseq_transport_set_options(tr, &o);
    fake.time = 10.0;
    sseq_transport_play(tr);
    sseq_midi_message in = {10.0, 0, 3, {0x90, 67, 100}};
    sseq_recorder_receive(rec, &in);
    in.time = 10.5;
    in.data[2] = 0;
    sseq_recorder_receive(rec, &in);
    in.time = 10.5;
    in.data[1] = 69;
    in.data[2] = 100;
    sseq_recorder_receive(rec, &in);
    fake.time = 11.0;
    sseq_transport_pump(tr);
    sseq_recorder_finish(rec, 11.0);
    sseq_transport_stop(tr);
    CHECK(fake.discards == 1); /* a stop while playing discards what was scheduled ahead */
    CHECK(sseq_recorder_count(rec) == 2);
    CHECK(sseq_recorder_event(rec, 0, &e) && e.time == 0 && e.data1 == 67 && e.duration == 120);
    CHECK(sseq_recorder_event(rec, 1, &e) && e.time == 120 && e.data1 == 69 && e.duration == 120);
    /* Keep take: the transport is stopped, so the sequence may change (a new transport picks it up). */
    CHECK(sseq_recorder_commit(rec, s, t, 0, 0, 0));
    CHECK(sseq_stream_event_count(s, t) == 5);
    CHECK(sseq_recorder_count(rec) == 0);
    sseq_recorder_free(rec);
    sseq_transport_free(tr);
    sseq_sequence_free(s);
}

static void test_quantize(void) {
    CHECK(sseq_resolution_ticks(SSEQ_RESOLUTION_SIXTEENTH, 120) == 30);
    CHECK(sseq_resolution_ticks(SSEQ_RESOLUTION_EIGHTH_TRIPLET, 120) == 40);
    CHECK(sseq_quantize_time(3615, 30, 100) == 3630);
    CHECK(sseq_quantize_time(3615, 30, 50) == 3622);
    CHECK(sseq_quantize_duration(44, 30) == 29);
    CHECK(sseq_quantize_duration(45, 30) == 59);

    sseq_sequence* s = sseq_sequence_new();
    int t0 = sseq_sequence_add_stream(s, 0);
    int t1 = sseq_sequence_add_stream(s, 1);
    sseq_event a = note(29, 0, 60, 100, 10);
    sseq_event b = note(29, 0, 62, 100, 10);
    sseq_event c = note(241, 0, 64, 100, 47);
    sseq_stream_insert_event(s, t0, &a);
    sseq_stream_insert_event(s, t0, &c);
    sseq_stream_insert_event(s, t1, &b);
    sseq_quantize_options o = {30, 1, 1, 100, 0};
    /* Only stream 1, only the first note's time range. */
    int streams[1] = {t0};
    sseq_selection sel = {streams, 1, 0, 100};
    CHECK(sseq_quantize(s, &sel, &o) == 1);
    sseq_event e;
    CHECK(sseq_stream_event(s, t0, 0, &e) && e.time == 30 && e.duration == 29);
    CHECK(sseq_stream_event(s, t0, 1, &e) && e.time == 241 && e.duration == 47);
    CHECK(sseq_stream_event(s, t1, 0, &e) && e.time == 29);
    /* Every stream, the whole range. */
    sseq_selection all = {NULL, 0, 0, 1000};
    CHECK(sseq_quantize(s, &all, &o) == 2);
    CHECK(sseq_stream_event(s, t0, 1, &e) && e.time == 240 && e.duration == 59);
    CHECK(sseq_stream_event(s, t1, 0, &e) && e.time == 30);
    CHECK(sseq_quantize(s, NULL, &o) == -1);
    sseq_sequence_free(s);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    CHECK(strcmp(sseq_version(), "0.5.0") == 0);
    CHECK(strcmp(sseq_last_error(), "") == 0);
    test_sequence_and_files(dir);
    {
        char text[64];
        int key = 0;
        sseq_tick ticks = 0;
        sseq_key_name(56, text, sizeof text);
        CHECK(strcmp(text, "G#4") == 0);
        CHECK(sseq_parse_key_name("C 5", &key) && key == 60);
        CHECK(!sseq_parse_key_name("H", &key));
        sseq_duration_text(200, 120, text, sizeof text);
        CHECK(strcmp(text, "1:080") == 0);
        CHECK(sseq_parse_duration_text("1:080", 120, &ticks) && ticks == 200);
        CHECK(strcmp(sseq_event_kind_name(SSEQ_EVENT_PITCH_WHEEL), "Wheel") == 0);
        CHECK(strcmp(sseq_gm_program_name(20), "Reed Organ") == 0);
        CHECK(strcmp(sseq_gm_program_name(200), "") == 0);
        CHECK(strcmp(sseq_gm_percussion_name(42), "Closed Hi-Hat") == 0);
        sseq_event e = note(0, 2, 60, 100, 240);
        sseq_event_channel_text(&e, text, sizeof text);
        CHECK(strcmp(text, "3") == 0);
        sseq_event_values_text(&e, NULL, 0, 120, text, sizeof text);
        CHECK(strcmp(text, "C 5  100  2:000") == 0);
        e.kind = SSEQ_EVENT_TEXT;
        sseq_event_values_text(&e, (const uint8_t*)"hi", 2, 120, text, sizeof text);
        CHECK(strcmp(text, "hi") == 0);
    }
    test_quantize();
    test_transport_and_recorder();
    printf("C API tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
