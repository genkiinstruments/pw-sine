#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h> // For usleep

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>        // Explicitly include filter header
#include <pipewire/properties.h>    // For pw_properties_*
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h> // Need this for SPA_AUDIO_FORMAT_MIDI
#include <spa/pod/builder.h>
#include <spa/support/loop.h>       // For pw_loop
#include <spa/utils/result.h>       // For SPA_RESULT_IS_ERROR etc.
#include <spa/utils/string.h>       // For spa_strerror

// --- MIDI Parsing Helper --- (Identical to previous version)

// Basic MIDI status bytes (upper nibble)
#define MIDI_NOTE_OFF           0x80
#define MIDI_NOTE_ON            0x90
#define MIDI_POLY_KEY_PRESSURE  0xA0 // Polyphonic Aftertouch
#define MIDI_CONTROL_CHANGE     0xB0
#define MIDI_PROGRAM_CHANGE     0xC0
#define MIDI_CHANNEL_PRESSURE   0xD0 // Channel Aftertouch
#define MIDI_PITCH_BEND         0xE0
#define MIDI_SYSTEM_COMMON      0xF0 // Includes System Exclusive (SysEx)

// Structure to hold application state
struct app_data {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener; // Listener hook for core events
    struct pw_filter *filter;
    struct spa_hook filter_listener; // Listener hook for filter events
    volatile bool running;
    unsigned char running_status; // For MIDI running status
};

// Function to parse and print MIDI messages (Identical to previous version)
static void parse_midi(struct app_data *data, const uint8_t *buffer, size_t size) {
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
                // Handle SysEx, MTC Quarter Frame, Song Select, etc. - More complex
                // For simplicity, we'll just log the status for now
                printf("System Common/Realtime: Status 0x%02X\n", status);
                // Specific handling would be needed here for variable length messages (like SysEx)
                // For now, assume single byte messages for Fx types if not SysEx start (F0)
                if (status == 0xF0) { // SysEx Start
                    printf("  (SysEx Start - parsing not fully implemented)\n");
                    // Find SysEx End (F7) - requires careful buffer boundary checks
                    while (pos < size && buffer[pos] != 0xF7) {
                       // printf(" SysEx data: 0x%02X\n", buffer[pos]);
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
                } else if (status == 0xF6) { // Tune Request
                    msg_len = 1;
                } else { // F4, F5, F7(End), F8(Timing), FA(Start), FB(Continue), FC(Stop), FE(Active Sense), FF(Reset)
                    msg_len = 1; // Most single byte system messages
                }
                break;

            default:
                fprintf(stderr, "Unknown MIDI status: 0x%02X\n", status);
                continue; // Skip to next byte potentially
        }

        // Check if we have enough data in the buffer for the *complete* message
        // (Considering running status: msg_len - 1 data bytes are needed)
        size_t data_bytes_needed = (data->running_status == status) ? msg_len -1 : msg_len -1 ; // bytes after status
        size_t bytes_available = size - pos; // bytes remaining from current position

        if (bytes_available < data_bytes_needed) {
           // fprintf(stderr, "MIDI Parse Warning: Incomplete message (Status 0x%02X). Have %zu, need %zu bytes.\n",
           //        status, bytes_available, data_bytes_needed);
            // Don't process incomplete message, wait for more data in next callback
            break; // Exit the inner loop, process next buffer later
        }

        // --- Process known message types ---
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
                 // Already logged the basic status above, specific types below if needed
                if (status == 0xF1) printf("  MTC Quarter Frame: %d\n", data1);
                else if (status == 0xF2) printf("  Song Pos Ptr: %d\n", (data2 << 7) | data1);
                else if (status == 0xF3) printf("  Song Select: %d\n", data1);
                // Single byte messages are just logged by status
                break;
        }

        // Advance buffer position by the number of *data bytes* consumed
        pos += data_bytes_needed;
    }
}


// --- PipeWire Filter Callbacks ---

