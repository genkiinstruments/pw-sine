#include <soundio/soundio.h>
#include <libremidi/libremidi.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <iostream>
#include <vector>
#include <string>
#include <string_view> // For string_view in open_port
#include <cmath> // For powf
#include <chrono> // For sleep
#include <thread> // For sleep
#include <iomanip> // For printing hex bytes

// --- Configuration ---
const int MIDI_PORT_INDEX = 0;

// --- Shared State (Thread-Safe) ---
std::atomic<float> g_current_frequency = {0.0f};
std::atomic<bool> g_is_note_on = {false};
std::atomic<int> g_current_midi_note = {-1};

// --- Audio Generation ---
static const float PI = 3.1415926535f;
static float seconds_offset = 0.0f;

float midi_note_to_frequency(int note_number) {
    if (note_number < 0) return 0.0f;
    return 440.0f * powf(2.0f, (float)(note_number - 69) / 12.0f);
}

static void write_callback(struct SoundIoOutStream *outstream,
                           int frame_count_min, int frame_count_max)
{
    const struct SoundIoChannelLayout *layout = &outstream->layout;
    float float_sample_rate = outstream->sample_rate;
    float seconds_per_frame = 1.0f / float_sample_rate;
    struct SoundIoChannelArea *areas;
    int frames_left = frame_count_max;
    int err;

    float current_freq = g_current_frequency.load(std::memory_order_relaxed);
    bool note_active = g_is_note_on.load(std::memory_order_relaxed);

    while (frames_left > 0) {
        int frame_count = frames_left;

        if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
            fprintf(stderr, "Error beginning write: %s\n", soundio_strerror(err));
            return;
        }

        if (!frame_count)
            break;

        if (note_active && current_freq > 0.0f) {
            float radians_per_second = current_freq * 2.0f * PI;
            for (int frame = 0; frame < frame_count; frame += 1) {
                float sample = sinf((seconds_offset + frame * seconds_per_frame) * radians_per_second);
                for (int channel = 0; channel < layout->channel_count; channel += 1) {
                    float *ptr = (float*)(areas[channel].ptr + areas[channel].step * frame);
                    *ptr = sample;
                }
            }
            seconds_offset += seconds_per_frame * frame_count;
            if (seconds_offset * current_freq > 1.0f) {
               seconds_offset -= floorf(seconds_offset * current_freq) / current_freq;
            }
        } else {
            for (int channel = 0; channel < layout->channel_count; channel += 1) {
                memset(areas[channel].ptr, 0, areas[channel].step * frame_count);
            }
            seconds_offset = 0.0f;
        }

        if ((err = soundio_outstream_end_write(outstream))) {
            fprintf(stderr, "Error ending write: %s\n", soundio_strerror(err));
            return;
        }

        frames_left -= frame_count;
    }
}

// --- MIDI Handling ---
void midi_callback(libremidi::message message) {
    libremidi::message_type type = message.get_message_type();
    uint8_t note = 0;
    uint8_t velocity = 0;

    if (message.size() >= 3) {
       note = message.bytes[1];
       velocity = message.bytes[2];
    } else {
         std::cout << "MIDI Message (short): [ ";
         for(const auto& byte : message.bytes) {
             std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)byte << " ";
         }
         std::cout << std::dec << "] ts=" << message.timestamp << std::endl;
        return;
    }

    if (type == libremidi::message_type::NOTE_ON && velocity > 0) {
        std::cout << "Note On : " << (int)note << " Vel: " << (int)velocity << std::endl;
        float freq = midi_note_to_frequency(note);
        g_current_midi_note.store(note, std::memory_order_relaxed);
        g_current_frequency.store(freq, std::memory_order_relaxed);
        g_is_note_on.store(true, std::memory_order_relaxed);
    } else if (type == libremidi::message_type::NOTE_OFF || (type == libremidi::message_type::NOTE_ON && velocity == 0)) {
        if (note == g_current_midi_note.load(std::memory_order_relaxed)) {
            std::cout << "Note Off: " << (int)note << std::endl;
            g_is_note_on.store(false, std::memory_order_relaxed);
            g_current_midi_note.store(-1, std::memory_order_relaxed);
        } else {
             std::cout << "Note Off ignored (not current): " << (int)note << std::endl;
        }
    } else {
         std::cout << "Other MIDI: type=" << static_cast<int>(type) << " [ ";
         for(const auto& byte : message.bytes) {
             std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)byte << " ";
         }
         std::cout << std::dec << "] ts=" << message.timestamp << std::endl;
    }
}

