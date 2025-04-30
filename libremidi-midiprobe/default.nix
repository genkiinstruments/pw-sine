{
  pkgs,
  stdenv,
  lib,
  libremidi,
}:
stdenv.mkDerivation {
  pname = "libremidi-midiprobe";
  version = "0.1";

  # Use the current directory containing libremidi-libremidi-midiprobe.cpp as the source
  src = ./.;

  # Dependencies needed ONLY during the build process itself
  nativeBuildInputs = [
    pkgs.pkg-config # Needed to find library flags
  ];

  # Dependencies needed by the program at runtime AND for building (headers, .so files)
  buildInputs = [
    pkgs.alsa-lib
  ];

  # No configure step needed for this simple C++ file
  dontConfigure = true;

  # Custom build command (no Makefile)
  buildPhase = ''
    runHook preBuild

    $CXX libremidi-libremidi-midiprobe.cpp -o libremidi-libremidi-midiprobe \
      -std=c++20 \
      -DLIBREMIDI_ALSA=1 \
      -DLIBREMIDI_HEADER_ONLY=1 \
      -I ${libremidi}/include \
      -lasound -pthread

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 libremidi-libremidi-midiprobe $out/bin/libremidi-libremidi-midiprobe

    runHook postInstall
  '';

  meta = with lib; {
    description = "MIDI probe utility for checking available MIDI inputs and outputs";
    platforms = platforms.linux;
  };
}
