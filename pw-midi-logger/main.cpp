#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <memory>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/properties.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/format-utils.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/support/loop.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

// MIDI status bytes (upper nibble)
#define MIDI_NOTE_OFF           0x80
#define MIDI_NOTE_ON            0x90
#define MIDI_POLY_KEY_PRESSURE  0xA0 // Polyphonic Aftertouch
#define MIDI_CONTROL_CHANGE     0xB0
#define MIDI_PROGRAM_CHANGE     0xC0
#define MIDI_CHANNEL_PRESSURE   0xD0 // Channel Aftertouch
#define MIDI_PITCH_BEND         0xE0
#define MIDI_SYSTEM_COMMON      0xF0 // Includes System Exclusive (SysEx)

// Structure to hold application state
struct AppData {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_filter *filter;
    struct spa_hook filter_listener;
    volatile bool running;
    unsigned char running_status; // For MIDI running status
};

// Forward declarations
static void parse_midi(AppData *data, const uint8_t *buffer, size_t size);
static void on_process(void *userdata);
static void on_core_info(void *data, const struct pw_core_info *info);
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message);
static void signal_handler(int signal);

// Global pointer for signal handling
static AppData *g_app_data = nullptr;

// Function to parse and print MIDI messages
static void parse_midi(AppData *data, const uint8_t *buffer, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        uint8_t status;
        size_t msg_len = 0; // Expected length including status byte

        // Check if the current byte is a status byte (MSB is 1)
        if (buffer[pos] & 0x80) {
            status = buffer[pos];
            data->running_status = status; // Update running status
            pos++;
        } else {
            // Use running status if the current byte is a data byte (MSB is 0)
            if (data->running_status != 0) {
                status = data->running_status;
                // Don't increment pos here, the current byte is the first data byte
            } else {
                // Invalid state: data byte without prior status
                fprintf(stderr, "MIDI Parse Error: Data byte 0x%02X received without running status.\n", buffer[pos]);
                pos++; // Skip this byte
                continue;
            }
        }

        uint8_t command = status & 0xF0; // Get command type (upper nibble)
        uint8_t channel = status & 0x0F; // Get channel (lower nibble)

        // Determine expected message length based on command
        switch (command) {
            case MIDI_NOTE_OFF:
            case MIDI_NOTE_ON:
            case MIDI_POLY_KEY_PRESSURE:
            case MIDI_CONTROL_CHANGE:
            case MIDI_PITCH_BEND:
                msg_len = 3; // Status + 2 data bytes
                break;
            case MIDI_PROGRAM_CHANGE:
            case MIDI_CHANNEL_PRESSURE:
                msg_len = 2; // Status + 1 data byte
                break;
            case MIDI_SYSTEM_COMMON:
                // Handle SysEx, MTC Quarter Frame, Song Select, etc.
                printf("System Common/Realtime: Status 0x%02X\n", status);
                // Specific handling for variable length messages 
                if (status == 0xF0) { // SysEx Start
                    printf("  (SysEx Start - parsing not fully implemented)\n");
                    // Find SysEx End (F7)
                    while (pos < size && buffer[pos] != 0xF7) {
                       pos++;
                    }
                    if (pos < size && buffer[pos] == 0xF7) {
                        printf("  (SysEx End 0xF7 found)\n");
                        pos++; // Consume the F7
                    } else {
                        printf("  (SysEx End 0xF7 not found in buffer)\n");
                    }
                    continue; // Skip normal length check
                } else if (status == 0xF1 || status == 0xF3) { // MTC Quarter Frame, Song Select
                     msg_len = 2;
                } else if (status == 0xF2) { // Song Position Pointer
                    msg_len = 3;
                } else { // Other system messages (F6, F8-FF)
                    msg_len = 1; // Most single byte system messages
                }
                break;

            default:
                fprintf(stderr, "Unknown MIDI status: 0x%02X\n", status);
                continue; // Skip to next byte potentially
        }

        // Check if we have enough data in the buffer for the complete message
        size_t data_bytes_needed = (data->running_status == status) ? msg_len - 1 : msg_len - 1;
        size_t bytes_available = size - pos;

        if (bytes_available < data_bytes_needed) {
            // Don't process incomplete message, wait for more data
            break;
        }

        // Process known message types
        uint8_t data1 = (data_bytes_needed > 0) ? buffer[pos] : 0;
        uint8_t data2 = (data_bytes_needed > 1) ? buffer[pos + 1] : 0;

        switch (command) {
            case MIDI_NOTE_OFF:
                printf("Note Off   Ch: %2d Note: %3d Vel: %3d\n", channel + 1, data1, data2);
                break;
            case MIDI_NOTE_ON:
                // Note On with velocity 0 is often treated as Note Off
                if (data2 == 0) {
                   printf("Note Off*  Ch: %2d Note: %3d Vel: %3d (NoteOn w/ Vel 0)\n", channel + 1, data1, data2);
                } else {
                   printf("Note On    Ch: %2d Note: %3d Vel: %3d\n", channel + 1, data1, data2);
                }
                break;
            case MIDI_POLY_KEY_PRESSURE:
                printf("Poly Press Ch: %2d Note: %3d Pressure: %3d\n", channel + 1, data1, data2);
                break;
            case MIDI_CONTROL_CHANGE:
                printf("Control Chg Ch: %2d Ctrl: %3d Val: %3d\n", channel + 1, data1, data2);
                break;
            case MIDI_PROGRAM_CHANGE:
                printf("Program Chg Ch: %2d Prog: %3d\n", channel + 1, data1);
                break;
            case MIDI_CHANNEL_PRESSURE:
                printf("Chan Press Ch: %2d Pressure: %3d\n", channel + 1, data1);
                break;
            case MIDI_PITCH_BEND: {
                int bend = ((data2 << 7) | data1) - 8192; // Combine LSB/MSB, center is 8192
                printf("Pitch Bend Ch: %2d Val: %5d\n", channel + 1, bend);
                break;
            }
            case MIDI_SYSTEM_COMMON:
                // Already logged the basic status above, specific types below
                if (status == 0xF1) printf("  MTC Quarter Frame: %d\n", data1);
                else if (status == 0xF2) printf("  Song Pos Ptr: %d\n", (data2 << 7) | data1);
                else if (status == 0xF3) printf("  Song Select: %d\n", data1);
                break;
        }

        // Advance buffer position by the data bytes consumed
        pos += data_bytes_needed;
    }
}

