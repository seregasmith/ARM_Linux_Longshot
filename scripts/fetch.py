"""The upscalers' vendor files a local build or game folder can use, which Longshot never commits or ships: each fetched
from its vendor's GitHub repository at one pinned commit, each file checked against its SHA-256, once the vendor's license
is accepted (its flag, or a typed yes when asked; nothing is downloaded without it).

    python -I scripts/fetch.py dlss [--accept-nvidia-license] [--platform linux|windows] [--game <game folder>]
                                    [--runtime-dir <folder>] [--build-only] [<sdk folder>]
        NVIDIA's DLSS SDK (v310.9.16), for a build with NVIDIA DLSS (LONGSHOT_NGX_SDK, gpu/vk/vk_dlss.cpp), into <sdk folder>
        (default sdk/ngx-<platform> in the development folder, `dev.py paths sdk`, where dev.py's builds look for it):
        LICENSE.txt and the headers (include/), the library the build links (lib/: libnvsdk_ngx.a on Linux, the /MT
        nvsdk_ngx_s.lib on Windows) and the runtimes NGX
        loads (runtime/: DLSS's and DLSS Frame Generation's; without the latter beside DLSS's, NGX takes the driver's own,
        which may be older). The NGX core comes with NVIDIA's driver (libnvidia-ngx.so; _nvngx.dll, which is loaded from the
        driver's folder and is never to be put beside the game).
          --game: the runtimes copied into <game folder>/ngx too, where the game looks first (nx2/make_game.py does the same
                  for the folder it builds when LONGSHOT_NGX_SDK is set)
          --runtime-dir: the runtimes copied into that folder instead (Linux: $XDG_DATA_HOME/longshot/ngx; Windows:
                  %LOCALAPPDATA%\\Longshot\\ngx. RX_VK_DLSS_DIR names another when the game runs)
          --build-only: only what the build reads, not the runtimes (CI's link check)
        Then build with LONGSHOT_NGX_SDK=<sdk folder>.
    python -I scripts/fetch.py fsr [--accept-amd-license] [--game <game folder>] [<folder>]
        AMD's FSR runtime for Vulkan (FSR SDK 2.4: FSR 4.1.2), for AMD FSR 4 (gpu/vk/vk_fsr4.cpp), into <folder> (default
        sdk/ffx in the development folder): amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll and
        LICENSE-AMD-FSR-SDK.md. Windows only: AMD publishes the runtime for Windows alone (Linux takes a Vulkan FSR 4 library
        of its own: the readme)
    python -I scripts/fetch.py xess [--accept-intel-license] [--game <game folder>] [<folder>]
        Intel's XeSS Super Resolution library (libxess.dll 2.0.2.68), for Intel XeSS (gpu/vk/vk_xess.cpp), into <folder>
        (default sdk/xess in the development folder), from the commit third_party/xess's headers come from
        (third_party/xess/PROVENANCE.md), with LICENSE-INTEL-XESS-SDK.txt and THIRD-PARTY-INTEL-XESS-SDK.txt. Windows only:
        Intel publishes no XeSS library for Linux
    fsr and xess --game: the files copied into <game folder> too, where the game looks after RX_VK_FSR_DIR / RX_VK_XESS_DIR
        and beside the executable (nx2/make_game.py --fetch-fsr-runtime / --fetch-xess-runtime do the same for the folder
        they build). A player's own DLL there that is not the pinned one is left alone.

The vendors' files are theirs, under their own licenses, fetched beside them. AMD's and Intel's ask for no acceptance step;
one is asked all the same, as for NVIDIA's: one rule for all three, and nobody gets the files without having seen the terms.
nx2/make_game.py loads this file as a module (DLSS, FSR and XESS below); so do the tests.
"""
import argparse
import importlib.util
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder off the path)
import helpers  # noqa: E402
from helpers import ROOT, sha256_file as sha256  # noqa: E402

TIMEOUT = 60   # (seconds without an answer, or between two reads, before a download gives up: a stalled one ends the build)
TRIES = 3      # (a download cut off is made again, this many times in all)