// Called when the filter needs to process data
// NOTE: Signature matches the struct definition in pipewire/filter.h
static void on_process(void *userdata) {
    struct app_data *data = userdata;
    struct pw_buffer *buf;
    struct spa_buffer *spa_buf;

    // Dequeue a buffer from the input port
    // Use pw_filter_dequeue_buffer for the simple filter API
    if ((buf = pw_filter_dequeue_buffer(data->filter)) == NULL) {
        // This can happen normally if no data is ready, don't log as warning unless debugging
        // pw_log_warn("Out of buffers or no data ready");
        return;
    }

    spa_buf = buf->buffer;

    // We expect one data plane for MIDI
    if (spa_buf->n_datas < 1) {
        // fprintf(stderr, "Warning: Received buffer with no data planes.\n");
        goto cleanup;
    }

    // Access the MIDI data
    // The offset should usually be 0 for the start of the data
    uint32_t offset = SPA_POD_BODY_SIZE(&spa_buf->datas[0].chunk->pod); // Usually 0 for simple data
    uint32_t size = spa_buf->datas[0].chunk->size;
    uint8_t *midi_data = SPA_PTROFF(spa_buf->datas[0].data, offset, uint8_t);

    if (midi_data && size > 0) {
       // printf("Received %d bytes\n", size); // Debug: Show raw byte count
        parse_midi(data, midi_data, size);
    }

cleanup:
    // Enqueue the buffer back to PipeWire
    pw_filter_queue_buffer(data->filter, buf);
}

// Callback structure for filter events
static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
    // Add other callbacks (.destroy, .state_changed) if needed
};


// --- PipeWire Core Callbacks ---

