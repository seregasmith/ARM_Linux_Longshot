"""Export a prepared Turnip cache for the Linux Setup AppImage."""

import argparse
from contextlib import contextmanager, ExitStack
import ctypes
from datetime import datetime
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import stat
import struct
import sys
import tempfile
import zipfile


SOURCE = "longshot_shader_dump_source.json"
ACTIVE = "shader-dump.active"
RUNTIME = "shader-dump.runtime"
RUNTIME_MAGIC = "longshot-shader-dump-v1"
PREPARED = "@@shader_dump "
LAYOUT = "mesa_shader_cache_sf"
CHUNK = 1 << 20
PROGRAMS = {b"HSP1": ("longshot_programs.bin", "longshot_programs.idx"),
            b"HSP2": ("longshot_programs.zst.bin", "longshot_programs.zst.idx")}
MESA_VERSION = r"[0-9]+\.[0-9]+\.[0-9]+[A-Za-z0-9.+-]*"
OPTIONAL = (
    "longshot_module_ids.bin", "longshot_compute_variants.bin", "longshot_computes.bin",
    "longshot_pipeline_states.bin", "longshot_pipeline_states_v1.bin", "longshot_pipelines.bin.built",
    "longshot_pipelines.bin.predicted", "longshot_pipelines.bin.predicted_keys", "longshot_vk_pipelines.bin",
    "longshot_vk_pipelines.stamp", "longshot_vk_pipelines.base",
)


class DumpError(ValueError):
    pass


@contextmanager
def cache_lock(path, *, exclusive):
    import fcntl

    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        except BlockingIOError as e:
            raise DumpError("The shader cache is in use. Close the game and finish any build or dump first.") from e
        yield
    finally:
        os.close(fd)


def shader_seed_hash(game):
    with (game / "longshot_shaders.bin").open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def legacy_seed_hash(path):
    """Return SHA-256 and the seed hash from vk_draw.cpp's seed_file_hash, in one pass."""
    mask = (1 << 64) - 1
    p1, p2 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F
    lanes = [(p1 + p2) & mask, p2, 0, (-p1) & mask]
    tail, size = 0xCBF29CE484222325, 0
    digest = hashlib.sha256()

    def mix(h, word):
        h = (h + word * p2) & mask
        return (((h << 31) | (h >> 33)) * p1) & mask

    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(CHUNK), b""):
            digest.update(chunk)
            size += len(chunk)
            end = len(chunk) & ~31
            for a, b, c, d in struct.iter_unpack("<4Q", memoryview(chunk)[:end]):
                lanes[0] = mix(lanes[0], a)
                lanes[1] = mix(lanes[1], b)
                lanes[2] = mix(lanes[2], c)
                lanes[3] = mix(lanes[3], d)
            for byte in chunk[end:]:
                tail = ((tail ^ byte) * 1099511628211) & mask
    value = ((size * p1) & mask) ^ tail
    for lane in lanes:
        value = mix(value ^ mix(0, lane), p1)
    return digest.hexdigest(), value, size


def turnip_log(path):
    facts = re.compile(r"vk: policy: facts ([0-9a-f]+):([0-9a-f]+), driver id 18, version 0x([0-9a-f]+) "
                       rf"\(Mesa ({MESA_VERSION})(.*?)\), type ")
    program = re.compile(r"vk: program cache .*?/(longshot_programs\.(?:zst\.)?bin):")
    details = {}
    try:
        with path.open(encoding="utf-8", errors="replace") as log:
            for line in log:
                if line.startswith("[hos host] longshot "):
                    details["version"] = line.strip().removeprefix("[hos host] longshot ")
                elif match := facts.search(line):
                    details["facts"] = match.groups()
                elif match := program.search(line):
                    details["programs"] = match[1]
                elif match := re.search(r"vk: shader store (.*): [0-9]+ SPIR-V blobs", line):
                    details["store"] = Path(match[1]).expanduser()
                elif match := re.search(r"vk: Compile shaders: (Most|All)\b", line):
                    details["mode"] = match[1].lower()
                elif "vk: robust buffer access on" in line:
                    details["robust_buffers"] = True
                elif "vk: driver shader cache:" in line and f"layout {LAYOUT};" in line and "/cache/driver (" in line:
                    details["layout"] = True
                elif "vk: start-up: the shaders took " in line:
                    return details
    except FileNotFoundError:
        pass
    return {}


