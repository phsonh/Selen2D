"""Build-only archive validation. Fast unchanged path; never a runtime dependency."""
import hashlib
import json
from pathlib import Path
import sys
import time


def read(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
    temporary.replace(path)


def metadata(path):
    try:
        stat = path.stat()
        with path.open("rb") as stream:
            valid = stream.read(8) == b"!<arch>\n"
        return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
                "ctime_ns": stat.st_ctime_ns, "archive": valid}
    except FileNotFoundError:
        return {}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def main():
    mode, archive, state_file, stamp_file, *extra = sys.argv[1:]
    archive, state_file, stamp_file = map(Path, (archive, state_file, stamp_file))
    state = read(state_file)
    current = metadata(archive)
    if mode == "check":
        good = bool(current.get("archive") and state.get("sha256"))
        if good and current != state.get("metadata"):
            print("Static DXC archive changed; verifying SHA256", flush=True)
            good = digest(archive) == state["sha256"]
            if good:
                state["metadata"] = current
                write(state_file, state)
        if not good or not stamp_file.exists():
            # Without a trusted record, regenerate only the final archive from
            # pinned object libraries; do not bless an arbitrary old bundle.
            corrupt = bool(current)
            write(stamp_file, {"invalid": corrupt, "checked_ns": time.time_ns()})
        return
    if mode == "prepare":
        if read(stamp_file).get("invalid") and (not state.get("sha256") or current != state.get("metadata")):
            archive.unlink(missing_ok=True)
        print("reset" if state.get("context") not in (None, extra[0]) else "reuse")
        return
    if mode == "record":
        if not current.get("archive"):
            raise RuntimeError("DXC build did not produce a valid COFF archive")
        write(state_file, {"metadata": current, "sha256": digest(archive), "context": extra[0]})
        stamp_file.parent.mkdir(parents=True, exist_ok=True)
        stamp_file.touch()
        return
    raise ValueError("unknown archive validation operation")


if __name__ == "__main__":
    main()
