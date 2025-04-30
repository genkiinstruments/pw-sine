{
  stdenv,
  lib,
  libsoundio,
  pkg-config,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "libsoundio-sine";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    pkg-config
  ];

  buildInputs = [
    libsoundio
  ];

  # No configure step needed for this simple C file
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    $CC main.c -o "${finalAttrs.pname}" -lsoundio -lm -pthread

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 "${finalAttrs.pname}" $out/bin/"${finalAttrs.pname}"

    runHook postInstall
  '';

  meta = with lib; {
    description = "Simple sine wave generator using libsoundio";
    platforms = platforms.all;
  };
})
