"""Shared build-tool validation. Never used by the engine at runtime."""
from __future__ import annotations
import json
import math
from pathlib import Path
from functools import lru_cache

try:
    import unicodedata2 as unicode
except ImportError as exc:
    raise RuntimeError("Install tools/pack/requirements-launch.txt for JSON launch validation") from exc
if unicode.unidata_version != "18.0.0":
    raise RuntimeError("Build tools require Unicode 18.0.0; install tools/pack/requirements-launch.txt")


class ConfigError(ValueError):
    pass


def _pairs(items):
    result = {}
    for name, value in items:
        if name in result:
            # Do not echo config values: they may be secret material.
            raise ConfigError("duplicate JSON object key")
        result[name] = value
    return result


def read_json(path) -> dict:
    """Strict bounded JSON object. Diagnostics never quote source contents."""
    try:
        with Path(path).open("rb") as stream:
            raw = stream.read(8 * 1024 * 1024 + 1)
        if len(raw) > 8 * 1024 * 1024:
            raise ConfigError("configuration exceeds 8 MiB")
        text = raw.decode("utf-8", errors="strict")
        def invalid_number(_):
            raise ConfigError("non-finite JSON number")
        def finite_number(token):
            result = float(token)
            if not math.isfinite(result):
                raise ConfigError("non-finite JSON number")
            return result
        def integer_number(token):
            if len(token) > 1024:
                raise ConfigError("JSON integer exceeds 1024 digits")
            return int(token)
        value = json.loads(text, object_pairs_hook=_pairs, parse_constant=invalid_number,
                           parse_float=finite_number, parse_int=integer_number)
        if type(value) is not dict:
            raise ConfigError("configuration must be a JSON object")
        return value
    except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ConfigError("configuration is not valid bounded UTF-8 JSON") from None


def normalize_path(value: str, label: str = "path") -> str:
    if type(value) is not str or not value:
        raise ConfigError(f"{label} must be a nonempty UTF-8 relative path")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeError:
        raise ConfigError(f"{label} contains invalid Unicode") from None
    if len(encoded) > 4096 or any(c in value for c in "\\:*?") or any(unicode.category(c) == "Cc" for c in value):
        raise ConfigError(f"{label} has forbidden characters or exceeds 4096 bytes")
    if any(part in ("", ".", "..") for part in value.split("/")):
        raise ConfigError(f"{label} has invalid path components")
    normalized = unicode.normalize("NFC", value)
    if len(normalized.encode("utf-8")) > 4096:
        raise ConfigError(f"{label} exceeds 4096 bytes after NFC")
    return normalized


@lru_cache(maxsize=1)
def _fold_table():
    table = {}
    for line in (Path(__file__).parent / "unicode/CaseFolding-18.0.0.txt").read_text(encoding="utf-8").splitlines():
        fields = line.split("#", 1)[0].split(";")
        if len(fields) >= 3 and fields[1].strip() in ("C", "F"):
            table[int(fields[0], 16)] = "".join(chr(int(n, 16)) for n in fields[2].split())
    return table


def fold_path(value: str) -> str:
    return unicode.normalize("NFC", unicode.normalize("NFC", value).translate(_fold_table()))


def secret_bytes(config: dict, required: bool = False) -> bytes | None:
    if "secret" not in config:
        if required:
            raise ConfigError("encrypted packages require an explicitly supplied secret")
        return None
    if type(config["secret"]) is not str:
        raise ConfigError("secret must be a UTF-8 string")
    try:
        return config["secret"].encode("utf-8", errors="strict")
    except UnicodeError:
        raise ConfigError("secret is not valid UTF-8") from None


def _keys(value, expected, label):
    if type(value) is not dict or set(value) != set(expected):
        raise ConfigError(f"{label} must contain exactly the documented fields")


def _inside(parent, child):
    # Only validated deployment-root directory records may contain this marker.
    # normalize_path remains strict for entry, mounts and all ordinary paths.
    return parent == "." or child == parent or child.startswith(parent + "/")