def warm_stamp(stamp, uuid, vendor, device, version, libraries):
    """Match vk_draw.cpp's warmed_stamp for recipe 2. Do not guess from the newest Mesa directory."""
    for byte in bytes.fromhex(uuid) + struct.pack("<5I", vendor, device, version, libraries, 2):
        stamp = ((stamp ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return stamp


def recover_preparation(game, stack):
    """Recover older installs' export metadata from the preparation stamp and a completed launch."""
    cache = game / "cache"
    try:
        stamp, seed_hash, seed_size, _, _, warmed = read_header(cache / "longshot_prepared.bin", "<6Q")
    except (OSError, DumpError) as e:
        raise DumpError("This install has no shader export metadata or usable shader preparation stamp.") from e
    layout = cache / "driver" / LAYOUT
    if not layout.is_dir():
        raise DumpError("This install has no Mesa single-file shader cache to export.")
    uuids = {
        p.name for p in layout.iterdir()
        if p.is_dir() and not p.is_symlink() and re.fullmatch(r"[0-9a-f]{32}", p.name)
        and any(db.is_file() for db in p.glob("*/foz_cache.foz"))
    }
    for name in ("longshot_log.txt", *(f"longshot_log.{i}.txt" for i in range(1, 5))):
        for folder in (game / "logs", game):
            run = turnip_log(folder / name)
            if not {"version", "facts", "programs", "layout"} <= run.keys():
                continue
            gpu, chip, ver, mesa, info = run["facts"]
            vendor, device, version = int(gpu, 16), int(chip, 16), int(ver, 16)
            matches = {uuid for uuid in uuids if any(warm_stamp(stamp, uuid, vendor, device, version, libraries) == warmed
                                                    for libraries in (0, 1, 3))}
            if len(matches) != 1:
                continue
            programs = cache / run["programs"]
            stack.enter_context(cache_lock(programs.with_name(programs.name + ".lock"), exclusive=True))
            magic, _, _ = read_header(programs, "<4sIQ")
            digest, actual_hash, size = legacy_seed_hash(game / "longshot_shaders.bin")
            if (actual_hash, size) != (seed_hash, seed_size):
                raise DumpError("The game shader seed changed since preparation.")
            return {
                "longshot_version": run["version"], "shader_seed_sha256": digest,
                "renderer_shader_cache_stamp": f"{stamp:016x}",
                "gpu": {"vendor_id": vendor, "device_id": device},
                "driver": {"id": 18, "version_id": version, "name": "Turnip", "info": f"Mesa {mesa}{info}",
                           "mesa_version": mesa, "pipeline_cache_uuid": matches.pop(), "cache_layout": LAYOUT},
                # Older runtimes report effective robust buffer access only when it is on.
                "shader_settings": {"compile_shaders": run.get("mode", "unknown"),
                                    "robust_buffers": run.get("robust_buffers", False)},
                "program_cache_format": magic.decode("ascii"), "spirv_store_path": str(run.get("store", "")),
            }
    raise DumpError("Setup could not match the existing shader preparation stamp to a Turnip launch in the game logs. "
                    "Keep a completed launch log from the driver that prepared the cache, then try again.")


def record_preparation(game, report, version):
    """Record the renderer's data only after its preparation process exits successfully."""
    cache = game / "cache"
    vendor, device, driver, version_id, uuid, stamp, mode, program_format, store, info = report.split("\t", 9)
    shader_settings = {"compile_shaders": mode}
    robust, sep, driver_info = info.partition("\t")
    if sep and robust in ("0", "1"):
        shader_settings["robust_buffers"] = robust == "1"
        info = driver_info
    mesa = re.match(rf"Mesa ({MESA_VERSION})(?=\s|$)", info)
    if not mesa or not re.fullmatch(r"[0-9a-f]{32}", uuid) or not re.fullmatch(r"[0-9a-f]{16}", stamp):
        raise DumpError("Shader preparation did not report a valid Turnip driver identity.")
    record = {
        "longshot_version": version,
        "shader_seed_sha256": shader_seed_hash(game),
        "renderer_shader_cache_stamp": stamp,
        "gpu": {"vendor_id": int(vendor), "device_id": int(device)},
        "driver": {"id": int(driver), "version_id": int(version_id), "name": "Turnip", "info": info,
                   "mesa_version": mesa[1], "pipeline_cache_uuid": uuid, "cache_layout": LAYOUT},
        "shader_settings": shader_settings,
        "program_cache_format": program_format,
        "spirv_store_path": store,
    }
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=cache, prefix=".shader-source-", delete=False) as f:
        temp = Path(f.name)
        try:
            json.dump(record, f)
            f.close()
            os.replace(temp, cache / SOURCE)
        finally:
            temp.unlink(missing_ok=True)


def read_header(path, layout):
    require_file(path)
    with path.open("rb") as f:
        data = f.read(struct.calcsize(layout))
    if len(data) != struct.calcsize(layout):
        raise DumpError(f"Incomplete shader cache file: {path.name}.")
    return struct.unpack(layout, data)


def require_file(path):
    if not stat.S_ISREG(path.lstat().st_mode):
        raise DumpError(f"Expected a regular shader cache file: {path}.")


def mesa_files(root):
    if root.resolve() != root or not root.is_dir():
        raise DumpError("The install-local Mesa single-file cache is missing.")
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise DumpError(f"The Mesa cache contains a link: {path}.")
        if path.is_dir():
            continue
        if path.name in {"foz_cache.foz", "foz_cache_idx.foz", "index"}:
            require_file(path)
            files.append(path)
    databases = [path for path in files if path.name == "foz_cache.foz"]
    if not databases:
        raise DumpError("No compiled Mesa shader cache was found.")
    for database in databases:
        for name in ("foz_cache_idx.foz", "index"):
            require_file(database.with_name(name))
    return files


def sources(game, cache, record, stack, *, seed_checked=False):
    if not seed_checked and shader_seed_hash(game) != record["shader_seed_sha256"]:
        raise DumpError("The game shader seed changed since preparation.")
    renderer_stamp = int(record["renderer_shader_cache_stamp"], 16)
    if read_header(cache / "longshot_prepared.bin", "<Q")[0] != renderer_stamp:
        raise DumpError("The preparation record doesn't match this shader dump source.")
    payload = [(p, f"driver/{p.relative_to(cache / 'driver').as_posix()}") for p in mesa_files(cache / "driver" / LAYOUT)]
    expected = record.pop("program_cache_format").encode("ascii")
    programs, index = PROGRAMS[expected]
    magic, store_id, stamp = read_header(cache / programs, "<4sIQ")
    if magic != expected or stamp != renderer_stamp:
        raise DumpError("No program cache matches this shader dump source.")
    if read_header(cache / index, "<4sIQ") != (b"HPIX", 1, stamp):
        raise DumpError("The program cache index doesn't match the shader cache.")
    required = (programs, index, "longshot_prepared.bin", "longshot_pipelines.bin")
    extra = [name for name in OPTIONAL if (cache / name).exists()]
    extra.extend(p.name for p in sorted(cache.glob("longshot_vk_pipelines_play_*.bin"))
                 if re.fullmatch(r"longshot_vk_pipelines_play_[0-9]+\.bin", p.name))
    for name in (*required, *extra):
        path = cache / name
        require_file(path)
        payload.append((path, f"longshot/{name}"))
    store_dir = record.pop("spirv_store_path")
    if store_id:
        if not store_dir:
            raise DumpError("The program cache needs its matching SPIR-V store.")
        store = Path(store_dir)
        stack.enter_context(cache_lock(store / "spirv.lock", exclusive=True))
        for name, tag in (("spirv.pack", b"LSPK"), ("spirv.idx", b"LSPI")):
            path = store / name
            if read_header(path, "<4sII") != (tag, 1, store_id):
                raise DumpError(f"The program cache needs a different SPIR-V store: {name}.")
            payload.append((path, f"spirv/{name}"))
    record["program_cache"] = {"format": magic.decode("ascii"), "store_id": store_id}
    return payload


def publish(temp, target):
    """Atomically publish a closed archive without overwriting another export."""
    if sys.platform == "linux":
        rename = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
        if rename:
            # Some filesystems supported by Setup (including exFAT) do not support hard links.
            if rename(-100, os.fsencode(temp), -100, os.fsencode(target), 1) == 0:  # AT_FDCWD, RENAME_NOREPLACE
                return
            failure = ctypes.get_errno()
            if failure not in (errno.ENOSYS, errno.EOPNOTSUPP, errno.EINVAL):
                raise OSError(failure, os.strerror(failure), target)
    os.link(temp, target)


def export(game, destination, now=None, on_progress=None, on_log=None):
    game = game.resolve(strict=True)
    cache = game / "cache"
    now = now or datetime.now().astimezone()
    with ExitStack() as stack:
        stack.enter_context(cache_lock(cache / ".shader-dump.lock", exclusive=True))
        selected = active_cache(cache)
        restored = selected != cache
        cache = selected
        legacy = not (cache / SOURCE).is_file()
        if legacy:
            if on_log:
                on_log("No Setup export record. Checking the existing preparation stamp and game logs.")
            record = recover_preparation(game, stack)
            if on_log:
                on_log(f"Matched the existing cache to {record['driver']['info']} ({record['driver']['pipeline_cache_uuid']}).")
        else:
            record = json.loads((cache / SOURCE).read_text(encoding="utf-8"))
            if restored and record["spirv_store_path"]:
                record["spirv_store_path"] = str(cache / "spirv")
        version = record["driver"]["mesa_version"]
        if not re.fullmatch(MESA_VERSION, version):
            raise DumpError("Shader preparation recorded an invalid Mesa version.")
        payload = sources(game, cache, record, stack, seed_checked=legacy)
        before = {path: file_identity(path) for path, _ in payload}
        total = sum(identity[2] for identity in before.values())
        if shutil.disk_usage(destination).free < total:
            raise DumpError(f"Not enough space for the shader dump in {destination}.")
        stem = f"mesa-{version}-{now:%d%m%y_%H%M%S}"
        temp_file = tempfile.NamedTemporaryFile(dir=destination, prefix=".longshot-shaders-", suffix=".part", delete=False)
        temp = Path(temp_file.name)
        stack.callback(temp.unlink, missing_ok=True)
        stack.enter_context(temp_file)
        with zipfile.ZipFile(temp_file, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
            files, done, last_pct = [], 0, -1
            for path, name in payload:
                digest, size = hashlib.sha256(), 0
                with path.open("rb") as source, archive.open(name, "w", force_zip64=True) as entry:
                    for chunk in iter(lambda: source.read(CHUNK), b""):
                        entry.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
                        done += len(chunk)
                        percent = done * 100 // total
                        if on_progress and percent != last_pct:
                            on_progress(percent)
                            last_pct = percent
                files.append({"path": name, "size": size, "sha256": digest.hexdigest()})
            manifest = dict(record, format="longshot-shader-dump", schema_version=1,
                            created_at=now.isoformat(timespec="seconds"), files=files)
            archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        temp_file.close()
        if any(file_identity(path) != identity for path, identity in before.items()):
            raise DumpError("The shader cache changed during export. Close the game and try again.")
        for index in range(256):
            suffix = f"_{index}" if index else ""
            target = destination / f"{stem}{suffix}.zip"
            try:
                publish(temp, target)
            except FileExistsError:
                continue
            return target
        raise DumpError("All 256 names for this timestamp are taken. Try dumping again.")


def file_identity(path):
    info = path.stat()
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def active_cache(cache):
    try:
        name = (cache / ACTIVE).read_text(encoding="ascii").strip()
    except FileNotFoundError:
        return cache
    if not re.fullmatch(r"shader-dump-[0-9a-f]{32}", name) or not (cache / name / RUNTIME).is_file():
        raise DumpError("The active shader dump selection is invalid.")
    return cache / name


def member_target(name):
    parts = name.split("/")
    if len(parts) == 2 and parts[0] == "longshot":
        leaf = parts[1]
        if leaf in (*OPTIONAL, "longshot_prepared.bin", "longshot_pipelines.bin",
                    *(file for files in PROGRAMS.values() for file in files)) or re.fullmatch(
                        r"longshot_vk_pipelines_play_[0-9]+\.bin", leaf):
            return leaf
    if parts in (["spirv", "spirv.pack"], ["spirv", "spirv.idx"]):
        return name
    if (len(parts) == 5 and parts[:2] == ["driver", LAYOUT] and re.fullmatch(r"[0-9a-f]{32}(?:[0-9a-f]{32})?", parts[2])
            and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 ._()+-]{0,127}", parts[3])
            and parts[4] in {"foz_cache.foz", "foz_cache_idx.foz", "index"}):
        return name
    raise DumpError(f"Unexpected file in shader dump: {name}.")


def read_dump(archive):
    entries = archive.infolist()
    files = {entry.filename: entry for entry in entries}
    if len(files) != len(entries) or "manifest.json" not in files or len(files) > 4096:
        raise DumpError("The shader dump has duplicate entries, too many entries, or no manifest.")
    for entry in entries:
        if (entry.orig_filename != entry.filename or entry.flag_bits & 1 or entry.compress_type != zipfile.ZIP_STORED or entry.is_dir()
                or stat.S_IFMT(entry.external_attr >> 16) not in (0, stat.S_IFREG)):
            raise DumpError(f"Unsupported archive entry: {entry.filename}.")
    if files.pop("manifest.json").file_size > 1 << 20:
        raise DumpError("The shader dump manifest is too large.")
    manifest = json.loads(archive.read("manifest.json"))
    if manifest["format"] != "longshot-shader-dump" or manifest["schema_version"] != 1:
        raise DumpError("Unsupported shader dump format.")
    settings = manifest["shader_settings"]
    if not isinstance(settings, dict) or ("robust_buffers" in settings and type(settings["robust_buffers"]) is not bool):
        raise DumpError("The shader dump has invalid Robust buffers metadata.")
    driver, gpu, program = (manifest[key] for key in ("driver", "gpu", "program_cache"))
    if (driver["id"] != 18 or driver["cache_layout"] != LAYOUT
            or not re.fullmatch(r"[0-9a-f]{32}", driver["pipeline_cache_uuid"])
            or not re.fullmatch(MESA_VERSION, driver["mesa_version"])
            or not re.fullmatch(r"[0-9a-f]{16}", manifest["renderer_shader_cache_stamp"])
            or not re.fullmatch(r"[0-9a-f]{64}", manifest["shader_seed_sha256"])):
        raise DumpError("The shader dump has invalid Turnip compatibility data.")
    for value in (driver["version_id"], gpu["vendor_id"], gpu["device_id"], program["store_id"]):
        if type(value) is not int or not 0 <= value <= 0xffffffff:
            raise DumpError("The shader dump has invalid numeric compatibility data.")
    cache_format = program["format"].encode("ascii")
    if cache_format not in PROGRAMS:
        raise DumpError("The shader dump uses an unsupported program cache format.")
    inventory = {}
    for item in manifest["files"]:
        name = item["path"]
        member_target(name)
        if (name in inventory or name not in files or type(item["size"]) is not int
                or item["size"] != files[name].file_size or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])):
            raise DumpError(f"Invalid inventory entry: {name}.")
        inventory[name] = item
    required = {f"longshot/{name}" for name in (*PROGRAMS[cache_format], "longshot_prepared.bin", "longshot_pipelines.bin")}
    if program["store_id"]:
        if cache_format != b"HSP1":
            raise DumpError("This program cache format cannot use an external SPIR-V store.")
        required.update(("spirv/spirv.pack", "spirv/spirv.idx"))
    elif any(name.startswith("spirv/") for name in inventory):
        raise DumpError("The shader dump contains an unused SPIR-V store.")
    databases = [name.rsplit("/", 1)[0] for name in files if name.endswith("/foz_cache.foz")]
    for folder in databases:
        required.update(f"{folder}/{name}" for name in ("foz_cache.foz", "foz_cache_idx.foz", "index"))
    if inventory.keys() != files.keys() or not databases or not required <= files.keys():
        raise DumpError("The shader dump is missing required cache files or contains unlisted entries.")
    return manifest, inventory


