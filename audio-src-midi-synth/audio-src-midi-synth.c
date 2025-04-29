/* PipeWire MIDI Sine Wave Synthesizer
 * Simple implementation that receives MIDI input and generates sine waves
 */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>

#include <pipewire/pipewire.h>

#define M_PI_M2 (M_PI * 2.0)

#define DEFAULT_RATE     44100
#define DEFAULT_CHANNELS 1
#define DEFAULT_VOLUME   0.7f

#define MIDI_NOTE_ON     0x90
#define MIDI_NOTE_OFF    0x80
#define MIDI_MAX_NOTES   128

// Type definitions for MIDI format
#define SPA_TYPE_OBJECT_Format 1
#define SPA_FORMAT_mediaType 1
#define SPA_FORMAT_mediaSubtype 2
#define SPA_MEDIA_TYPE_application 2
#define SPA_MEDIA_SUBTYPE_midi 1
#define SPA_PARAM_EnumFormat 0
#define SPA_META_Control 0

struct data {
    struct pw_main_loop *loop;
    struct pw_stream *audio_stream;
    struct pw_stream *midi_stream;

    // Audio state
    double phase;
    float frequency;
    float amplitude;
    uint32_t sample_rate;
    
    // MIDI state
    uint8_t velocities[MIDI_MAX_NOTES];
    int active_notes;
};

// Convert MIDI note to frequency (A4 = 69 = 440Hz)
static float midi_note_to_frequency(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

// Process MIDI messages (basic note on/off handling)
static void process_midi_message(struct data *data, const uint8_t *message, size_t size) {
    if (size < 3) return; // Need at least 3 bytes for note on/off
    
    uint8_t status = message[0];
    uint8_t note = message[1] & 0x7F;    // Note number (0-127)
    uint8_t velocity = message[2] & 0x7F; // Velocity (0-127)
    
    uint8_t command = status & 0xF0;     // Command nibble
    
    if (command == MIDI_NOTE_ON && velocity > 0) {
        // Note On
        if (data->velocities[note] == 0) {
            data->active_notes++;
        }
        data->velocities[note] = velocity;
        
        // Set the frequency based on MIDI note
        data->frequency = midi_note_to_frequency(note);
        // Set amplitude based on velocity
        data->amplitude = DEFAULT_VOLUME * ((float)velocity / 127.0f);
        
        fprintf(stderr, "Note On: %d, freq: %.1f Hz, vel: %d\n", 
                note, data->frequency, velocity);
    } 
    else if (command == MIDI_NOTE_OFF || (command == MIDI_NOTE_ON && velocity == 0)) {
        // Note Off
        if (data->velocities[note] > 0) {
            data->velocities[note] = 0;
            data->active_notes--;
            
            if (data->active_notes <= 0) {
                // All notes are off, silence output
                data->amplitude = 0.0f;
                fprintf(stderr, "All notes off\n");
            }
        }
    }
}

// Audio processing callback
static void on_audio_process(void *userdata) {
    struct data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *samples;
    uint32_t n_samples;
    
    if ((b = pw_stream_dequeue_buffer(data->audio_stream)) == NULL) {
        pw_log_warn("out of audio buffers");
        return;
    }
    
    buf = b->buffer;
    samples = buf->datas[0].data;
    if (samples == NULL) {
        pw_stream_queue_buffer(data->audio_stream, b);
        return;
    }
    
    n_samples = buf->datas[0].maxsize / sizeof(float);
    
    // Generate sine wave (or silence if amplitude is 0)
    for (uint32_t i = 0; i < n_samples; i++) {
        samples[i] = data->amplitude * sin(data->phase);
        data->phase += M_PI_M2 * data->frequency / data->sample_rate;
        if (data->phase >= M_PI_M2)
            data->phase -= M_PI_M2;
    }
    
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float);
    buf->datas[0].chunk->size = n_samples * sizeof(float);
    
    pw_stream_queue_buffer(data->audio_stream, b);
}

// MIDI processing callback - simplest approach to raw MIDI bytes
static void on_midi_process(void *userdata) {
    struct data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    
    if ((b = pw_stream_dequeue_buffer(data->midi_stream)) == NULL) {
        pw_log_warn("out of MIDI buffers");
        return;
    }
    
    buf = b->buffer;
    
    // Simple approach: check if there's any MIDI data in the buffer
    if (buf->datas[0].data != NULL && buf->datas[0].chunk->size > 0) {
        uint8_t *midi_data = buf->datas[0].data;
        uint32_t size = buf->datas[0].chunk->size;
        
        // Process each MIDI message (assuming standard 3-byte format)
        for (uint32_t i = 0; i + 2 < size; i += 3) {
            process_midi_message(data, &midi_data[i], 3);
        }
    }
    
    pw_stream_queue_buffer(data->midi_stream, b);
}

