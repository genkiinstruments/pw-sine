{
  stdenv,
  lib,
  libremidi,
  pkg-config,
  pipewire,
  alsa-lib, # Still needed if libremidi itself links it, or for -lasound if kept
  fetchFromGitHub,
  autoPatchelfHook, # Add the hook
  udev, # Only add if libremidi truly needs it for pipewire backend
}:
let
  readerwriterqueue = fetchFromGitHub {
    owner = "cameron314";
    repo = "readerwriterqueue";
    rev = "v1.0.7";
    hash = "sha256-FUCgW22g7tuaMPERYf53BlXwHA4rESE/7C0yx7c6xzc=";
  };
  args = fetchFromGitHub {
    owner = "Taywee";
    repo = "args";
    rev = "6.4.7";
    hash = "sha256-IQzhbXl1CfEV164EjulKrOUdCTZNZAFgVyzxk4rTNlU=";
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "libremidi-pipewire-midi1-in";
  version = "0.1";

  # Assume your source file is correctly named main.cpp in this directory
  src = ./.;

  nativeBuildInputs = [
    pkg-config
    # autoPatchelfHook # Use the hook for automatic RPATH
  ];

  buildInputs = [
    libremidi
    pipewire # Provides runtime libs and .dev includes headers/pkgconfig now
    alsa-lib # For runtime libasound.so
    udev # Only if needed
  ];

  # Ensure C++ standard library is properly linked if needed (often automatic)
  NIX_CFLAGS_COMPILE = toString [ "-std=c++20" ]; # Set standard via env var is often cleaner

  # Remove dontConfigure if you add a build system later
  dontConfigure = true;

  # Remove preBuildPhase assuming src file is main.cpp
  # preBuildPhase = ''
  #   cp main.c main.cpp || true
  # '';

  buildPhase = ''
    runHook preBuild

    # Query pkg-config for PipeWire flags - libremidi might do this internally
    # via CMake, but for manual build we need it.
    # Note: pipewire package now often includes .dev, check if pipewire.dev is needed
    PIPEWIRE_CFLAGS=$(pkg-config --cflags libpipewire-0.3)
    PIPEWIRE_LIBS=$(pkg-config --libs libpipewire-0.3)
    ALSA_LIBS=$(pkg-config --libs alsa) # Get alsa libs via pkg-config too

    echo "Compiling with CXX: $CXX"
    echo "PipeWire CFLAGS: $PIPEWIRE_CFLAGS"
    echo "PipeWire LIBS: $PIPEWIRE_LIBS"
    echo "ALSA LIBS: $ALSA_LIBS"

    # Simplified Compile Command
    # - Rely on buildInputs for include paths where possible
    # - Rely on pkg-config for library-specific flags/libs
    # - autoPatchelfHook handles runtime linking
    $CXX main.cpp -o ${finalAttrs.pname} \
      $NIX_CFLAGS_COMPILE \
      -DLIBREMIDI_HEADER_ONLY=1 \
      -DLIBREMIDI_PIPEWIRE=1 \
      -I${libremidi}/include \
      -I${libremidi}/examples \
      -I${readerwriterqueue} \
      -I${args} \
      $PIPEWIRE_CFLAGS \
      $PIPEWIRE_LIBS \
      $ALSA_LIBS \
      -pthread # Keep pthread for safety, -ldl often not needed explicitly

    # Check if the executable links correctly
    echo "Checking linked libraries:"
    ldd ./${finalAttrs.pname} || echo "ldd failed (expected in build sandbox?)"

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 ${finalAttrs.pname} $out/bin/${finalAttrs.pname}

    runHook postInstall
  '';

  # Remove manual fixupPhase, autoPatchelfHook handles it
  # fixupPhase = ''
  #   ...
  # '';

  meta = with lib; {
    description = "MIDI probe utility using Libremidi and PipeWire";
    platforms = platforms.linux;
    license = licenses.mit; # Please specify a license
  };
})
