#pragma once

#include <libremidi/libremidi.hpp>
// Credits to https://raw.githubusercontent.com/atsushieno/cmidi2
#include <libremidi/cmidi2.hpp>
#if LIBREMIDI_USE_NI_MIDI2
  #include <midi/universal_packet.h>
#endif

#include "args.hxx"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <variant>
#include <string>

// Simple message printing
inline std::ostream& operator<<(std::ostream& s, const libremidi::message& message)
{
  auto nBytes = message.size();
  s << "[ ";
  for (auto i = 0U; i < nBytes; i++)
    s << std::hex << static_cast<int>(message[i]) << std::dec << " ";
  s << "]";
  if (nBytes > 0)
    s << " ; stamp = " << message.timestamp;
  return s;
}

// Simplified port_information printing
inline std::ostream& operator<<(std::ostream& s, const libremidi::port_information& rhs)
{
  s << "[ ";
  
  if (!rhs.device_name.empty())
    s << "device_name: " << rhs.device_name;
  if (!rhs.port_name.empty())
    s << ", port_name: " << rhs.port_name;
  if (!rhs.display_name.empty())
    s << ", display_name: " << rhs.display_name;
  
  s << " ]";
  return s;
}

namespace libremidi::examples
{
struct arguments
{
  libremidi::API api{libremidi::API::UNSPECIFIED};
  int input_port{0};
  int output_port{0};
  int count{50};
  bool virtual_port{};

  static std::string api_list()
  {
    std::string ret;
    ret.reserve(64);
    auto apis = libremidi::available_apis();
    for (auto api : apis)
    {
      ret += libremidi::get_api_name(api);
      ret += ", ";
    }
    if (apis.size() > 0)
      ret.resize(ret.size() - 2);
    return ret;
  }

  arguments(int argc, const char** argv)
  {
    args::ArgumentParser parser("libremidi example");
    args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});

    args::ValueFlag<std::string> opt_api(parser, "api", "API to use (" + api_list() + ")", {'a'});
    args::ValueFlag<int> opt_port(parser, "input", "Input port to open", {'i'});
    args::ValueFlag<int> opt_out_port(parser, "output", "Output port to open", {'o'});
    args::ValueFlag<int> opt_count(parser, "count", "Number of bytes", {'n'});
    args::Flag opt_virt(
        parser, "virtual", "Open a virtual port instead of an existing one", {'v'});

    args::CompletionFlag completion(parser, {"complete"});

    try
    {
      parser.ParseCLI(argc, argv);
    }
    catch (args::Help)
    {
      std::cout << parser;
      std::exit(1);
    }
    catch (args::ParseError e)
    {
      std::cerr << e.what() << std::endl;
      std::cerr << parser;
      std::exit(1);
    }
    catch (args::ValidationError e)
    {
      std::cerr << e.what() << std::endl;
      std::cerr << parser;
      std::exit(1);
    }

    if (opt_api)
      api = libremidi::get_compiled_api_by_name(opt_api.Get());
    if (opt_port)
      input_port = opt_port.Get();
    if (opt_out_port)
      output_port = opt_out_port.Get();
    if (opt_count)
      count = opt_count.Get();
    if (opt_virt)
      virtual_port = true;
  }

  template <typename T>
  inline bool open_port(T& midi)
  {
    if (this->virtual_port)
    {
      midi.open_virtual_port();
      return true; // Simplified error handling
    }

    // Create an observer with default settings
    libremidi::observer_configuration obs_cfg{};
    obs_cfg.track_any = true;
    
    const auto obs = libremidi::observer{obs_cfg, {}};
    
    // Handle input and output differently
    int index = 0;
    
    if constexpr (std::is_same_v<T, libremidi::midi_out>) {
      auto ports = obs.get_output_ports();
      index = this->output_port;
      
      if (ports.empty()) {
        std::cout << "No output ports available!" << std::endl;
        return false;
      }
      
      if (index >= 0 && index < static_cast<int>(ports.size())) {
        std::cout << "Opening output port: " << ports[index].display_name << std::endl;
        midi.open_port(ports[index]);
        return true;
      } else {
        std::cout << "Cannot open output port " << index << std::endl;
        return false;
      }
    } else {
      auto ports = obs.get_input_ports();
      index = this->input_port;
      
      if (ports.empty()) {
        std::cout << "No input ports available!" << std::endl;
        return false;
      }
      
      if (index >= 0 && index < static_cast<int>(ports.size())) {
        std::cout << "Opening input port: " << ports[index].display_name << std::endl;
        midi.open_port(ports[index]);
        return true;
      } else {
        std::cout << "Cannot open input port " << index << std::endl;
        return false;
      }
    }
    // The input/output port selection and opening is handled above
    return false; // Default case, should not reach here
  }
};
}
