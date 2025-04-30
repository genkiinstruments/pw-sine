{
  stdenv,
  pipewire,
  pkg-config,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "pipewire-midi-logger";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = [
    pkg-config # Needed to find library flags (for pipewire)
  ];

  buildInputs = [
    pipewire # Provides libpipewire headers and shared library
    # pkgs.glibc # Provided by stdenv
  ];

  buildPhase = ''
    runHook preBuild

    $CXX main.cpp -o ${finalAttrs.pname} \
      $(pkg-config --cflags --libs libpipewire-0.3) \
      -lstdc++ -lm

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 ${finalAttrs.pname} $out/bin/${finalAttrs.pname}

    runHook postInstall
  '';
})