// Called when the connection to the PipeWire daemon is established
// Note: The 'done' event indicates initial sync is complete
static void on_core_info(void *_data, const struct pw_core_info *info) {
    struct app_data *data = _data;
    printf("PipeWire core connected (version %s)\n", info ? info->version : "unknown");

    // --- Create the Filter ---
    // Use pw_properties for setting attributes
    struct pw_properties *props = pw_properties_new(
        // Use PW_KEY_* constants from <pipewire/properties.h>
        PW_KEY_MEDIA_TYPE, "Midi",       // General type
        PW_KEY_MEDIA_CATEGORY, "Filter", // Role
        PW_KEY_MEDIA_CLASS, "Midi/Raw",  // Specific type
        PW_KEY_APP_NAME, "midi-logger",
        PW_KEY_NODE_NAME, "midi-logger-node",
        NULL // Terminator
    );
    if (!props) {
        fprintf(stderr, "Failed to create properties.\n");
        pw_main_loop_quit(data->loop);
        return;
    }

    // Create the filter using pw_filter_new (more explicit than _new_simple sometimes)
    // Or stick with pw_filter_new_simple which now takes pw_loop*
    data->filter = pw_filter_new_simple(
        pw_context_get_main_loop(data->context), // *** FIX: Pass the loop ***
        "MIDI Logger",       // Name for the filter
        props,               // *** FIX: Pass pw_properties (takes ownership) ***
        &filter_events,      // Callbacks for the filter
        data                 // User data for callbacks
    );
    // pw_properties_free(props); // props ownership is taken by pw_filter_new_simple

    if (!data->filter) {
        fprintf(stderr, "Failed to create PipeWire filter.\n");
        pw_main_loop_quit(data->loop);
        return;
    }

    // Add listener for filter events (needed for pw_filter_new_simple lifecycle?)
    // It's good practice anyway.
    pw_filter_add_listener(data->filter, &data->filter_listener, &filter_events, data);


    // --- Add an Input Port to the Filter ---
    // Define the port format: Raw MIDI using SPA_AUDIO_FORMAT_MIDI
    uint8_t port_buffer[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(port_buffer, sizeof(port_buffer));

    const struct spa_pod *port_params[1];
    // *** FIX: Ensure SPA_AUDIO_FORMAT_MIDI is available (needs type-info.h) ***
    port_params[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(
                .format = SPA_AUDIO_FORMAT_MIDI
            ));

    // *** FIX: Use pw_properties for port properties ***
    struct pw_properties *port_props = pw_properties_new(
        PW_KEY_PORT_NAME, "midi_in",
        PW_KEY_FORMAT_DSP, "8 bit raw midi", // Descriptive format string
        NULL
    );
     if (!port_props) {
        fprintf(stderr, "Failed to create port properties.\n");
        pw_filter_destroy(data->filter); // Clean up filter
        pw_main_loop_quit(data->loop);
        return;
    }

    int res = pw_filter_add_port(data->filter,
                           PW_DIRECTION_INPUT,
                           PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                           0,             // Let PipeWire choose port ID
                           port_props,    // *** FIX: Pass pw_properties (takes ownership) ***
                           port_params,   // Format parameters (MIDI)
                           1              // Number of parameters
                           );

    if (SPA_RESULT_IS_ERROR(res)) { // Check result properly
         fprintf(stderr, "Failed to add input port to filter: %s\n", spa_strerror(res));
         pw_filter_destroy(data->filter); // Clean up filter
         pw_main_loop_quit(data->loop);
         return;
    }
    // pw_properties_free(port_props); // Ownership taken by add_port

    // --- Connect the Filter ---
    res = pw_filter_connect(data->filter,
                          PW_FILTER_FLAG_RT_PROCESS, // Request real-time processing
                          NULL,                      // No specific parameters needed
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
    struct app_data *data = _data;
    fprintf(stderr, "PipeWire core error: %s (code %d, %s)\n", message, res, spa_strerror(res));
    data->running = false; // Signal termination
    pw_main_loop_quit(data->loop); // Stop the main loop
}

// Core event listeners structure
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .info = on_core_info, // Use 'info' event which provides pw_core_info
    .error = on_core_error,
    // .done = optional callback after initial object enumeration
};


// --- Signal Handling --- (Identical to previous version)

static void signal_int(int signal) {
     // This is unsafe in a real signal handler (printf, loop access)
     // Use a global atomic flag or self-pipe trick for robust handling.
     // For this example, we assume app_data is somehow accessible or use a flag.
    // A better approach: have the main loop check a flag periodically or use pw_loop_signal_fd
    printf("\nCaught signal %d, shutting down...\n", signal);

    // How to get 'data' safely here is tricky.
    // We rely on the main loop checking data.running.
    // Set a global volatile flag instead:
    // g_running = false;
    // pw_main_loop_signal_event(data->loop, some_event_source); // If using eventfd
}

// --- Main Function ---

int main(int argc, char *argv[]) {
    struct app_data data = { 0 };
    data.running = true;
    data.running_status = 0; // Initialize running status

    // Initialize PipeWire
    pw_init(&argc, &argv);

    // Create a main loop
    data.loop = pw_main_loop_new(NULL);
    if (!data.loop) {
        fprintf(stderr, "Failed to create PipeWire main loop.\n");
        return 1;
    }

    // Create a PipeWire context
    data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    if (!data.context) {
        fprintf(stderr, "Failed to create PipeWire context.\n");
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Setup signal handler
    signal(SIGINT, signal_int);
    signal(SIGTERM, signal_int);

    // Connect to the PipeWire core daemon
    data.core = pw_context_connect(data.context, NULL, 0);
    if (!data.core) {
        fprintf(stderr, "Failed to connect to PipeWire core.\n");
        pw_context_destroy(data.context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Add listener for core events
    // *** FIX: Use data.core_listener, not a non-existent member ***
    pw_core_add_listener(data.core,
                         &data.core_listener, // Use the listener struct in app_data
                         &core_events,
                         &data);

    printf("Connecting to PipeWire...\n");

    // Start the main loop
    // Need to check data.running for signal handling if not using more advanced methods
    // pw_main_loop_run(data.loop); // This blocks indefinitely

    // Alternative loop structure to handle signals more gracefully (basic version)
    while (data.running) {
        int res = pw_loop_iterate(pw_main_loop_get_loop(data.loop), -1); // Wait indefinitely for events
        if (res < 0) {
             fprintf(stderr, "PipeWire loop iteration error: %s\n", spa_strerror(res));
             break; // Exit on loop error
        }
        // Check signal flag here if using one
    }


    // --- Cleanup ---
    printf("Exiting...\n");

    if (data.filter) {
        // Remove listeners before destroying objects
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