// --- Main Application ---
int main(int argc, char **argv) {
    int err;

    // == Initialize SoundIO == (Same as before)
    struct SoundIo *soundio = soundio_create();
    if (!soundio) { fprintf(stderr, "out of memory (soundio_create)\n"); return 1; }
    err = soundio_connect(soundio);
    if (err) { fprintf(stderr, "Error connecting SoundIO: %s\n", soundio_strerror(err)); soundio_destroy(soundio); return 1; }
    fprintf(stderr, "SoundIO Connected using backend: %s\n", soundio_backend_name(soundio->current_backend));
    soundio_flush_events(soundio);
    int default_out_device_index = soundio_default_output_device_index(soundio);
    if (default_out_device_index < 0) { fprintf(stderr, "No output device found\n"); soundio_destroy(soundio); return 1; }
    struct SoundIoDevice *device = soundio_get_output_device(soundio, default_out_device_index);
    if (!device) { fprintf(stderr, "out of memory (soundio_get_output_device)\n"); soundio_destroy(soundio); return 1; }
    fprintf(stderr, "Output device: %s\n", device->name);
    struct SoundIoOutStream *outstream = soundio_outstream_create(device);
    if (!outstream) { fprintf(stderr, "out of memory (soundio_outstream_create)\n"); soundio_device_unref(device); soundio_destroy(soundio); return 1; }
    outstream->format = SoundIoFormatFloat32NE;
    outstream->write_callback = write_callback;
    if ((err = soundio_outstream_open(outstream))) { fprintf(stderr, "Unable to open output stream: %s\n", soundio_strerror(err)); soundio_outstream_destroy(outstream); soundio_device_unref(device); soundio_destroy(soundio); return 1; }
    fprintf(stderr, "Output stream opened:\n");
    fprintf(stderr, "  Sample Rate: %d Hz\n", outstream->sample_rate);
    fprintf(stderr, "  Latency: %.4f sec\n", outstream->software_latency);
    fprintf(stderr, "  Channels: %d\n", outstream->layout.channel_count);
    fprintf(stderr, "  Format: %s\n", soundio_format_string(outstream->format));
    if (outstream->layout_error) fprintf(stderr, "Warning: Unable to set channel layout: %s\n", soundio_strerror(outstream->layout_error));

    // == Initialize libremidi ==
    libremidi::observer observer;
    libremidi::input_configuration midi_config { // Define config first
        .on_message = midi_callback,
        .ignore_sysex = true,
        .ignore_timing = true,
        .ignore_sensing = true
    };
    // Construct midi_in WITH the configuration
    libremidi::midi_in midi_in(midi_config);

    // Keep track if MIDI setup succeeds
    bool midi_opened = false;

    try {
        auto ports = observer.get_input_ports();

        std::cout << "\nAvailable MIDI input ports:\n";
        if (ports.empty()) {
            std::cout << "  No MIDI input ports available!\n";
        } else {
            for (size_t i = 0; i < ports.size(); ++i) {
                std::cout << "  " << i << ": " << ports[i].display_name << std::endl;
            }
        }
        std::cout << std::endl;

        libremidi::input_port selected_port;
        bool port_selected = false;
        if (ports.size() > MIDI_PORT_INDEX) {
             selected_port = ports[MIDI_PORT_INDEX];
             std::cout << "Selecting MIDI port: " << MIDI_PORT_INDEX << " (" << selected_port.display_name << ")" << std::endl;
             port_selected = true;
        } else if (!ports.empty()) {
            std::cerr << "Warning: Requested MIDI port index " << MIDI_PORT_INDEX
                      << " is out of range. Selecting port 0 instead." << std::endl;
            selected_port = ports[0];
             std::cout << "Selecting MIDI port: 0 (" << selected_port.display_name << ")" << std::endl;
             port_selected = true;
        } else {
            std::cerr << "Warning: No MIDI input ports found. Running without MIDI input.\n" << std::endl;
        }

        if (port_selected) {
            // Open the port using the selected port object.
            // The configuration (callback etc) is already part of midi_in.
            // The second optional argument is a name for the client connection.
            midi_in.open_port(selected_port, "SineSynthClient");
            midi_opened = true; // Mark MIDI as successfully opened
             std::cout << "MIDI port opened successfully." << std::endl;
        }

    } catch (const libremidi::midi_exception& error) {
        std::cerr << "MIDI Error: " << error.what() << std::endl;
    }


    // == Start Audio ==
    if ((err = soundio_outstream_start(outstream))) {
        fprintf(stderr, "Unable to start output stream: %s\n", soundio_strerror(err));
        if (midi_opened) midi_in.close_port(); // Close MIDI if it was opened
        soundio_outstream_destroy(outstream);
        soundio_device_unref(device);
        soundio_destroy(soundio);
        return 1;
    }

    if (midi_opened) {
        fprintf(stderr, "Synth started. Press MIDI keys on the selected input device...\n");
    } else {
        fprintf(stderr, "Synth started (Audio only, no MIDI device opened).\n");
    }
    fprintf(stderr, "Press Ctrl+C to quit.\n");

    // == Main Loop ==
    for (;;) {
        soundio_wait_events(soundio);
    }

    // == Cleanup (Unreachable) ==
    // if (midi_opened) {
    //     midi_in.close_port();
    // }
    // soundio_outstream_destroy(outstream);
    // soundio_device_unref(device);
    // soundio_destroy(soundio);
    // return 0;
}