// Audio stream state change callback
static void on_audio_state_changed(void *userdata, enum pw_stream_state old,
                              enum pw_stream_state state, const char *error) {
    struct data *data = userdata;
    
    fprintf(stderr, "Audio stream state changed: %s -> %s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state));
            
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "Audio stream error: %s\n", error);
        pw_main_loop_quit(data->loop);
    }
}

// MIDI stream state change callback
static void on_midi_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    struct data *data = userdata;
    
    fprintf(stderr, "MIDI stream state changed: %s -> %s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state));
            
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "MIDI stream error: %s\n", error);
        pw_main_loop_quit(data->loop);
    }
}

// Format change callback for audio
static void on_audio_format_changed(void *userdata, uint32_t id, const struct spa_pod *format) {
    struct data *data = userdata;
    struct spa_audio_info_raw info;
    
    if (id != SPA_PARAM_EnumFormat || format == NULL)
        return;
        
    if (spa_format_audio_raw_parse(format, &info) < 0) {
        fprintf(stderr, "Failed to parse audio format\n");
        return;
    }
    
    fprintf(stderr, "Audio format:\n");
    fprintf(stderr, "  Rate: %d\n", info.rate);
    fprintf(stderr, "  Channels: %d\n", info.channels);
    fprintf(stderr, "  Format: %d\n", info.format);
    
    data->sample_rate = info.rate;
}

static void do_quit(void *userdata, int sig) {
    struct data *data = userdata;
    pw_main_loop_quit(data->loop);
}

// Helper function to build MIDI format
static struct spa_pod *build_midi_format(struct spa_pod_builder *b) {
    struct spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, 
                               SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_application),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_midi),
        0);
    return spa_pod_builder_pop(b, &f);
}

int main(int argc, char *argv[]) {
    struct data data = { 0 };
    const struct spa_pod *audio_params[1];
    const struct spa_pod *midi_params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    
    // Init values
    data.frequency = 440.0f;  // Default to A4
    data.amplitude = 0.0f;    // Start silent
    data.sample_rate = DEFAULT_RATE;
    
    pw_init(&argc, &argv);
    
    // Create the main loop
    data.loop = pw_main_loop_new(NULL);
    if (data.loop == NULL) {
        fprintf(stderr, "Failed to create main loop\n");
        return 1;
    }
    
    // Add signal handling for clean exit
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);
    
    // Audio output stream callbacks
    const struct pw_stream_events audio_events = {
        PW_VERSION_STREAM_EVENTS,
        .process = on_audio_process,
        .state_changed = on_audio_state_changed,
        .param_changed = on_audio_format_changed,
    };
    
    // MIDI input stream callbacks
    const struct pw_stream_events midi_events = {
        PW_VERSION_STREAM_EVENTS,
        .process = on_midi_process,
        .state_changed = on_midi_state_changed,
    };
    
    // Create the audio stream
    data.audio_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop),
        "midi-synth-audio",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            NULL),
        &audio_events,
        &data);
    
    if (data.audio_stream == NULL) {
        fprintf(stderr, "Failed to create audio stream\n");
        return 1;
    }
    
    // Create the MIDI stream
    data.midi_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop),
        "midi-synth-input",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Midi",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Music",
            NULL),
        &midi_events,
        &data);
    
    if (data.midi_stream == NULL) {
        fprintf(stderr, "Failed to create MIDI stream\n");
        return 1;
    }
    
    // Audio format
    audio_params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = DEFAULT_CHANNELS,
            .rate = DEFAULT_RATE));
    
    // Connect audio stream
    if (pw_stream_connect(data.audio_stream,
                         PW_DIRECTION_OUTPUT,
                         PW_ID_ANY,
                         PW_STREAM_FLAG_AUTOCONNECT |
                         PW_STREAM_FLAG_MAP_BUFFERS |
                         PW_STREAM_FLAG_RT_PROCESS,
                         audio_params, 1) < 0) {
        fprintf(stderr, "Failed to connect audio stream\n");
        return 1;
    }
    
    // Reset the builder for MIDI format
    b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    
    // MIDI format
    midi_params[0] = build_midi_format(&b);
    
    // Connect MIDI stream
    if (pw_stream_connect(data.midi_stream,
                         PW_DIRECTION_INPUT,
                         PW_ID_ANY,
                         PW_STREAM_FLAG_AUTOCONNECT |
                         PW_STREAM_FLAG_MAP_BUFFERS |
                         PW_STREAM_FLAG_RT_PROCESS,
                         midi_params, 1) < 0) {
        fprintf(stderr, "Failed to connect MIDI stream\n");
        return 1;
    }
    
    fprintf(stderr, "MIDI synth ready!\n");
    fprintf(stderr, "Connect a MIDI source to 'midi-synth-input'\n");
    fprintf(stderr, "Connect 'midi-synth-audio' to an audio output\n");
    fprintf(stderr, "Press Ctrl+C to exit\n");
    
    // Run the main loop
    pw_main_loop_run(data.loop);
    
    // Cleanup
    pw_stream_destroy(data.audio_stream);
    pw_stream_destroy(data.midi_stream);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    
    return 0;
}