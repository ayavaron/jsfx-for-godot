"""Package built binaries as a drop-in Godot addon zip.

    python tools/package_release.py 0.1.0

Expects the binaries produced by the windows-godot-debug/-release and
web-godot-release[-threads] presets under demo/bin/. Platforms whose binaries
are missing are left out of the .gdextension file. Output:
dist/jsfx_for_godot-<version>.zip containing addons/jsfx_for_godot/.
"""

import argparse
import pathlib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "demo" / "bin"
ADDON = "addons/jsfx_for_godot"

# (gdextension feature key, binary under demo/bin). Web debug exports use the
# release Wasm: the debug Wasm is large and adds nothing for game projects.
LIBRARIES = [
    ("windows.debug.x86_64", "windows-debug/jsfx_for_godot.dll"),
    ("windows.release.x86_64", "windows-release/jsfx_for_godot.dll"),
    ("linux.debug.x86_64", "linux-debug/libjsfx_for_godot.so"),
    ("linux.release.x86_64", "linux-release/libjsfx_for_godot.so"),
    ("macos.debug", "macos-debug/libjsfx_for_godot.dylib"),
    ("macos.release", "macos-release/libjsfx_for_godot.dylib"),
    ("web.debug.wasm32", "web-release/jsfx_for_godot.wasm"),
    ("web.release.wasm32", "web-release/jsfx_for_godot.wasm"),
    ("web.debug.threads.wasm32", "web-release-threads/jsfx_for_godot.wasm"),
    ("web.release.threads.wasm32", "web-release-threads/jsfx_for_godot.wasm"),
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    args = parser.parse_args()

    lines = [
        "[configuration]",
        "",
        'entry_symbol = "jsfx_for_godot_init"',
        'compatibility_minimum = "4.3"',
        "",
        "[libraries]",
        "",
    ]
    files = {}
    for feature, relative in LIBRARIES:
        source = BIN / relative
        if not source.is_file():
            print(f"skip {feature}: {relative} not built")
            continue
        lines.append(f'{feature} = "res://{ADDON}/bin/{relative}"')
        files[f"{ADDON}/bin/{relative}"] = source

    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    out = dist / f"jsfx_for_godot-{args.version}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"{ADDON}/jsfx_for_godot.gdextension", "\n".join(lines) + "\n")
        for name, source in sorted(files.items()):
            archive.write(source, name)
        archive.write(ROOT / "LICENSE", f"{ADDON}/LICENSE")
        archive.write(ROOT / "third_party" / "wdl" / "VENDORED.md",
                      f"{ADDON}/THIRD_PARTY_WDL.md")
    print(f"wrote {out} ({out.stat().st_size // 1024} KiB, {len(files)} binaries)")


if __name__ == "__main__":
    main()
