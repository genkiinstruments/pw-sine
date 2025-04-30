{
  stdenv,
  lib,
  libremidi,
  libsoundio,
  pkg-config,
  patchelf,
  alsa-lib,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "libsoundio-libremidi-sine-synth";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    pkg-config
    patchelf
  ];

  buildInputs = [
    libsoundio
    libremidi
    alsa-lib # Dependency for libremidi ALSA backend
  ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    $CXX main.cpp -o "${finalAttrs.pname}" \
      -std=c++20 \
      -lsoundio -llibremidi -lm -lasound -lpthread -latomic

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 "${finalAttrs.pname}" $out/bin/"${finalAttrs.pname}"

    runHook postInstall
  '';

  # Remove custom fixupPhase, let the default nix hook handle rpath
  # It uses the buildInputs and linker flags to determine necessary RPATHs.
  # Ensure patchelf is in nativeBuildInputs for this hook to work.

  meta = with lib; {
    description = "Simple sine wave synth using libsoundio and libremidi for midi";
    license = licenses.mit; # Add a license if applicable
    platforms = platforms.linux; # Be more specific if needed
  };
})
