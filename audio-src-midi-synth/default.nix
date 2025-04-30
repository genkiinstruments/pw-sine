{
  stdenv,
  lib,
  pkg-config,
  pipewire,
}:
stdenv.mkDerivation {
  pname = "audio-src-midi-synth";
  version = "0.1";

  # Use the current directory containing main.c as the source
  src = ./.;

  # Dependencies needed ONLY during the build process itself
  nativeBuildInputs = [
    pkg-config # Needed to find library flags
  ];

  # Dependencies needed by the program at runtime AND for building (headers, .so files)
  buildInputs = [
    pipewire # Provides libpipewire, libspa, and headers
  ];

  # No configure step needed for this simple C file
  dontConfigure = true;

  # Custom build command (no Makefile)
  buildPhase = ''
    runHook preBuild

    # Compile the C file using the compiler from stdenv ($CC)
    # We explicitly include both pipewire and spa headers
    $CC audio-src-midi-synth.c -o audio-src-midi-synth $(pkg-config --cflags --libs libpipewire-0.3 libspa-0.2) -lm

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 audio-src-midi-synth $out/bin/audio-src-midi-synth

    runHook postInstall
  '';

  meta = with lib; {
    description = "Simple PipeWire sine wave generator";
    platforms = platforms.all;
  };
}