# (each vendor's pinned commit: changing one changes the license text the player accepts, here and in the Longshot
# Launcher's App/ViewModels/SettingsStepViewModel.cs, which names the same commits)
DLSS_COMMIT = "9981c6350dc58c0ce21f024fd65e31e851c31689"   # (v310.9.16)
FSR_COMMIT = "7539ae8b6568ad29ed78a753404351daf3b5cca7"    # (tag v2.4.0: FSR 4.1.2, FSR 3.1.6)
XESS_COMMIT = "de0fb9c1c510661c571164e1418ceca8101dab69"   # (main after the v3.0.2 release: libxess.dll 2.0.2.68)


def copy_if_changed(src, dst, digest=None):
    """src copied to dst through dst.part, unless dst is there with src's bytes (digest: src's SHA-256, when known)"""
    if os.path.isfile(dst) and (sha256(dst) == digest if digest else
                                os.path.getsize(dst) == os.path.getsize(src) and sha256(dst) == sha256(src)):
        return
    shutil.copyfile(src, dst + ".part")
    os.replace(dst + ".part", dst)


class Sdk(object):
    """One vendor's files at one commit of its GitHub repository: the download, its SHA-256 check and the license asked"""

    def __init__(self, tag, vendor, flag, repo, commit, license_path, notice, default):
        self.tag = tag            # (the messages' prefix)
        self.vendor = vendor
        self.flag = flag          # (the command line's acceptance of the vendor's license)
        self.commit = commit
        self.base = "https://raw.githubusercontent.com/%s/%s" % (repo, commit)
        self.license_url = "https://github.com/%s/blob/%s/%s" % (repo, commit, license_path)
        self.notice = notice % self.license_url
        self.default = default    # (the folder's name in the development folder's sdk/, when none is given)

    def fetch_one(self, path, digest, dst, opener=None, tries=1):
        """One file of the vendor's repository into dst, unless it is there already with that SHA-256 (False); a download
        with other bytes is not kept (ValueError)"""
        return helpers.download(self.base + "/" + path, dst, digest, timeout=TIMEOUT, tries=tries, opener=opener)

    def download(self, table, folder, opener=None, say=print):
        """Each (path in the vendor's repository, SHA-256, path in folder) of table into folder"""
        for path, digest, rel in table:
            if self.fetch_one(path, digest, os.path.join(folder, *rel.split("/")), opener, TRIES):
                say("%s: %s" % (self.tag, rel))

    def accepted(self, flag, ask=input, interactive=None):
        """The vendor's license, accepted: the flag, or a typed yes when there is someone to ask"""
        if flag:
            return True
        if not (sys.stdin.isatty() if interactive is None else interactive):
            return False
        try:
            return ask("Do you accept %s's license? Type yes to go on: " % self.vendor).strip().lower() in ("y", "yes")
        except EOFError:
            return False

    def default_folder(self, platform=None):
        return dev_sdk(self.default)


def dev_sdk(name):
    """sdk/<name> in this PC's development folder (scripts/devtools/common.py dev_home; loaded by its path, as make_game.py
    loads this file)"""
    spec = importlib.util.spec_from_file_location("devtools_common", os.path.join(ROOT, "scripts", "devtools", "common.py"))
    common = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(common)
    return os.path.join(common.dev_home(), "sdk", name)


# ---- NVIDIA DLSS: an SDK folder a build reads (LONGSHOT_NGX_SDK), per platform ----

def host_platform():
    """The platform a build here is for: Windows on Windows or with RX_TARGET=windows (a cross build), else Linux"""
    return "windows" if os.name == "nt" or os.environ.get("RX_TARGET", "").lower() == "windows" else "linux"


