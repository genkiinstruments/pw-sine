#define _POSIX_C_SOURCE 200809L  // For signal handling
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <errno.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/debug/pod.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// For MIDI support
#define SPA_MEDIA_TYPE_application 2
#define SPA_MEDIA_SUBTYPE_midi 1
#define SPA_CONTROL_Midi 0
#define SPA_DATA_ControlMidi 2

struct data {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;

    double phase;        // Current phase of the sine wave
    float frequency;     // Current frequency of the sine wave
    float amplitude;     // Amplitude of the sine wave
    uint32_t sample_rate;  // Sample rate (will be filled by param_changed)
    
    // MIDI state
    int last_note;       // Last MIDI note received
    int note_active;     // Whether a note is currently active
    
    int shutting_down;   // Flag for graceful shutdown
};

// --- Process callback for generating sine wave ---
static void on_process(void *userdata)
{
    struct data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *samples;
    uint32_t n_samples;

    // Get a buffer for output
    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    samples = buf->datas[0].data;
    if (samples == NULL)
        return;

    n_samples = buf->datas[0].maxsize / sizeof(float);

    // Generate sine wave
    for (uint32_t i = 0; i < n_samples; i++) {
        samples[i] = data->amplitude * sinf(data->phase);
        data->phase += 2.0 * M_PI * data->frequency / data->sample_rate;
        if (data->phase >= 2.0 * M_PI)
            data->phase -= 2.0 * M_PI;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float);
    buf->datas[0].chunk->size = n_samples * sizeof(float);

    pw_stream_queue_buffer(data->stream, b);
}

// --- Handle stream state changes ---
static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                   enum pw_stream_state state, const char *error)
{
    struct data *data = userdata;

    printf("Stream state changed: %s -> %s\n",
           pw_stream_state_as_string(old),
           pw_stream_state_as_string(state));

    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "Stream error: %s\n", error);
        pw_main_loop_quit(data->loop);
    }
}

// --- Handle format changes ---
static void on_stream_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    struct data *data = userdata;

    // Respond only to the Format parameter
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    // Parse the format information we need
    struct spa_audio_info_raw info = { 0 };
    if (spa_format_audio_raw_parse(param, &info) < 0)
        return;

    // Store parameters for audio generation
    data->sample_rate = info.rate;
    printf("Got sample rate: %d\n", data->sample_rate);
}

// --- Stream events ---
static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
};

// --- Signal handler for graceful exit ---
static struct data *g_data = NULL;
static void signal_handler(int sig)
{
    fprintf(stderr, "\nCaught signal %d, exiting...\n", sig);
    if (g_data)
        pw_main_loop_quit(g_data->loop);
}

int main(int argc, char *argv[])
{
    struct data data = { 0 };
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    // For signal handling
    g_data = &data;

    // Initialize library
    pw_init(&argc, &argv);

    // Create a main loop
    data.loop = pw_main_loop_new(NULL);
    if (data.loop == NULL) {
        fprintf(stderr, "Failed to create main loop\n");
        return 1;
    }

    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create a context
    data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    if (data.context == NULL) {
        fprintf(stderr, "Failed to create context\n");
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Connect to PipeWire
    data.core = pw_context_connect(data.context, NULL, 0);
    if (data.core == NULL) {
        fprintf(stderr, "Failed to connect to PipeWire\n");
        pw_context_destroy(data.context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Setup audio parameters
    data.frequency = 440.0f;  // A4 note
    data.amplitude = 0.5f;    // 50% volume
    data.phase = 0.0f;
    data.sample_rate = 44100;  // Will be updated in param_changed

    // Create a simple stream for playback
    data.stream = pw_stream_new_simple(
        pw_main_loop_get_loop(data.loop),
        "sine-generator",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            NULL),
        &stream_events,
        &data);

    if (data.stream == NULL) {
        fprintf(stderr, "Failed to create stream\n");
        pw_core_disconnect(data.core);
        pw_context_destroy(data.context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Define audio format (raw audio, stereo, float)
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = 1,
            .rate = 44100
        ));

    // Connect the stream, asking for the specified format
    if (pw_stream_connect(data.stream,
                         PW_DIRECTION_OUTPUT,
                         PW_ID_ANY,
                         PW_STREAM_FLAG_AUTOCONNECT |
                         PW_STREAM_FLAG_MAP_BUFFERS |
                         PW_STREAM_FLAG_RT_PROCESS,
                         params, 1) < 0) {
        fprintf(stderr, "Failed to connect stream\n");
        pw_stream_destroy(data.stream);
        pw_core_disconnect(data.core);
        pw_context_destroy(data.context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Run the main loop until we quit
    printf("Running PipeWire main loop (press Ctrl+C to quit)...\n");
    pw_main_loop_run(data.loop);

    // Clean up
    pw_stream_destroy(data.stream);
    pw_core_disconnect(data.core);
    pw_context_destroy(data.context);
    pw_main_loop_destroy(data.loop);
    pw_deinit();

    return 0;
}