#!/usr/bin/env python3
"""Build relocatable decoder images, shared by all multi-codec variants."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

CODECS = ("h264", "mpeg4", "hevc", "av1")
ENTRIES = {
    "h264": ["h264_module_entry.c"],
    "mpeg4": ["mpeg4_module_entry.c"],
    "hevc": ["hevc_module.cpp", "hevc_module_runtime.cpp"],
    "av1": ["av1_module.c"],
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matches_stamp(path: Path, signature: dict) -> bool:
    try:
        return json.loads(path.read_text()) == signature
    except (OSError, ValueError):
        return False


def write_stamp(path: Path, signature: dict) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(signature))
    temporary.replace(path)


@contextmanager
def build_lock(path: Path):
    # Recursive release builds can request the same module concurrently.
    # OS locks are released even if a compiler or build process is killed.
    with path.open("a+b") as handle:
        handle.seek(0, os.SEEK_END)
        if not handle.tell():
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            while True:
                try:
                    msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.1)
            try:
                yield
            finally:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def sources_for(root: Path, codec: str) -> list[str]:
    folder = {"h264": "h264bsd", "mpeg4": "xvid", "hevc": "hevc", "av1": "av1"}[codec]
    if codec == "mpeg4":
        make = (root / "Makefile").read_text()
        block = make.split("XVID_DECODER_SRCS =", 1)[1].split("\n\n", 1)[0]
        sources = re.findall(r"src/codecs/xvid/[^\s\\]+\.c", block)
        if not sources:
            raise ValueError("Xvid source list is missing from Makefile")
        sources.append("src/codecs/rgb565_arm.S")
    else:
        sources = sorted(p.relative_to(root).as_posix()
                         for p in (root / "src/codecs" / folder).rglob("*")
                         if p.suffix in (".c", ".cpp", ".S"))
    adapter = {"av1": "av1_decoder.c", "hevc": "hevc_decoder.cpp", "mpeg4": "mpeg4_xvid.c"}.get(codec)
    if adapter:
        sources.append("src/codecs/" + adapter)
    sources += ["src/codecs/modules/" + name for name in ["module_runtime.c", *ENTRIES[codec]]]
    return sources


def run(command: list[str], root: Path) -> None:
    result = subprocess.run(command, cwd=root, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(command[:2]) + " failed:\n" + result.stdout + result.stderr)


def build(root: Path, args, codec: str, header_hash: str) -> None:
    output = args.output_dir.resolve()
    work = output / codec
    work.mkdir(parents=True, exist_ok=True)
    with build_lock(work / ".build.lock"):
        flags = shlex.split(args.flags)
        flags = [f for f in flags if not f.startswith("-std=")]
        flags += ["-DNDVIDEO_BUILD_MODULE=1", f"-DNDVIDEO_BUILD_{codec.upper()}_MODULE=1"]
        flags += [f"-DNDVIDEO_WITH_{c.upper()}={int(c == codec)}" for c in CODECS]
        if args.sdk:
            flags += ["-isystem", str(args.sdk.resolve() / "include")]
        if codec == "av1":
            flags += ["-DDAV1D_STATIC", "-Isrc/codecs/av1/dav1d", "-Isrc/codecs/av1/dav1d/include"]
        if codec == "hevc":
            flags += ["-DLIBDE265_STATIC_BUILD", "-DHAVE_STDINT_H", "-DHAVE_ALLOCA_H", "-Isrc/codecs/hevc"]
        versions = {compiler: subprocess.check_output([compiler, "-dumpfullversion"], text=True).strip()
                    for compiler in (args.cc, args.cxx)}

        def compile_one(source: str):
            obj = work / (source.replace("/", "_") + ".o")
            cpp = source.endswith(".cpp")
            compiler = args.cxx if cpp else args.cc
            selected = flags + (["-std=c++11", "-fno-exceptions", "-fno-rtti"] if cpp else ["-std=c11"])
            if source.endswith(".S"):
                selected = ["-marm", "-mcpu=arm926ej-s"]
            if source.endswith("_tmpl.c") and codec == "av1":
                selected += ["-DBITDEPTH=8"]
            if source.endswith("module_runtime.c"):
                # Keep libc thunks externally visible to late LTO-generated
                # calls (notably memset), without recursive builtin folding.
                selected = [f for f in selected if f != "-flto"]
                selected += ["-fno-lto", "-fno-builtin"]
            if source.startswith("src/codecs/xvid/"):
                selected += ["-Wno-incompatible-pointer-types"]
            command = [compiler, *selected, "-c", source, "-o", str(obj)]
            signature = {"command": command, "source": digest(root / source),
                         "headers": header_hash, "compiler": versions[compiler]}
            stamp = obj.with_suffix(".json")
            if not (obj.exists() and matches_stamp(stamp, signature)):
                run(command, root)
                write_stamp(stamp, signature)
            return obj, signature

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            compiled = list(pool.map(compile_one, sources_for(root, codec)))
        elf = output / (codec + ".elf")
        zehn = output / (codec + ".zehn")
        script = root / "tools/codec_module.ld"
        link = [args.cxx if codec == "hevc" else args.cc, "-marm", "-mcpu=arm926ej-s", "-mfloat-abi=soft",
                "-O3", "-flto", "-nostartfiles", "-nodefaultlibs", "-Wl,--gc-sections,--emit-relocs,--pic-veneer",
                "-T", str(script), *[str(p) for p, _ in compiled], "-Wl,--start-group"]
        if codec == "hevc":
            link += ["-lstdc++"]
        link += ["-lm", "-lgcc", "-Wl,--end-group", "-o", str(elf)]
        signature = {"command": link, "objects": [s for _, s in compiled],
                     "script": digest(script), "packer": digest(root / "tools/pack_zehn.py")}
        stamp = work / "linked.json"
        if not (elf.exists() and zehn.exists() and matches_stamp(stamp, signature)):
            response = work / "link.rsp"
            response.write_text(" ".join('"' + item.replace("\\", "/") + '"' for item in link[1:]))
            run([link[0], "@" + str(response)], root)
            run([sys.executable, str(root / "tools/pack_zehn.py"), "--input", str(elf),
                 "--output", str(zehn), "--name", "ND Video " + codec, "--ndless-min", "45"], root)
            write_stamp(stamp, signature)
        print(f"{codec}: {zehn.stat().st_size} bytes", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codecs", nargs="+", choices=CODECS, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--cc", default="arm-none-eabi-gcc")
    parser.add_argument("--cxx", default="arm-none-eabi-g++")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--flags", required=True, help="Module compiler flags, passed as one argument")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    root = Path(__file__).resolve().parent.parent
    headers = hashlib.sha256()
    for p in sorted((root / "src").rglob("*.h")):
        headers.update(p.relative_to(root).as_posix().encode())
        headers.update(p.read_bytes())
    try:
        for codec in dict.fromkeys(args.codecs):
            build(root, args, codec, headers.hexdigest())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"build_codec_modules.py: {exc}\n")


if __name__ == "__main__":
    main()