class Dlss(Sdk):
    RUNTIMES = ("runtime", "framegen")   # (DLSS's runtime, then DLSS Frame Generation's)

    def __init__(self, common, platforms, **kw):
        Sdk.__init__(self, **kw)
        self.common = common          # [(path in NVIDIA's repository, its SHA-256)]: what every platform's build reads
        self.platforms = platforms    # {platform: {"library" | "runtime" | "framegen": (path, SHA-256)}}

    @staticmethod
    def destination(path):
        """Where a file of NVIDIA's repository goes in the SDK folder (a relative path)"""
        if path.startswith("lib/"):
            return ("runtime/" if "/rel/" in path else "lib/") + path.rsplit("/", 1)[1]
        return path

    def files(self, platform, runtime=True):
        """[(path in NVIDIA's repository, SHA-256, path in the SDK folder)] for one platform; runtime=False: what a build
        reads only (not the runtimes)"""
        spec = self.platforms[platform]
        extra = [spec["library"]] + ([spec[k] for k in self.RUNTIMES] if runtime else [])
        return [(p, s, self.destination(p)) for p, s in self.common + extra]

    def library(self, sdk, platform):
        """The library a build with this SDK folder links, for that platform"""
        return os.path.join(sdk, *self.destination(self.platforms[platform]["library"][0]).split("/"))

    def runtime(self, sdk, platform, kind="runtime"):
        """A runtime in this SDK folder, for that platform: DLSS's ("runtime") or DLSS Frame Generation's ("framegen")"""
        return os.path.join(sdk, *self.destination(self.platforms[platform][kind][0]).split("/"))

    def runtime_name(self, platform, kind="runtime"):
        return self.platforms[platform][kind][0].rsplit("/", 1)[1]

    def fetch(self, sdk, platform, opener=None, say=print, runtime=True):
        """Every file of that platform into the SDK folder (runtime=False: not the runtimes)"""
        self.download(self.files(platform, runtime), sdk, opener, say)

    def install_runtime(self, sdk, platform, folder):
        """The SDK folder's DLSS runtime copied into folder (made when missing), where the game finds it, and DLSS Frame
        Generation's beside it when the SDK folder has it (one fetched before it was in the table has not: fetch() adds it).
        Returns DLSS's runtime there"""
        os.makedirs(folder, exist_ok=True)
        placed = []
        for kind in self.RUNTIMES:
            src = self.runtime(sdk, platform, kind)
            if kind != "runtime" and not os.path.isfile(src):
                continue
            dst = os.path.join(folder, self.runtime_name(platform, kind))
            copy_if_changed(src, dst)
            placed.append(dst)
        return placed[0]

    def default_folder(self, platform=None):
        """sdk/ngx-linux or sdk/ngx-windows in the development folder: dev.py's builds take the one for their system
        (nx2/make_game.py keeps its own, for the Builder: work/ngx-sdk)"""
        return dev_sdk("ngx-" + (platform or host_platform()))


DLSS = Dlss(
    tag="ngx-sdk", vendor="NVIDIA", flag="--accept-nvidia-license", repo="NVIDIA/DLSS", commit=DLSS_COMMIT,
    license_path="LICENSE.txt", default=None,
    notice="""\
NVIDIA DLSS: this downloads NVIDIA's DLSS SDK (release v310.9.16) from github.com/NVIDIA/DLSS.
NVIDIA's files are NVIDIA's, not Longshot's, and are under NVIDIA's own license, the NVIDIA RTX SDKs license:
  %s
(saved as LICENSE.txt beside the files). Using them means accepting it. Longshot never includes or redistributes them:
they stay on this PC, in the SDK folder and the game folder's ngx folder. Never share a game folder built with them.""",
    common=[
        ("LICENSE.txt", "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"),
        ("include/nvsdk_ngx_defs_dlssd.h", "2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716"),
        ("include/nvsdk_ngx_defs_dlssg.h", "c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1"),
        ("include/nvsdk_ngx_defs.h", "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"),
        ("include/nvsdk_ngx_defs_vk.h", "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"),
        ("include/nvsdk_ngx.h", "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"),
        ("include/nvsdk_ngx_helpers_cuda.h", "f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"),
        ("include/nvsdk_ngx_helpers_d3d.h", "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"),
        ("include/nvsdk_ngx_helpers_dlssd_cuda.h", "7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730"),
        ("include/nvsdk_ngx_helpers_dlssd_d3d.h", "6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293"),
        ("include/nvsdk_ngx_helpers_dlssd.h", "a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532"),
        ("include/nvsdk_ngx_helpers_dlssd_vk.h", "11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b"),
        ("include/nvsdk_ngx_helpers_dlssg_d3d.h", "6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220"),
        ("include/nvsdk_ngx_helpers_dlssg.h", "6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f"),
        ("include/nvsdk_ngx_helpers_dlssg_vk.h", "b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316"),
        ("include/nvsdk_ngx_helpers.h", "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"),
        ("include/nvsdk_ngx_helpers_vk.h", "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"),
        ("include/nvsdk_ngx_loader.h", "abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75"),
        ("include/nvsdk_ngx_params_dlssd.h", "d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e"),
        ("include/nvsdk_ngx_params_dlssg.h", "c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad"),
        ("include/nvsdk_ngx_params.h", "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"),
        ("include/nvsdk_ngx_standalone_common.h", "38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53"),
        ("include/nvsdk_ngx_standalone_cuda.h", "b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499"),
        ("include/nvsdk_ngx_vk.h", "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"),
    ],
    # each platform's library (into lib/), DLSS's runtime and DLSS Frame Generation's (both into runtime/, the game's ngx)
    platforms={
        "linux": {
            "library": ("lib/Linux_x86_64/libnvsdk_ngx.a", "d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df"),
            "runtime": ("lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16",
                        "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6"),
            "framegen": ("lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16",
                         "574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7"),
        },
        "windows": {
            # (the static NGX loader built against the static C runtime (/MT); llvm-mingw links it with
            # gpu/vk/vk_ngx_msvc_rt.cpp)
            "library": ("lib/Windows_x86_64/x64/nvsdk_ngx_s.lib",
                        "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"),
            "runtime": ("lib/Windows_x86_64/rel/nvngx_dlss.dll",
                        "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"),
            "framegen": ("lib/Windows_x86_64/rel/nvngx_dlssg.dll",
                         "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"),
        },
    })


