#!/usr/bin/env python3
"""Package the seven codec builds and their matching debug symbols."""
import argparse
import hashlib
from pathlib import Path
import shutil
import zipfile


VARIANTS = (
    ("", "H.264, MPEG-4, H.265 / HEVC"),
    ("-h264", "H.264"),
    ("-mpeg4", "MPEG-4 Part 2"),
    ("-hevc", "H.265 / HEVC"),
    ("-h264-mpeg4", "H.264, MPEG-4"),
    ("-h264-hevc", "H.264, H.265 / HEVC"),
    ("-mpeg4-hevc", "MPEG-4, H.265 / HEVC"),
)


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

    output.mkdir(parents=True, exist_ok=True)
    for path in players + licenses:
        shutil.copy2(path, output / path.name)
    archive = output / "ndvideo-symbols.zip"
    temporary = archive.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in symbols:
            zf.write(path, path.name)
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
            "Choose `_ndvideo.tns` for all codecs, or a smaller build for the formats needed.\n\n"
            "| Player | Codecs | Size |\n| --- | --- | ---: |\n" + "\n".join(rows) +
            "\n\n`ndvideo-symbols.zip` contains the matching ELF and Zehn files for every build.\n"
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