// PipeWire Filter Callbacks
static void on_process(void *userdata, struct spa_io_position *position) {
    auto *data = static_cast<AppData*>(userdata);
    struct pw_buffer *buf;
    struct spa_buffer *spa_buf;
    struct spa_data *spa_data = nullptr;
    uint32_t size = 0;
    uint8_t *midi_data = nullptr;

    // Dequeue a buffer from the input port
    if ((buf = pw_filter_dequeue_buffer(data->filter)) == nullptr) {
        return;
    }

    spa_buf = buf->buffer;

    // We expect one data plane for MIDI
    if (spa_buf->n_datas >= 1) {
        // Access the MIDI data
        spa_data = &spa_buf->datas[0];
        size = spa_data->chunk->size;
        midi_data = static_cast<uint8_t*>(spa_data->data);

        if (midi_data && size > 0) {
            parse_midi(data, midi_data, size);
        }
    }

    // Enqueue the buffer back to PipeWire
    pw_filter_queue_buffer(data->filter, buf);
}

// Callback structure for filter events
static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

// PipeWire Core Callbacks
static void on_core_info(void *_data, const struct pw_core_info *info) {
    auto *data = static_cast<AppData*>(_data);
    printf("PipeWire core connected (version %s)\n", info ? info->version : "unknown");

    // Create the Filter
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Midi",       
        PW_KEY_MEDIA_CATEGORY, "Filter", 
        PW_KEY_MEDIA_CLASS, "Midi/Raw",  
        PW_KEY_APP_NAME, "midi-logger",
        PW_KEY_NODE_NAME, "midi-logger-node",
        nullptr
    );
    
    if (!props) {
        fprintf(stderr, "Failed to create properties.\n");
        pw_main_loop_quit(data->loop);
        return;
    }

    // Create the filter
    data->filter = pw_filter_new_simple(
        pw_context_get_main_loop(data->context),
        "MIDI Logger",       // Name for the filter
        props,               // Properties (takes ownership)
        &filter_events,      // Callbacks for the filter
        data                 // User data for callbacks
    );

    if (!data->filter) {
        fprintf(stderr, "Failed to create PipeWire filter.\n");
        pw_main_loop_quit(data->loop);
        return;
    }

    // Add listener for filter events
    pw_filter_add_listener(data->filter, &data->filter_listener, &filter_events, data);

    // Add an Input Port to the Filter
    uint8_t port_buffer[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(port_buffer, sizeof(port_buffer));

    // Create a simple MIDI format pod
    const struct spa_pod *pod;
    pod = reinterpret_cast<const struct spa_pod *>(
        spa_pod_builder_add_object(&pb,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control)
        )
    );
    
    const struct spa_pod *port_params[1];
    port_params[0] = pod;

    // Port properties
    struct pw_properties *port_props = pw_properties_new(
        PW_KEY_PORT_NAME, "midi_in",
        PW_KEY_FORMAT_DSP, "8 bit raw midi",
        nullptr
    );
    
    if (!port_props) {
        fprintf(stderr, "Failed to create port properties.\n");
        pw_filter_destroy(data->filter);
        pw_main_loop_quit(data->loop);
        return;
    }

    // Using void* return value from pw_filter_add_port
    void *port = pw_filter_add_port(data->filter,
                          PW_DIRECTION_INPUT,
                          PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                          0,             // Let PipeWire choose port ID
                          port_props,    // Port properties (takes ownership)
                          port_params,   // Format parameters
                          1              // Number of parameters
                          );
                           
    int res = port ? 0 : -1;  // Convert to result code

    if (SPA_RESULT_IS_ERROR(res)) {
         fprintf(stderr, "Failed to add input port to filter: %s\n", spa_strerror(res));
         pw_filter_destroy(data->filter);
         pw_main_loop_quit(data->loop);
         return;
    }

    // Connect the Filter
    res = pw_filter_connect(data->filter,
                          PW_FILTER_FLAG_RT_PROCESS,
                          nullptr,
                          0);

    if (SPA_RESULT_IS_ERROR(res)) {
        fprintf(stderr, "Failed to connect PipeWire filter: %s\n", spa_strerror(res));
        pw_filter_destroy(data->filter);
        pw_main_loop_quit(data->loop);
        return;
    }

    printf("MIDI Logger filter created and connected.\n");
    printf("Connect a MIDI source to the port (e.g., 'midi-logger-node:midi_in') using Helvum, QjackCtl, pw-link, etc.\n");
    printf("Waiting for MIDI events...\n");
}