def inspect_dump(dump):
    with zipfile.ZipFile(dump) as archive:
        manifest, _ = read_dump(archive)
        return manifest


def supports_restore(executable):
    magic = RUNTIME_MAGIC.encode("ascii")
    tail = b""
    with executable.open("rb") as source:
        for chunk in iter(lambda: source.read(CHUNK), b""):
            if magic in tail + chunk:
                return True
            tail = chunk[-len(magic):]
    return False


def restore(game, dump, *, on_progress=None, already_locked=False, built_by_setup=False):
    game = game.resolve(strict=True)
    cache = game / "cache"
    cache.mkdir(exist_ok=True)
    with ExitStack() as stack:
        if not already_locked:
            stack.enter_context(cache_lock(cache / ".shader-dump.lock", exclusive=True))
        archive = stack.enter_context(zipfile.ZipFile(dump))
        manifest, inventory = read_dump(archive)
        # The Builder knows the newly installed Linux runtime was built with this restore protocol.
        # Only restore-only mode has to inspect a previously installed executable.
        if not built_by_setup and not supports_restore(game / "bin/longshot"):
            raise DumpError("The Longshot executable in the selected game folder cannot load a restored shader cache. "
                            "Keep this ZIP selected and build the game with a Setup that supports restore.")
        if shader_seed_hash(game) != manifest["shader_seed_sha256"]:
            raise DumpError("This shader dump was made from a different game shader seed.")
        total = sum(item["size"] for item in inventory.values())
        if shutil.disk_usage(cache).free < total:
            raise DumpError("Not enough free space to restore the shader dump.")
        generation = cache / ("shader-dump-" + secrets.token_hex(16))
        generation.mkdir(mode=0o700)
        try:
            done, last_pct = 0, -1
            for name, item in inventory.items():
                target = generation / member_target(name)
                target.parent.mkdir(parents=True, exist_ok=True)
                digest, size = hashlib.sha256(), 0
                with archive.open(name) as source, target.open("xb") as output:
                    for chunk in iter(lambda: source.read(CHUNK), b""):
                        size += len(chunk)
                        if size > item["size"]:
                            raise DumpError(f"The shader dump entry exceeds its declared size: {name}.")
                        output.write(chunk)
                        digest.update(chunk)
                        done += len(chunk)
                        pct = done * 100 // total
                        if on_progress and pct != last_pct:
                            on_progress(pct)
                            last_pct = pct
                if size != item["size"] or digest.hexdigest() != item["sha256"]:
                    raise DumpError(f"The shader dump entry failed validation: {name}.")
            record = {key: manifest[key] for key in ("longshot_version", "shader_seed_sha256", "renderer_shader_cache_stamp",
                                                      "gpu", "driver", "shader_settings")}
            program = manifest["program_cache"]
            record.update(program_cache_format=program["format"],
                          spirv_store_path=str(generation / "spirv") if program["store_id"] else "")
            # Reuse export's binary header checks before admitting a generation into use.
            with ExitStack() as checks:
                sources(game, generation, record.copy(), checks, seed_checked=True)
            program_file, _ = PROGRAMS[program["format"].encode("ascii")]
            if read_header(generation / program_file, "<4sIQ")[1] != program["store_id"]:
                raise DumpError("The shader dump manifest names a different SPIR-V store.")
            (generation / SOURCE).write_text(json.dumps(record), encoding="utf-8")
            driver, gpu = manifest["driver"], manifest["gpu"]
            robust = manifest["shader_settings"].get("robust_buffers")
            robust_line = "" if robust is None else "on\n" if robust else "off\n"
            (generation / RUNTIME).write_text(
                f"{RUNTIME_MAGIC}\n{gpu['vendor_id']:08x} {gpu['device_id']:08x} {driver['id']:08x} {driver['version_id']:08x}\n"
                f"{driver['pipeline_cache_uuid']} {manifest['renderer_shader_cache_stamp']} {program['format']} "
                f"{program['store_id']:08x} {driver['mesa_version']}\n{robust_line}", encoding="ascii")
            with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", dir=cache, prefix=".shader-active-", delete=False) as f:
                pointer = Path(f.name)
                try:
                    f.write(generation.name + "\n")
                    f.close()
                    os.replace(pointer, cache / ACTIVE)
                finally:
                    pointer.unlink(missing_ok=True)
        finally:
            # An interrupt immediately after activation must not remove the selected generation.
            if not (cache / ACTIVE).is_file() or (cache / ACTIVE).read_bytes() != (generation.name + "\n").encode("ascii"):
                shutil.rmtree(generation)
        return manifest["driver"]["mesa_version"]


def cli():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("destination", type=Path, nargs="?")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--restore", action="store_true")
    action.add_argument("--inspect", action="store_true")
    args = parser.parse_args()

    def interrupted(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    def progress(n):
        print(f"@@progress {n / 100}", flush=True)

    try:
        if args.inspect:
            target = inspect_dump(args.game)["driver"]["mesa_version"]
        elif args.destination is None:
            parser.error("a destination or shader dump ZIP is required")
        elif args.restore:
            target = restore(args.game, args.destination, on_progress=progress)
        else:
            target = export(args.game, args.destination, on_progress=progress, on_log=lambda line: print(line, flush=True))
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"@@error {error}", flush=True)
        return 1
    print(f"@@done {target}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(cli())