def _tail(parent, child):
    if parent == child:
        return ""
    return child if parent == "." else child[len(parent) + 1:]


def _record_names(names, value):
    normalize_path(value)
    components = value.split("/")
    for count in range(1, len(components) + 1):
        raw = "/".join(components[:count])
        key = fold_path(raw)
        if key in names and names[key] != raw:
            raise ConfigError("mapping names have an NFC or Unicode case collision")
        names[key] = raw


def validate_launch(config: dict) -> dict:
    if type(config) is not dict or set(config) - {"secret", "mounts", "directories", "entry"} or not {"mounts", "directories", "entry"} <= set(config):
        raise ConfigError("launch requires mounts, directories, entry and optional secret only")
    if type(config["mounts"]) is not list or len(config["mounts"]) > 4096:
        raise ConfigError("mounts must be an array of at most 4096 records")
    if type(config["directories"]) is not list or len(config["directories"]) > 4096:
        raise ConfigError("directories must be an array of at most 4096 records")
    mounts, parts = [], set()
    virtual_names, deployment_names, user_names = {}, {}, {}
    for record in config["mounts"]:
        _keys(record, ("source", "target", "parts", "encrypted"), "mount")
        part = record["parts"]
        if type(part) is not int or not 0 <= part <= 0xffffffff or part in parts:
            raise ConfigError("parts must be globally unique uint32 integers")
        if type(record["encrypted"]) is not bool:
            raise ConfigError("encrypted must be a boolean")
        _record_names(virtual_names, record["target"])
        _record_names(deployment_names, record["source"])
        parts.add(part)
        mounts.append(dict(source=normalize_path(record["source"], "source"), target=normalize_path(record["target"], "target"), parts=part, encrypted=record["encrypted"]))
    mounts.sort(key=lambda m: m["parts"])
    directories = []
    for record in config["directories"]:
        if type(record) is str:
            record = dict(base="deployment", path=record, target=record)
        _keys(record, ("base", "path", "target"), "directory")
        if record["base"] not in ("deployment", "user_data"):
            raise ConfigError("directory base must be deployment or user_data")
        root_mapping = record == dict(base="deployment", path=".", target=".")
        if root_mapping:
            current = dict(record)
        else:
            _record_names(virtual_names, record["target"])
            _record_names(deployment_names if record["base"] == "deployment" else user_names, record["path"])
            current = dict(base=record["base"], path=normalize_path(record["path"], "directory path"), target=normalize_path(record["target"], "directory target"))
        if current not in directories:
            directories.append(current)
    # Parent-child rules may only repeat the same physical-to-virtual suffix.
    for i, first in enumerate(directories):
        for second in directories[i + 1:]:
            for a, b in ((first, second), (second, first)):
                virtual_overlap = _inside(a["target"], b["target"])
                physical_overlap = a["base"] == b["base"] and _inside(a["path"], b["path"])
                if virtual_overlap or physical_overlap:
                    equivalent = virtual_overlap and physical_overlap and _tail(a["target"], b["target"]) == _tail(a["path"], b["path"])
                    if not equivalent:
                        raise ConfigError("directory mappings overlap or alias different sources")
    entry = normalize_path(config["entry"], "entry")
    if not any(_inside(root, entry) for root in [m["target"] for m in mounts] + [d["target"] for d in directories]):
        raise ConfigError("entry is outside the declared virtual ranges")
    protected = any(m["encrypted"] for m in mounts)
    secret_bytes(config, required=protected)
    if protected and not any(m["encrypted"] and _inside(m["target"], entry) for m in mounts):
        raise ConfigError("protected entry must belong to an encrypted package target")
    result = dict(mounts=mounts, directories=directories, entry=entry)
    if "secret" in config:
        result["secret"] = config["secret"]
    return result


def load_launch_config(path) -> dict:
    return validate_launch(read_json(path))
