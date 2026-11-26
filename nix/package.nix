{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  glfw,
  freetype,
  fontconfig,
  libpng,
  librsvg,
  libepoxy,
  libGL,
  lld,
  makeWrapper,
  addDriverRunpath,
}:

let
  version = lib.strings.trim (builtins.readFile ../VERSION);
in
stdenv.mkDerivation {
  pname = "flux";
  inherit version;

  src = lib.cleanSource ../.;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    lld
    makeWrapper
    addDriverRunpath
  ];

  buildInputs = [
    glfw
    freetype
    fontconfig
    libpng
    librsvg
    libepoxy
    libGL
  ];

  cmakeFlags = [
    # -march=native is not reproducible and can produce binaries that do not
    # run on the machine that builds them (for example with remote builders).
    "-DFLUX_NATIVE=OFF"
    # The project enables ThinLTO in Release builds, which needs lld here.
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
  ];

  postFixup = ''
    # libepoxy loads the GL implementation at runtime, so the vendor driver
    # directory has to be reachable through LD_LIBRARY_PATH on NixOS.
    wrapProgram $out/bin/flux \
      --prefix LD_LIBRARY_PATH : /run/opengl-driver/lib
  '';

  meta = {
    description = "Lightweight Linux software center";
    homepage = "https://github.com/dimunyx/Flux";
    license = lib.licenses.gpl3Only;
    mainProgram = "flux";
    platforms = lib.platforms.linux;
    sourceProvenance = [ lib.sourceTypes.fromSource ];
  };
}