# ---- AMD FSR 4 and Intel XeSS: a runtime the game loads from its folder (Windows) ----

class Runtime(Sdk):
    def __init__(self, files, release, **kw):
        Sdk.__init__(self, **kw)
        self.files = files        # [(path in the vendor's repository, its SHA-256, its name here)]
        self.release = release    # (the pinned release, as the messages name it)

    def dlls(self):
        return [name for _, _, name in self.files if name.endswith(".dll")]

    def fetch(self, folder, opener=None, say=print):
        """Every file into folder"""
        self.download(self.files, folder, opener, say)

    def complete(self, folder):
        """Whether folder holds every file, each the one expected"""
        return all(os.path.isfile(os.path.join(folder, n)) and sha256(os.path.join(folder, n)) == d for _, d, n in self.files)

    def others(self, game):
        """The vendor's DLLs in game that are not the pinned ones (a newer SDK's the player put there by hand, say)"""
        digests = {name: digest for _, digest, name in self.files}
        return [n for n in self.dlls()
                if os.path.isfile(os.path.join(game, n)) and sha256(os.path.join(game, n)) != digests[n]]

    def install(self, folder, game, say=print):
        """The files copied from folder into game (made when missing): where the game finds them. The vendor's DLLs the player
        put there already, other than the pinned ones, are left alone and nothing is copied (the game takes the newest FSR 4
        the loader lists, so replacing them could be a downgrade; its log names the XeSS it loads), and say tells which.
        Returns game"""
        os.makedirs(game, exist_ok=True)
        kept = self.others(game)
        if kept:
            one = len(kept) == 1
            say("%s: %s in %s %s not %s's: left as %s (delete %s to get %s's)" % (
                self.tag, " and ".join(kept), game, "is" if one else "are", self.release, "it is" if one else "they are",
                "it" if one else "them", self.release))
            return game
        for _, digest, name in self.files:
            copy_if_changed(os.path.join(folder, name), os.path.join(game, name), digest)
        return game


FSR = Runtime(
    tag="ffx-sdk", vendor="AMD", flag="--accept-amd-license", repo="GPUOpen-LibrariesAndSDKs/FidelityFX-SDK",
    commit=FSR_COMMIT, license_path="Kits/FidelityFX/docs/license.md", default="ffx", release="FSR SDK v2.4.0",
    # (AMD's license grants installing, copying and redistributing these in binary form, with its notices kept and no
    # reverse engineering)
    notice="""\
AMD FSR 4: this downloads AMD's FSR runtime for Vulkan (FSR SDK v2.4.0: FSR 4.1.2) from
github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK. AMD's files are AMD's, not Longshot's, signed by AMD and under AMD's
FSR SDK license (saved as LICENSE-AMD-FSR-SDK.md beside them):
  %s""",
    files=[
        ("Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll",
         "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9", "amd_fidelityfx_loader_vk.dll"),
        ("Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll",
         "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7", "amd_fidelityfx_upscaler_vk.dll"),
        ("Kits/FidelityFX/docs/license.md",
         "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb", "LICENSE-AMD-FSR-SDK.md"),
    ])

