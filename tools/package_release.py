#!/usr/bin/env python3
"""Package every codec combination and its matching debug symbols."""
import argparse
import hashlib
from itertools import combinations
from pathlib import Path
import shutil
import zipfile


CODECS = (("h264", "H.264"), ("mpeg4", "MPEG-4 Part 2"),
          ("hevc", "H.265 / HEVC"), ("av1", "AV1"))
VARIANTS = (("", ", ".join(label for _, label in CODECS)),) + tuple(
    ("-" + "-".join(name for name, _ in group), ", ".join(label for _, label in group))
    for count in range(1, len(CODECS)) for group in combinations(CODECS, count)
)


def module_symbols(players: list[Path], dist: Path) -> list[Path]:
    """Verify bundled payloads against the exact images kept for debugging."""
    from bundle_codec_modules import CODECS as IDS, ENTRY, FOOTER, MAGIC

    required = set()
    names = {value: name for name, value in IDS.items()}
    for player in players:
        with player.open("rb") as stream:
            stream.seek(-FOOTER.size, 2)
            footer = stream.read(FOOTER.size)
            if footer[:8] != MAGIC:
                continue  # Single-codec and explicitly static builds.
            _, version, count, offset, size, total, reserved, digest = FOOTER.unpack(footer)
            if (version != 1 or not 0 < count <= len(IDS) or reserved or
                    total != player.stat().st_size or size != count * ENTRY.size or
                    offset + size + FOOTER.size != total):
                raise ValueError(f"Invalid codec bundle: {player}")
            stream.seek(offset)
            index = stream.read(size)
            if hashlib.sha256(index).digest() != digest:
                raise ValueError(f"Corrupt codec directory: {player}")
            seen = set()
            for i in range(count):
                codec, abi, start, length, _, _, flags, reserved, digest = ENTRY.unpack_from(index, i * ENTRY.size)
                if (codec not in names or codec in seen or abi != 1 or flags or reserved or
                        start > offset or length > offset - start):
                    raise ValueError(f"Invalid codec entry: {player}")
                seen.add(codec)
                image = dist / "modules" / (names[codec] + ".zehn")
                payload = image.read_bytes()
                stream.seek(start)
                if (len(payload) != length or hashlib.sha256(payload).digest() != digest or
                        stream.read(length) != payload):
                    raise ValueError(f"Codec symbols do not match {player}: {image}")
                required.add(names[codec])
    return [dist / "modules" / f"{codec}.{ext}"
            for codec in sorted(required) for ext in ("elf", "zehn")]


def package(dist: Path, output: Path, notes: Path | None = None) -> None:
    root = Path(__file__).resolve().parent.parent
    players = [dist / f"_ndvideo{suffix}.tns" for suffix, _ in VARIANTS]
    symbols = [dist / f"ndvideo{suffix}.{ext}"
               for suffix, _ in VARIANTS for ext in ("elf", "zehn")]
    licenses = [root / "LICENSE", root / "THIRD_PARTY_NOTICES"]
    if dist.resolve() == output.resolve():
        raise ValueError("The release output must be separate from the build directory.")
    for path in players + symbols + licenses:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f"Missing or empty release input: {path}")
    modules = module_symbols(players, dist)
    for path in modules:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f"Missing or empty codec symbol input: {path}")

    output.mkdir(parents=True, exist_ok=True)
    for path in players + licenses:
        shutil.copy2(path, output / path.name)
    archive = output / "ndvideo-symbols.zip"
    temporary = archive.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in symbols:
            zf.write(path, path.name)
        for path in modules:
            zf.write(path, "modules/" + path.name)
    temporary.replace(archive)

    assets = [output / path.name for path in players + licenses] + [archive]
    checksums = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
                 for path in sorted(assets, key=lambda path: path.name)]
    (output / "SHA256SUMS.txt").write_text("".join(checksums), encoding="utf-8")
    rows = [f"| `{path.name}` | {codecs} | {path.stat().st_size / 1024:.1f} KiB |"
            for path, (_, codecs) in zip(players, VARIANTS)]
    if notes:
        notes.parent.mkdir(parents=True, exist_ok=True)
        notes.write_text(
            "Choose `_ndvideo.tns` for all codecs, or a smaller build for the formats needed.\n\n" +
            ("Each player is a single file. Multi-codec builds load the required decoder on demand, "
             "leaving more RAM for buffered frames.\n\n" if modules else "") +
            "| Player | Codecs | Size |\n| --- | --- | ---: |\n" + "\n".join(rows) +
            "\n\n`ndvideo-symbols.zip` contains the matching player and decoder ELF and Zehn files.\n"
            "`SHA256SUMS.txt` verifies the downloads. Shared license texts are included once.\n",
            encoding="utf-8",
        )
    print("\n".join(rows))
    print(f"Release files: {output.resolve()}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=Path("dist"))
    parser.add_argument("--output", type=Path, default=Path("release"))
    parser.add_argument("--notes", type=Path, help="Optional release-description file outside the assets")
    args = parser.parse_args()
    try:
        package(args.dist, args.output, args.notes)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
