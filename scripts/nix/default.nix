# The target side of build_docker.sh: what the game is compiled and linked against. The tools that run in the container
# (Python, Ninja, ...) are in scripts/nix/shell.nix. After the TotK port's tools/nix/default.nix, with the same nixpkgs.
#
#   nix-build scripts/nix -A toolchain                       clang, clang++, lld and the LLVM tools (EMSDK/upstream/bin)
#   nix-build scripts/nix -A sysroot --argstr arch aarch64   Ubuntu 24.04: glibc 2.39, libstdc++ 13 (LONGSHOT_SYSROOT)
#   nix-build scripts/nix -A ffmpeg --argstr arch aarch64    FFmpeg with dav1d, static, against it (LONGSHOT_FFMPEG)
#   nix-build scripts/nix -A sdl2 --argstr arch aarch64      libSDL2-2.0.so.0 against it (LONGSHOT_SDL2)
#   nix-build scripts/nix -A headers                         Vulkan-Headers, SPIRV-Headers, VMA (LONGSHOT_*_HEADERS)
#
# FFmpeg's, dav1d's, SDL2's and the headers' versions and checksums are scripts/linux/deps.sh's, read from it here, so
# this build and the AppImage's take the same sources: a download by its SHA-256, a tag by what it unpacks to (deps.sh
# tree_is). What runs on the handheld (longshot, longshot_vk.so, libSDL2-2.0.so.0) is linked against the sysroot, not
# against Nix's glibc, so it runs on any distribution with glibc 2.39 or newer, outside Nix. The sysroot's packages are
# pinned by sysroot-<deb arch>.lock.json (scripts/nix/sysroot_lock.py writes it; ./build_docker.sh lock runs that).
{ pkgs ? import <nixpkgs> { }, arch ? "aarch64" }:
let
  lib = pkgs.lib;
  llvm = pkgs.llvmPackages_23 or pkgs.llvmPackages_latest;
  debArch = { aarch64 = "arm64"; x86_64 = "amd64"; }.${arch}
    or (throw "scripts/nix: arch is aarch64 or x86_64, not ${arch}");
  triple = "${arch}-linux-gnu";
  lockFile = ./. + "/sysroot-${debArch}.lock.json";
  lock =
    if builtins.pathExists lockFile then builtins.fromJSON (builtins.readFile lockFile)
    else throw "scripts/nix/sysroot-${debArch}.lock.json is missing: ./build_docker.sh --arch ${arch} lock writes it";
  # Store names allow neither '~' nor '%', which Debian file names may hold.
  storeName = url: builtins.replaceStrings [ "~" "%" ] [ "_" "_" ] (baseNameOf url);

  # A NAME=value line of scripts/linux/deps.sh
  depsLines = lib.splitString "\n" (builtins.readFile ../linux/deps.sh);
  pin = name:
    let line = lib.findFirst (l: lib.hasPrefix "${name}=" l) null depsLines;
    in if line == null then throw "scripts/linux/deps.sh has no ${name}= line"
       else lib.head (lib.splitString " " (lib.removePrefix "${name}=" line));

  # A git tag, held to what deps.sh pins it to unpack to (its tree_is: the SHA-256 of the files' sha256sum lines, by
  # name; anything but files and folders refused), so a tag that moved stops the build instead of changing what is
  # compiled.
  # (builtins.fetchGit by tag: build_docker.sh does not evaluate in pure mode)
  tagged = name: url: tag: tree:
    let src = builtins.fetchGit { inherit url; ref = "refs/tags/${tag}"; shallow = true; };
    in pkgs.runCommand "longshot-src-${name}-${tag}" { } ''
      cd ${src}
      other=$(find . ! -type f ! -type d -print -quit)
      got=$(find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
      if [ -n "$other" ] || [ "$got" != ${tree} ]; then
        echo "${name} ${tag}: not what scripts/linux/deps.sh pins" \
          "(''${other:+$other is not a file; }its files hash to $got, expected ${tree})" >&2
        exit 1
      fi
      cp -r . $out
    '';

  # build_docker.sh setup: prebuilt programs the setup AppImage carries, which run on the handheld itself, outside Nix:
  # LLVM's clang and lld, python-build-standalone, and appimagetool with the type-2 runtime that packs it. aarch64: the
  # TotK port's setup pins (its tools/make_release_linux.py PINS): LLVM 23.1.4, which reads the play profiles as 23.1.5
  # does, and Python 3.14, which Longshot's Windows builder runs too (tools.lock). x86_64: scripts/linux/deps.sh's.
  release = {
    aarch64 = {
      llvm = {
        url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-Linux-ARM64.tar.xz";
        sha256 = "9b27cf263d711aa6ebfd4045ab241d947bd09b3cd19ffa5728f48a80c9eb3a91";
      };
      python = {
        url = "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/"
          + "cpython-3.14.10%2B20261026-aarch64-unknown-linux-gnu-install_only.tar.gz";
        sha256 = "dcc31ce7b9ddbbffac160da7fff0cc069054f43eca52382a7d0eebf94e985b88";
      };
      appimagetool = {
        url = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-aarch64.AppImage";
        sha256 = "f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158";
      };
      runtime = {
        url = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64";
        sha256 = "00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444";
      };
    };
    x86_64 = {
      llvm = {
        url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-${pin "LLVM_VER"}/"
          + "LLVM-${pin "LLVM_VER"}-Linux-X64.tar.xz";
        sha256 = pin "LLVM_SHA256";
      };
      python = {
        url = "https://github.com/astral-sh/python-build-standalone/releases/download/${pin "PY_TAG"}/"
          + "cpython-${pin "PY_VER"}%2B${pin "PY_TAG"}-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz";
        sha256 = pin "PY_SHA256";
      };
      appimagetool = {
        url = "https://github.com/AppImage/appimagetool/releases/download/${pin "APPIMAGETOOL"}/"
          + "appimagetool-x86_64.AppImage";
        sha256 = pin "APPIMAGETOOL_SHA256";
      };
      runtime = {
        url = "https://github.com/AppImage/type2-runtime/releases/download/${pin "APPIMAGE_RUNTIME"}/runtime-x86_64";
        sha256 = pin "APPIMAGE_RUNTIME_SHA256";
      };
    };
  }.${arch};
  # (a fixed name: '%' in a URL is not allowed in a store name)
  released = what: name: pkgs.fetchurl { inherit (release.${what}) url sha256; inherit name; };
in
rec {
  inherit triple;

  # Nix's clang for the target, without Nix's cc-wrapper (clang-wrapper.sh), next to lld and the LLVM tools.
  # nx2/make_game.py finds it as EMSDK/upstream/bin (build_docker.sh links it there); llvm-profdata reads the play
  # profiles.
  toolchain = pkgs.runCommand "longshot-llvm-${llvm.clang-unwrapped.version}" { } ''
    mkdir -p $out/bin
    for t in clang clang++; do
      substitute ${./clang-wrapper.sh} $out/bin/$t --subst-var-by shell ${pkgs.runtimeShell} \
        --subst-var-by clang ${llvm.clang-unwrapped}
      chmod +x $out/bin/$t
    done
    for t in lld ld.lld llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip llvm-readelf llvm-readobj llvm-objdump \
             llvm-profdata; do
      for d in ${llvm.lld}/bin ${llvm.llvm}/bin; do
        if [ -e $d/$t ]; then ln -s $d/$t $out/bin/$t; break; fi
      done
    done
  '';

  # Ubuntu 24.04's files unpacked as the packages ship them, /usr merged as on an installed system (the C library's
  # linker scripts and the dynamic linker's path name /lib/...). Absolute symlinks are made relative, so they point into
  # the sysroot.
  sysroot = pkgs.runCommand "longshot-sysroot-ubuntu2404-${debArch}"
    {
      nativeBuildInputs = [ pkgs.dpkg ];
      debs = map (p: pkgs.fetchurl { inherit (p) url sha256; name = storeName p.url; }) lock.packages;
    } ''
    mkdir -p $out
    for d in $debs; do dpkg-deb -x "$d" $out; done
    chmod -R u+w $out
    for d in lib lib64 bin sbin; do
      if [ -d $out/$d ] && [ ! -L $out/$d ]; then
        mkdir -p $out/usr/$d
        cp -a $out/$d/. $out/usr/$d/
        rm -rf $out/$d
      fi
      if [ -d $out/usr/$d ] && [ ! -e $out/$d ]; then ln -s usr/$d $out/$d; fi
    done
    find $out -type l | while IFS= read -r l; do
      t=$(readlink "$l")
      case $t in
        /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$out$t")" "$l" ;;
      esac
    done
  '';

  # meson's cross file for dav1d: the host machine is the target, against the sysroot (nothing it builds is run here)
  mesonCross = pkgs.writeText "longshot-${arch}-cross.txt" ''
    [binaries]
    c = '${toolchain}/bin/clang'
    cpp = '${toolchain}/bin/clang++'
    ar = '${toolchain}/bin/llvm-ar'
    strip = '${toolchain}/bin/llvm-strip'

    [built-in options]
    c_args = ['--target=${triple}', '--sysroot=${sysroot}']
    c_link_args = ['--target=${triple}', '--sysroot=${sysroot}', '-fuse-ld=lld']

    [host_machine]
    system = 'linux'
    cpu_family = '${arch}'
    cpu = '${arch}'
    endian = 'little'
  '';

  # CMake's toolchain file for SDL2: the same compiler and sysroot
  cmakeToolchain = pkgs.writeText "longshot-${arch}.cmake" ''
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR ${arch})
    set(CMAKE_SYSROOT ${sysroot})
    set(CMAKE_C_COMPILER ${toolchain}/bin/clang)
    set(CMAKE_C_COMPILER_TARGET ${triple})
    set(CMAKE_CXX_COMPILER ${toolchain}/bin/clang++)
    set(CMAKE_CXX_COMPILER_TARGET ${triple})
    set(CMAKE_AR ${toolchain}/bin/llvm-ar)
    set(CMAKE_RANLIB ${toolchain}/bin/llvm-ranlib)
    set(CMAKE_EXE_LINKER_FLAGS_INIT -fuse-ld=lld)
    set(CMAKE_SHARED_LINKER_FLAGS_INIT -fuse-ld=lld)
    set(CMAKE_FIND_ROOT_PATH ${sysroot})
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    # (the sysroot's own folders, named: nixpkgs' CMake searches no /usr of its own, so find_library and find_path found
    # nothing there, and SDL2 left out what it finds that way, X11 and ALSA, without a word. Game Mode's gamescope gives
    # its programs an X11 display only: the setup window and the game got SDL's offscreen driver there)
    set(CMAKE_LIBRARY_ARCHITECTURE ${triple})
    set(CMAKE_LIBRARY_PATH ${sysroot}/usr/lib/${triple} ${sysroot}/lib/${triple} ${sysroot}/usr/lib)
    set(CMAKE_INCLUDE_PATH ${sysroot}/usr/include/${triple} ${sysroot}/usr/include)
  '';

  # dav1d, static and position independent, as scripts/linux/deps.sh builds it (the movies are AV1). stdenv, not
  # stdenvNoCC: meson wants a compiler for the build machine too, though nothing is built for it
  dav1d = pkgs.stdenv.mkDerivation {
    pname = "longshot-dav1d-${arch}";
    version = pin "DAV1D_VER";
    src = tagged "dav1d" "https://code.videolan.org/videolan/dav1d.git" (pin "DAV1D_VER") (pin "DAV1D_TREE");
    nativeBuildInputs = [ pkgs.meson pkgs.ninja toolchain ] ++ lib.optional (arch == "x86_64") pkgs.nasm;
    dontFixup = true;   # (a target library: no Nix patchelf or strip)
    configurePhase = ''
      runHook preConfigure
      meson setup build --cross-file ${mesonCross} --prefix=$out --libdir=lib --buildtype=release \
        --default-library=static -Db_staticpic=true -Denable_tools=false -Denable_tests=false -Denable_examples=false
      runHook postConfigure
    '';
    buildPhase = "ninja -C build";
    installPhase = "ninja -C build install";
  };

  # FFmpeg's libavcodec, libswscale, libswresample and libavutil, static and LGPL, with dav1d: scripts/linux/deps.sh's
  # configuration, compiled against the sysroot. LONGSHOT_FFMPEG: nx2/build.py ffmpeg_linux takes lib/*.a and include/
  ffmpeg = pkgs.stdenv.mkDerivation {
    pname = "longshot-ffmpeg-${arch}";
    version = pin "FFMPEG_VER";
    src = pkgs.fetchurl {
      url = "https://ffmpeg.org/releases/ffmpeg-${pin "FFMPEG_VER"}.tar.xz";
      sha256 = pin "FFMPEG_SHA256";
    };
    nativeBuildInputs = [ pkgs.pkg-config toolchain ] ++ lib.optional (arch == "x86_64") pkgs.nasm;
    dontFixup = true;
    enableParallelBuilding = true;
    configurePhase = ''
      runHook preConfigure
      export PKG_CONFIG_LIBDIR=${dav1d}/lib/pkgconfig
      unset PKG_CONFIG_PATH
      ./configure --prefix=$out --pkg-config-flags=--static \
        --enable-cross-compile --arch=${arch} --target-os=linux --sysroot=${sysroot} \
        --cc=${toolchain}/bin/clang --cxx=${toolchain}/bin/clang++ --ld=${toolchain}/bin/clang \
        --ar=${toolchain}/bin/llvm-ar --ranlib=${toolchain}/bin/llvm-ranlib --nm=${toolchain}/bin/llvm-nm \
        --strip=${toolchain}/bin/llvm-strip \
        --extra-cflags=--target=${triple} --extra-ldflags="--target=${triple} -fuse-ld=lld" \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-network \
        --disable-autodetect --disable-everything --disable-avformat --disable-avfilter --disable-avdevice \
        --enable-avcodec --enable-swscale --enable-swresample --enable-avutil \
        --enable-libdav1d --enable-decoder=vp8,vp9,libdav1d --enable-parser=vp8,vp9,av1
      runHook postConfigure
    '';
    postInstall = ''
      cp ${dav1d}/lib/libdav1d.a $out/lib/
      rm -rf $out/share $out/lib/pkgconfig
      # (the licences, named as scripts/linux/deps.sh names them: the setup AppImage carries this folder)
      cp COPYING.LGPLv2.1 $out/FFmpeg-COPYING.LGPLv2.1
      cp LICENSE.md $out/FFmpeg-LICENSE.md
      cp ${dav1d.src}/COPYING $out/dav1d-COPYING
    '';
  };

  # SDL2, as scripts/linux/deps.sh builds it, against the sysroot: libSDL2-2.0.so.0 goes beside the game
  # (LONGSHOT_SDL2), which loads it at run time (host/posix). Its window, sound and input back ends load their own
  # libraries at run time, so it needs only glibc from the system.
  sdl2 = pkgs.stdenvNoCC.mkDerivation {
    pname = "longshot-sdl2-${arch}";
    version = pin "SDL_VER";
    src = pkgs.fetchurl {
      url = "https://github.com/libsdl-org/SDL/releases/download/release-${pin "SDL_VER"}/SDL2-${pin "SDL_VER"}.tar.gz";
      sha256 = pin "SDL_SHA256";
    };
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.wayland-scanner toolchain ];
    dontFixup = true;
    preConfigure = ''
      export PKG_CONFIG_SYSROOT_DIR=${sysroot}
      export PKG_CONFIG_LIBDIR=${sysroot}/usr/lib/${triple}/pkgconfig:${sysroot}/usr/share/pkgconfig
      unset PKG_CONFIG_PATH
    '';
    cmakeFlags = [
      "-DCMAKE_TOOLCHAIN_FILE=${cmakeToolchain}"
      # stdenvNoCC passes empty CMAKE_AR/CMAKE_RANLIB cache entries; override them explicitly for the cross build.
      "-DCMAKE_AR=${toolchain}/bin/llvm-ar" "-DCMAKE_RANLIB=${toolchain}/bin/llvm-ranlib"
      "-DCMAKE_BUILD_TYPE=Release"
      "-DSDL_SHARED=ON" "-DSDL_STATIC=OFF" "-DSDL_TEST=OFF"
      "-DSDL_X11=ON" "-DSDL_WAYLAND=ON" "-DSDL_PULSEAUDIO=ON" "-DSDL_PIPEWIRE=ON" "-DSDL_ALSA=ON"
    ];
    # (a back end SDL2 didn't find is left out without a word, and on the device SDL then takes its offscreen driver: a
    # window nobody sees. So the libraries the window, sound and input load at run time must be named in it)
    postInstall = ''
      install -Dm644 ../LICENSE.txt $out/share/licenses/SDL2-LICENSE.txt
      for l in libX11.so.6 libXext.so.6 libXi.so.6 libwayland-client.so.0 libxkbcommon.so.0 libasound.so.2 libpulse.so.0; do
        grep -qa "$l" $out/lib/libSDL2-2.0.so.0 || { echo "SDL2 was built without $l: that back end is off"; exit 1; }
      done
    '';
  };

  # The Vulkan back end's headers at deps.sh's tags, each a package root with include/ in it (nx2/build.py VK_HEADERS)
  vulkanHeaders = tagged "vulkan-headers" "https://github.com/KhronosGroup/Vulkan-Headers.git"
    (pin "VK_HEADERS") (pin "VK_HEADERS_TREE");
  spirvHeaders = tagged "spirv-headers" "https://github.com/KhronosGroup/SPIRV-Headers.git"
    (pin "SPIRV_HEADERS") (pin "SPIRV_HEADERS_TREE");
  vma = tagged "vma" "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git"
    (pin "VMA") (pin "VMA_TREE");
  headers = pkgs.runCommand "longshot-vulkan-headers" { } ''
    mkdir -p $out
    ln -s ${vulkanHeaders} $out/vulkanheaders
    ln -s ${spirvHeaders} $out/spirv-headers
    ln -s ${vma} $out/vulkanmemoryallocator
  '';

  # ---- the setup AppImage's parts (build_docker.sh setup) ----

  # The same headers as a CPM cache, as scripts/linux/deps.sh lays it out for tools.lock's cpm (RX_CPM):
  # <package>/<tag>/include with the licences
  cpm = pkgs.runCommand "longshot-cpm" { } ''
    put() {
      mkdir -p $out/$1/$2
      cp -r $3/include $out/$1/$2/
      for f in $3/LICENSE*; do if [ -f "$f" ]; then cp "$f" $out/$1/$2/; fi; done
    }
    put vulkanheaders ${pin "VK_HEADERS"} ${vulkanHeaders}
    put spirv-headers ${pin "SPIRV_HEADERS"} ${spirvHeaders}
    put vulkanmemoryallocator ${pin "VMA"} ${vma}
  '';

  # LLVM's release for the target, as scripts/linux/deps.sh keeps it (tools.lock's emsdk: upstream/bin and clang's own
  # headers and runtimes), untouched by Nix. Its clang.cfg and clang++.cfg make the carried sysroot and lld the
  # defaults, for what is compiled with no target flags (a64emit and zbic, which the AppImage builds and carries);
  # nx2/build.py names its own (LONGSHOT_SYSROOT), which come after them and win.
  setupLlvm = pkgs.runCommand "longshot-setup-llvm-${arch}" { nativeBuildInputs = [ pkgs.xz ]; } ''
    mkdir -p x $out/upstream/bin $out/upstream/lib
    tar -xJf ${released "llvm" "llvm-${arch}.tar.xz"} -C x --strip-components=1 --wildcards \
      '*/bin/clang' '*/bin/clang++' '*/bin/clang-[0-9]*' '*/bin/lld' '*/bin/ld.lld' '*/bin/llvm-profdata' \
      '*/lib/clang/*'
    mv x/bin/* $out/upstream/bin/
    mv x/lib/clang $out/upstream/lib/clang
    find $out/upstream/lib/clang/*/lib -type f ! -name 'libclang_rt.profile*.a' ! -name 'libclang_rt.builtins*.a' \
      -delete 2>/dev/null || true
    ${llvm.llvm}/bin/llvm-strip $out/upstream/bin/clang-[0-9]* $out/upstream/bin/lld $out/upstream/bin/llvm-profdata \
      2>/dev/null || true
    printf -- '--sysroot=<CFGDIR>/../../../sysroot\n-fuse-ld=lld\n' > $out/upstream/bin/clang.cfg
    cp $out/upstream/bin/clang.cfg $out/upstream/bin/clang++.cfg
  '';
  setupPython = released "python" "python-${arch}.tar.gz";
  appimagetool = released "appimagetool" "appimagetool-${arch}.AppImage";
  appimageRuntime = released "runtime" "runtime-${arch}";
}