XESS = Runtime(
    tag="xess-sdk", vendor="Intel", flag="--accept-intel-license", repo="intel/xess", commit=XESS_COMMIT,
    license_path="LICENSE.txt", default="xess", release="XeSS 2.0.2.68",
    # (Intel's license grants using and redistributing these unmodified, in binary form, with its notice kept and no
    # reverse engineering)
    notice="""\
Intel XeSS: this downloads Intel's XeSS Super Resolution library for Windows (libxess.dll 2.0.2.68, 78 MB) from
github.com/intel/xess. Intel's files are Intel's, not Longshot's, signed by Intel and under the Intel Simplified Software
License (saved as LICENSE-INTEL-XESS-SDK.txt beside them):
  %s""",
    files=[
        ("bin/libxess.dll", "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7", "libxess.dll"),
        ("LICENSE.txt", "784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df", "LICENSE-INTEL-XESS-SDK.txt"),
        ("third-party-programs.txt", "e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c",
         "THIRD-PARTY-INTEL-XESS-SDK.txt"),
    ])

SDKS = {"dlss": DLSS, "fsr": FSR, "xess": XESS}
ABOUT = {"dlss": "NVIDIA's DLSS SDK, for a build with NVIDIA DLSS (LONGSHOT_NGX_SDK)",
         "fsr": "AMD's FSR 4 runtime for Vulkan (Windows)", "xess": "Intel's XeSS library (Windows)"}


def say(line, err=False):
    print(line, file=sys.stderr if err else sys.stdout, flush=True)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="sdk", required=True)
    for name, sdk in SDKS.items():
        s = sub.add_parser(name, help=ABOUT[name])
        s.add_argument("folder", nargs="?", help="where the files go (default sdk/%s in the development folder, "
                                                 "`dev.py paths sdk`)" % ("ngx-<platform>" if sdk is DLSS else sdk.default))
        s.add_argument(sdk.flag, dest="accept", action="store_true", help="%s's license, accepted without the question"
                       % sdk.vendor)
        s.add_argument("--game", help="a game folder: the runtime into it too")
    s = sub.choices["dlss"]
    s.add_argument("--platform", choices=sorted(DLSS.platforms), default=host_platform())
    s.add_argument("--runtime-dir", help="the DLSS runtimes into this folder")
    s.add_argument("--build-only", action="store_true", help="not DLSS's runtimes: only what the build reads")
    a = p.parse_args(argv)
    sdk = SDKS[a.sdk]
    if sdk is DLSS and a.build_only and (a.game or a.runtime_dir):
        s.error("--build-only fetches no runtime for --game or --runtime-dir")
    folder = os.path.abspath(a.folder) if a.folder else sdk.default_folder(getattr(a, "platform", None))

    say(sdk.notice)
    if not sdk.accepted(a.accept):
        say("%s: %s's license was not accepted (%s, or yes when asked): nothing downloaded" % (sdk.tag, sdk.vendor, sdk.flag),
            err=True)
        return 2
    try:
        if sdk is DLSS:
            sdk.fetch(folder, a.platform, runtime=not a.build_only)
        else:
            sdk.fetch(folder)
    except (OSError, ValueError) as e:
        say("%s: %s" % (sdk.tag, e), err=True)
        return 1

    if sdk is DLSS:
        say("ngx-sdk: the SDK's headers and library in %s (NVIDIA's license: %s)" % (folder, os.path.join(folder, "LICENSE.txt")))
        for target in ([os.path.join(a.game, "ngx")] if a.game else []) + ([a.runtime_dir] if a.runtime_dir else []):
            say("ngx-sdk: the DLSS runtime in %s" % sdk.install_runtime(folder, a.platform, target))
            say("ngx-sdk: DLSS Frame Generation's in %s" % os.path.join(target, sdk.runtime_name(a.platform, "framegen")))
        say("ngx-sdk: build with LONGSHOT_NGX_SDK=%s" % folder)
    else:
        say("%s: %s's %s in %s" % (sdk.tag, sdk.vendor, "FSR runtime" if sdk is FSR else "XeSS library", folder))
        if a.game:
            say("%s: copied into %s" % (sdk.tag, sdk.install(folder, a.game, say=say)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
