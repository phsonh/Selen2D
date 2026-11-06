"""Validate an explicit Release ZIP, optionally run its existing GPU example.

Outputs always go to a new caller-selected directory; no repository cleanup and
no edits to a user's existing deployment. Uses only the Python standard library.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import time
import zipfile


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(archive: Path, manifest: Path, output: Path, smoke: bool) -> dict:
    output.mkdir(parents=True, exist_ok=False)
    deployment = output / "deployment"
    deployment.mkdir()
    expected = json.loads(manifest.read_text(encoding="utf-8"))
    directories = {parent.as_posix() for name in expected for parent in PurePosixPath(name).parents if parent != PurePosixPath(".")}
    with zipfile.ZipFile(archive) as package:
        files = {}
        all_names = set()
        for entry in package.infolist():
            path = PurePosixPath(entry.filename)
            canonical = entry.filename[:-1] if entry.is_dir() else entry.filename
            if (entry.orig_filename != entry.filename or
                    path.is_absolute() or not path.parts or ".." in path.parts or
                    "\\" in entry.filename or ":" in entry.filename or
                    path.as_posix() != canonical or
                    any(part.endswith((".", " ")) or re.search(r'[<>:"|?*\x00-\x1f]', part) or
                        re.fullmatch(r'(?:CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\..*)?', part, re.I)
                        for part in path.parts) or
                    stat.S_ISLNK(entry.external_attr >> 16)):
                raise ValueError(f"unsafe ZIP path: {entry.filename}")
            folded = entry.filename.rstrip("/").casefold()
            if folded in all_names:
                raise ValueError(f"duplicate ZIP path: {entry.filename}")
            all_names.add(folded)
            if entry.is_dir():
                if canonical not in directories:
                    raise ValueError(f"unexpected package directory: {canonical}")
                continue
            name = path.as_posix()
            if name not in {"Selen2D.exe", "dxcompiler.dll", "main.lua"} and path.parts[0] not in {"example", "doc"}:
                raise ValueError(f"unexpected package root: {name}")
            if name not in {"Selen2D.exe", "dxcompiler.dll"} and path.suffix.lower() in {".dll", ".exe", ".lib", ".pdb", ".obj", ".log", ".zip"}:
                raise ValueError(f"unexpected package artifact: {name}")
            if path.name.casefold() in {"launch.h", "launch.json", "product-info.json", "build-configuration.txt"}:
                raise ValueError(f"private/build material in package: {name}")
            data = package.read(entry)
            files[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            destination = deployment.joinpath(*path.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        if files != expected:
            raise ValueError("ZIP does not exactly match the staging file/hash manifest")
    required = {"Selen2D.exe", "dxcompiler.dll", "main.lua", "example/main.lua",
                "example/effect.hlsl", "example/palette.hlsl", "doc/guide/getting-started.md",
                "doc/api/modules/Graphic.lua", "doc/guide/licenses/dxc/LICENSE.TXT", "doc/guide/CHANGELOG.md"}
    missing = required - files.keys()
    if missing:
        raise ValueError(f"required package files missing: {sorted(missing)}")
    # Guide/changelog links must work offline after the changelog's relocation.
    # External references and in-document anchors do not name package files.
    guide_links = 0
    for document in (deployment / "doc/guide").glob("*.md"):
        for target in re.findall(r'\]\(([^\s)]+)\)', document.read_text(encoding="utf-8")):
            if ":" in target or target.startswith("#"):
                continue
            resolved = (document.parent / target.split("#", 1)[0]).resolve()
            if not resolved.is_relative_to(deployment.resolve()) or not resolved.exists():
                raise ValueError(f"broken package guide link: {document.name} -> {target}")
            guide_links += 1
    if 'File.RunLuaFile("example/main.lua")' not in (deployment / "main.lua").read_text(encoding="utf-8"):
        raise ValueError("default Lua shell no longer starts the published example")
    result = {"ok": True, "zip": str(archive), "sha256": digest(archive),
              "fileCount": len(files), "guideLinks": guide_links, "files": files, "smoke": "not requested"}
    if smoke:
        if os.name != "nt":
            raise RuntimeError("the GPU smoke currently supports Windows only")
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        get_system_directory = kernel32.GetSystemDirectoryW
        get_system_directory.argtypes = [ctypes.c_wchar_p, ctypes.c_uint]
        get_system_directory.restype = ctypes.c_uint
        system_directory = ctypes.create_unicode_buffer(32768)
        length = get_system_directory(system_directory, len(system_directory))
        if not 0 < length < len(system_directory):
            raise OSError(ctypes.get_last_error(), "GetSystemDirectoryW failed")
        foreign = output / "foreign-cwd"
        foreign.mkdir()
        environment = os.environ.copy()
        environment["PATH"] = system_directory.value
        runs = []
        for label, arguments in (("finite", ["--frames", "8"]), ("self-test", ["--self-test", "--frames", "9000"])):
            started = time.monotonic()
            with (output / f"{label}.stdout.txt").open("wb") as stdout, (output / f"{label}.stderr.txt").open("wb") as stderr:
                child = subprocess.run([str(deployment / "Selen2D.exe"), "--no-error-dialog", *arguments], cwd=foreign,
                                       env=environment, stdout=stdout, stderr=stderr, timeout=180)
            runs.append({"name": label, "exit": child.returncode, "seconds": time.monotonic() - started})
            if child.returncode != 0:
                raise RuntimeError(f"{label} failed with exit {child.returncode}; stdout/stderr retained")
            if (deployment / "engine.log").exists() or (foreign / "engine.log").exists():
                raise RuntimeError("implicit engine.log was created")
            text = (output / f"{label}.stdout.txt").read_text(encoding="utf-8", errors="strict")
            if "actual='direct3d12'" not in text or "Engine shutdown complete" not in text:
                raise RuntimeError(f"{label} did not confirm D3D12 and complete shutdown")
        outcome = json.loads((deployment / "output/result.json").read_text(encoding="utf-8"))
        if not outcome["completed"] or outcome["exitCalls"] != 1 or outcome["finalCount"] != 0:
            raise RuntimeError("Orbit Garden did not complete its lifecycle checks")
        for name in outcome["screenshots"]:
            if (deployment / "output" / f"{name}.png").read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                raise RuntimeError(f"missing/invalid screenshot {name}")
        result["smoke"] = {"runs": runs, "result": outcome, "path": environment["PATH"], "cwd": str(foreign)}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new evidence/deployment directory")
    parser.add_argument("--smoke", action="store_true", help="requires a working D3D12 GPU")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("--output must not already exist")
    try:
        result = check(args.zip.resolve(), args.manifest.resolve(), output, args.smoke)
    except Exception as error:
        result = {"ok": False, "error": str(error)}
    output.mkdir(parents=True, exist_ok=True)
    (output / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "files"}, ensure_ascii=False))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
