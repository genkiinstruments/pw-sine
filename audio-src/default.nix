{
  stdenv,
  lib,
  pipewire,
  pkg-config,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "audio-src";
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
    $CC audio-src.c -o ${finalAttrs.pname} -lpipewire -lspa -lm

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 ${finalAttrs.pname} $out/bin/${finalAttrs.pname}

    runHook postInstall
  '';

  meta = with lib; {
    description = "Simple PipeWire sine wave generator";
    platforms = platforms.all;
  };
})
