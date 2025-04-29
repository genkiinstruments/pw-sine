/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Audio and MIDI source using \ref pw_stream "pw_stream".
 [title]
 */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#define M_PI_M2f (float)(M_PI+M_PI)

#define DEFAULT_RATE            44100
#define DEFAULT_CHANNELS        2
#define DEFAULT_VOLUME          0.7f

#define MIDI_NOTE_ON            0x90
#define MIDI_NOTE_OFF           0x80
#define MIDI_CONTROL_CHANGE     0xB0

struct data {
    struct pw_main_loop *loop;
    struct pw_stream *audio_stream;
    struct pw_stream *midi_stream;

    float accumulator;
    
    // MIDI timing
    uint32_t frame_count;
    uint32_t next_note_time;
    uint8_t current_note;
    bool note_is_on;
};

static void fill_f32(struct data *d, void *dest, int n_frames)
{
    float *dst = dest, val;
    int i, c;

    for (i = 0; i < n_frames; i++) {
        d->accumulator += M_PI_M2f * 440 / DEFAULT_RATE;
        if (d->accumulator >= M_PI_M2f)
            d->accumulator -= M_PI_M2f;

        val = sinf(d->accumulator) * DEFAULT_VOLUME;
        for (c = 0; c < DEFAULT_CHANNELS; c++)
            *dst++ = val;
    }
}

static void on_audio_process(void *userdata)
{
    struct data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    int n_frames, stride;
    uint8_t *p;

    if ((b = pw_stream_dequeue_buffer(data->audio_stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((p = buf->datas[0].data) == NULL)
        return;

    stride = sizeof(float) * DEFAULT_CHANNELS;
    n_frames = buf->datas[0].maxsize / stride;
    if (b->requested)
        n_frames = SPA_MIN((int)b->requested, n_frames);

    fill_f32(data, p, n_frames);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = n_frames * stride;

    pw_stream_queue_buffer(data->audio_stream, b);
}

static void on_midi_process(void *userdata)
{
    struct data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    uint32_t frames = 1024; // Typical buffer size
    uint8_t *p;
    int size;

    if ((b = pw_stream_dequeue_buffer(data->midi_stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    if ((p = buf->datas[0].data) == NULL)
        return;

    size = buf->datas[0].maxsize;
    
    // Reset buffer
    memset(p, 0, size);
    
    // Simple MIDI pattern: play a note every second
    if (data->frame_count >= data->next_note_time) {
        if (!data->note_is_on) {
            // Note On event
            p[0] = MIDI_NOTE_ON;
            p[1] = data->current_note;  // Note number (60 = middle C)
            p[2] = 100;                 // Velocity
            
            data->note_is_on = true;
            data->next_note_time = data->frame_count + DEFAULT_RATE / 4;  // Note duration (quarter second)
            
            buf->datas[0].chunk->offset = 0;
            buf->datas[0].chunk->size = 3;  // 3 bytes for a MIDI note event
        } else {
            // Note Off event
            p[0] = MIDI_NOTE_OFF;
            p[1] = data->current_note;  // Note number
            p[2] = 0;                   // Velocity (0 for note off)
            
            data->note_is_on = false;
            data->next_note_time = data->frame_count + DEFAULT_RATE / 4;  // Wait quarter second before next note
            
            // Change to next note (simple scale pattern)
            data->current_note = 60 + ((data->current_note - 60 + 1) % 12);
            
            buf->datas[0].chunk->offset = 0;
            buf->datas[0].chunk->size = 3;  // 3 bytes for a MIDI note event
        }
    } else {
        // No MIDI event this time
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->size = 0;
    }
    
    data->frame_count += frames;
    
    pw_stream_queue_buffer(data->midi_stream, b);
}

static const struct pw_stream_events audio_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_audio_process,
};

static const struct pw_stream_events midi_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_midi_process,
};

static void do_quit(void *userdata, int signal_number)
{
    struct data *data = userdata;
    pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
    struct data data = { 0, };
    const struct spa_pod *audio_params[1];
    const struct spa_pod *midi_params[1];
    uint8_t buffer1[1024];
    uint8_t buffer2[1024];
    struct pw_properties *audio_props;
    struct pw_properties *midi_props;
    struct spa_pod_builder b1 = SPA_POD_BUILDER_INIT(buffer1, sizeof(buffer1));
    struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(buffer2, sizeof(buffer2));
    struct spa_pod_builder_state state;

    // Initialize MIDI-related data
    data.frame_count = 0;
    data.next_note_time = 0;
    data.current_note = 60;  // Middle C
    data.note_is_on = false;

    pw_init(&argc, &argv);

    /* make a main loop. If you already have another main loop, you can add
     * the fd of this pipewire mainloop to it. */
    data.loop = pw_main_loop_new(NULL);

    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    /* Create audio stream */
    audio_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                    PW_KEY_MEDIA_CATEGORY, "Playback",
                    PW_KEY_MEDIA_ROLE, "Music",
                    NULL);
    if (argc > 1)
        /* Set stream target if given on command line */
        pw_properties_set(audio_props, PW_KEY_TARGET_OBJECT, argv[1]);
    
    data.audio_stream = pw_stream_new_simple(
                    pw_main_loop_get_loop(data.loop),
                    "audio-src",
                    audio_props,
                    &audio_stream_events,
                    &data);

    /* Create MIDI stream */
    midi_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Midi",
                    PW_KEY_MEDIA_CATEGORY, "Playback",
                    PW_KEY_MEDIA_ROLE, "Music",
                    NULL);
    
    data.midi_stream = pw_stream_new_simple(
                    pw_main_loop_get_loop(data.loop),
                    "midi-src",
                    midi_props,
                    &midi_stream_events,
                    &data);

    /* Configure audio format */
    audio_params[0] = spa_format_audio_raw_build(&b1, SPA_PARAM_EnumFormat,
                    &SPA_AUDIO_INFO_RAW_INIT(
                            .format = SPA_AUDIO_FORMAT_F32,
                            .channels = DEFAULT_CHANNELS,
                            .rate = DEFAULT_RATE ));

    /* Configure MIDI format - using generic format for MIDI */
    midi_params[0] = spa_pod_builder_add_object(&b2,
                    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                    SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
                    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_midi));

    /* Connect audio stream */
    pw_stream_connect(data.audio_stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      audio_params, 1);

    /* Connect MIDI stream */
    pw_stream_connect(data.midi_stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS |
                      PW_STREAM_FLAG_RT_PROCESS,
                      midi_params, 1);

    /* and wait while we let things run */
    pw_main_loop_run(data.loop);

    pw_stream_destroy(data.audio_stream);
    pw_stream_destroy(data.midi_stream);
    pw_main_loop_destroy(data.loop);
    pw_deinit();

    return 0;
}



