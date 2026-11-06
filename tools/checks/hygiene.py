"""Check tracked first-party inputs; never walk private/ignored workspaces."""
from __future__ import annotations

import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SELF = Path(__file__).resolve().relative_to(ROOT).as_posix()
FORBIDDEN_ROOTS = {"build", "tests", "devs", "releases", "launch"}
FORBIDDEN_EXTENSIONS = {".dll", ".exe", ".pdb", ".lib", ".obj", ".log", ".pyc", ".key", ".pem"}
TEXT_EXTENSIONS = {".cpp", ".h", ".hpp", ".in", ".cmake", ".ps1", ".py", ".lua", ".md", ".txt", ".yml", ".yaml", ".json", ".hlsl", ".hlsli"}
STALE = re.compile(r"solongo|二号机|DxcStatic|EngineDxcStatic|ENGINE_DXC_STATIC|dxc-static\.lib|single[- ](?:exe|executable)|no runtime dll|无运行时\s*DLL|单\s*EXE|statically embedded DXC|static DXC|DXC (?:is |uses |remains )?static|静态\s*DXC|DXC\s*静态", re.IGNORECASE)
# Migration history may name retired interfaces/implementations. This exemption
# applies only to these reviewed, older sections in the central changelog, never
# to its current development section, secrets, artifacts, or other documents.
HISTORY_VERSIONS = {"2.0.0", "2.1.0", "2.1.1", "2.2.0", "2.2.1", "2.2.2"}


def main() -> int:
    files = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode("utf-8").split("\0")
    failures: list[str] = []
    scanned = 0
    for name in filter(None, files):
        path = PurePosixPath(name)
        # Vendored sources and their licenses are not rewritten by project hygiene.
        own_integration = name == "3rd_party/CMakeLists.txt" or name.startswith("3rd_party/cmake/") or name == "3rd_party/dxc/RUNTIME-INTEGRATION.md"
        if path.parts[0] == "3rd_party" and not own_integration:
            continue
        if path.parts[0] in FORBIDDEN_ROOTS or path.suffix.lower() in FORBIDDEN_EXTENSIONS or path.name == "launch.h" or path.name.startswith(".env"):
            failures.append(f"{name}: private/generated artifact is tracked")
            continue
        source = ROOT / name
        if not source.is_file():
            continue  # A staged deletion is not a current source input.
        if name == SELF or path.suffix.lower() not in TEXT_EXTENSIONS:
            continue
        try:
            content = source.read_text(encoding="utf-8-sig")
        except UnicodeError:
            failures.append(f"{name}: expected UTF-8 text")
            continue
        scanned += 1
        history_section = False
        for number, line in enumerate(content.splitlines(), 1):
            if line.startswith("## "):
                version = re.match(r"## (\d+\.\d+\.\d+)\s+[—-]\s+", line)
                history_section = name == "CHANGELOG.md" and bool(version and version[1] in HISTORY_VERSIONS)
            if STALE.search(line) and not history_section:
                failures.append(f"{name}:{number}: retired terminology/integration")
        if path.suffix == ".json":
            try:
                value = json.loads(content)
            except ValueError:
                failures.append(f"{name}: invalid JSON")
                continue
            def secrets(item: object) -> bool:
                if isinstance(item, dict):
                    return any((key.lower() == "secret" and bool(value)) or secrets(value) for key, value in item.items())
                return isinstance(item, list) and any(secrets(value) for value in item)
            if secrets(value):
                # Never echo the matched value into build logs.
                failures.append(f"{name}: nonempty secret in tracked JSON")
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"hygiene: {scanned} tracked first-party text inputs; {len(failures)} failures")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
