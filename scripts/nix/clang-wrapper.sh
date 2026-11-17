#!@shell@
# clang and clang++ for target builds (scripts/nix/default.nix toolchain; after the TotK port's
# tools/nix/clang-wrapper.sh): Nix's clang without Nix's cc-wrapper, which would add Nix's glibc and its dynamic linker:
# the sysroot (--sysroot) provides the C library instead. For a dynamic link it names the target's dynamic linker, which
# nixpkgs leaves out of clang's link command (its purity patch) and its cc-wrapper would otherwise add.
# (longshot) A command that names no sysroot gets LONGSHOT_SYSROOT's when that is set, and a link that names no linker
# gets lld: the translator and the unpacker the lift builds and loads into Python (a64emit, zbic) are compiled with no
# target flags (third_party/a64-kit/tools/toolchain.py host=True), and Nix's clang has no C library of its own.
link=1 static=0 target= sysroot= linker=
for a in "$@"; do
    case $a in
        -c | -S | -E | -M | -MM | -fsyntax-only | --version | -dumpmachine | -dumpversion | -print-* | '-###') link=0 ;;
        -static | -shared | -r | -nostdlib) static=1 ;;
        --target=*) target=${a#--target=} ;;
        --sysroot | --sysroot=* | -isysroot*) sysroot=1 ;;
        -fuse-ld=* | --ld-path=*) linker=1 ;;
    esac
done
if [ -z "$sysroot" ] && [ -n "${LONGSHOT_SYSROOT:-}" ]; then set -- "$@" "--sysroot=$LONGSHOT_SYSROOT"; fi
if [ "$link" = 1 ] && [ -z "$linker" ]; then set -- "$@" -fuse-ld=lld; fi
case ${target:-$(uname -m)} in
    aarch64*) dl=/lib/ld-linux-aarch64.so.1 ;;
    x86_64*) dl=/lib64/ld-linux-x86-64.so.2 ;;
    *) dl= ;;
esac
tool=@clang@/bin/$(basename "$0")
if [ "$link" = 1 ] && [ "$static" = 0 ] && [ -n "$dl" ]; then
    exec "$tool" "$@" -Wl,--dynamic-linker="$dl"
fi
exec "$tool" "$@"
