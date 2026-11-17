# The build environment of build_docker.sh (Docker + Nix, as the TotK and Bloodborne ports' are): the tools that run in
# the container. What runs on the handheld is compiled against the target sysroot instead (scripts/nix/default.nix),
# never against Nix's glibc.
#
#   nix-shell scripts/nix/shell.nix --argstr arch aarch64 --run 'python3.12 --version'
#
# Python 3.12 for nx2/ and the tests (requirements.txt's pins, through uv into the container's /venv), LLVM 23
# (default.nix toolchain), and what the build and the tests call by name. Programs built for a usual Linux (Python's
# binary wheels) run through nix-ld: build_docker.sh links the container's /lib/ld-linux-*.so.* to LONGSHOT_NIX_LD_STUB.
{ pkgs ? import <nixpkgs> { }, arch ? (if pkgs.stdenv.hostPlatform.isAarch64 then "aarch64" else "x86_64") }:
let
  lib = pkgs.lib;
  llvm = pkgs.llvmPackages_23 or pkgs.llvmPackages_latest;
  target = import ./default.nix { inherit pkgs arch; };
  # What prebuilt Linux programs and Python's binary wheels load from the system besides glibc.
  fhsLibs = with pkgs; [ stdenv.cc.cc zlib zstd ];
in
pkgs.mkShell {
  packages = with pkgs; [
    llvm.lld llvm.llvm ninja gnumake cmake pkg-config git curl xz zstd file which rsync
    python312 uv glslang spirv-tools
  ];
  # clang, clang++, lld and the LLVM tools for the target (build_docker.sh links them as EMSDK/upstream/bin)
  LONGSHOT_LLVM = target.toolchain;
  # nix-ld: the dynamic linker that prebuilt programs name, and the libraries it gives them
  LONGSHOT_NIX_LD_STUB = "${pkgs.nix-ld}/libexec/nix-ld";
  NIX_LD = pkgs.stdenv.cc.bintools.dynamicLinker;
  NIX_LD_LIBRARY_PATH = lib.makeLibraryPath fhsLibs;
  # Python's binary wheels (numpy, unicorn, capstone, ...) under Nix's Python: their libstdc++ and zlib
  LONGSHOT_WHEEL_LIBS = lib.makeLibraryPath fhsLibs;
}