// Called if there's an error with the PipeWire core connection
static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message) {
    auto *data = static_cast<AppData*>(_data);
    fprintf(stderr, "PipeWire core error: %s (code %d, %s)\n", message, res, spa_strerror(res));
    data->running = false;
    pw_main_loop_quit(data->loop);
}

// Core event listeners structure
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .info = on_core_info,
    .error = on_core_error,
};

// Signal handling
static void signal_handler(int signal) {
    printf("\nCaught signal %d, shutting down...\n", signal);
    if (g_app_data) {
        g_app_data->running = false;
    }
}

int main(int argc, char *argv[]) {
    AppData data = {};
    data.running = true;
    data.running_status = 0;
    
    // Store global pointer for signal handling
    g_app_data = &data;

    // Initialize PipeWire
    pw_init(&argc, &argv);

    // Create a main loop
    data.loop = pw_main_loop_new(nullptr);
    if (!data.loop) {
        fprintf(stderr, "Failed to create PipeWire main loop.\n");
        return 1;
    }

    // Create a PipeWire context
    data.context = pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
    if (!data.context) {
        fprintf(stderr, "Failed to create PipeWire context.\n");
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Connect to the PipeWire core daemon
    data.core = pw_context_connect(data.context, nullptr, 0);
    if (!data.core) {
        fprintf(stderr, "Failed to connect to PipeWire core.\n");
        pw_context_destroy(data.context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Add listener for core events
    pw_core_add_listener(data.core,
                         &data.core_listener,
                         &core_events,
                         &data);

    printf("Connecting to PipeWire...\n");

    // Alternative loop structure for graceful signal handling
    while (data.running) {
        int res = pw_loop_iterate(pw_main_loop_get_loop(data.loop), -1);
        if (res < 0) {
             fprintf(stderr, "PipeWire loop iteration error: %s\n", spa_strerror(res));
             break;
        }
    }

    // Cleanup
    printf("Exiting...\n");

    if (data.filter) {
        spa_hook_remove(&data.filter_listener);
        pw_filter_destroy(data.filter);
    }
    if (data.core) {
        spa_hook_remove(&data.core_listener);
        pw_core_disconnect(data.core);
    }
    if (data.context) {
        pw_context_destroy(data.context);
    }
    if (data.loop) {
        pw_main_loop_destroy(data.loop);
    }

    pw_deinit();
    
    return 0;
}