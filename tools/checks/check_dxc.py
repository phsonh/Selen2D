#!/usr/bin/env python3
"""Blocking Windows DXC checks; run from an activated MSVC developer shell.

Uses the built production checker, generates test-owned fault DLLs, and retains
every subprocess's raw output. Engine negative cases fail before SDL/GPU. The output
directory must not already exist; this script never removes evidence or caches.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_record(command: list[str], directory: Path, name: str, *, cwd: Path,
               env: dict[str, str] | None = None, timeout: int = 180) -> dict:
    started = time.time()
    timed_out = False
    launch_error = None
    try:
        # Supply lpApplicationName explicitly on Windows. With a null value,
        # CreateProcess limits the executable portion of the command line to
        # MAX_PATH even when that command line uses the extended path prefix.
        process = subprocess.run(command, executable=command[0], cwd=cwd, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=timeout, check=False)
        code, stdout, stderr = process.returncode, process.stdout, process.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = None, error.stdout or b"", error.stderr or b""
        timed_out = True
    except OSError as error:
        code, stdout, stderr = None, b"", str(error).encode("utf-8")
        launch_error = str(error)
    (directory / f"{name}.stdout.bin").write_bytes(stdout)
    (directory / f"{name}.stderr.bin").write_bytes(stderr)
    result = {"command": command, "cwd": str(cwd), "exit_code": code,
              "timed_out": timed_out, "launch_error": launch_error, "duration_seconds": time.time() - started,
              "path": env.get("PATH") if env else None}
    write_json(directory / f"{name}.process.json", result)
    return {**result, "stdout": stdout, "stderr": stderr}


def required_run(command: list[str], directory: Path, name: str) -> dict:
    result = run_record(command, directory, name, cwd=directory)
    if result["exit_code"] != 0:
        raise RuntimeError(f"{name} failed; see {directory / (name + '.stderr.bin')}")
    return result


def build_faults(directory: Path, dumpbin: Path) -> dict[str, Path]:
    directory.mkdir()
    compiler, linker = dumpbin.with_name("cl.exe"), dumpbin.with_name("link.exe")
    x86_compiler = compiler.parent.parent / "x86" / "cl.exe"
    for tool in (compiler, linker, x86_compiler):
        if not tool.is_file():
            raise RuntimeError(f"required MSVC tool missing: {tool}")
    source = directory / "fault.cpp"
    source.write_text(r'''#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
static wchar_t marker[32768];
extern "C" BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        const DWORD size=GetEnvironmentVariableW(L"ENGINE_DXC_CHECK_MARKER",marker,32768);
        if(size && size<32768) {
            HANDLE file=CreateFileW(marker,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,0,nullptr);
            if(file!=INVALID_HANDLE_VALUE) {DWORD written=0;WriteFile(file,"loaded",6,&written,nullptr);CloseHandle(file);}
        }
    }
    return TRUE;
}
#ifdef EXPORT_FACTORY
extern "C" __declspec(dllexport) HRESULT WINAPI DxcCreateInstance(const void*,const void*,void**) {return E_FAIL;}
#endif
''', encoding="utf-8")
    wrong_source = directory / "wrong_arch.cpp"
    wrong_source.write_text('extern "C" __declspec(dllexport) long __stdcall DxcCreateInstance(const void*,const void*,void**){return -2147467259L;}\n', encoding="utf-8")
    outputs: dict[str, Path] = {}
    for name, define in (("no-export", []), ("decoy", ["/DEXPORT_FACTORY"])):
        obj, dll = directory / f"{name}.obj", directory / f"{name}.dll"
        required_run([str(compiler), "/nologo", "/c", "/O2", "/GS-", "/Zl", "/W4", *define,
                      f"/Fo{obj}", str(source)], directory, f"compile-{name}")
        required_run([str(linker), "/nologo", "/DLL", "/NODEFAULTLIB", "/ENTRY:DllMain", "/MACHINE:X64",
                      f"/OUT:{dll}", f"/IMPLIB:{directory / (name + '.lib')}", str(obj), "kernel32.lib"], directory, f"link-{name}")
        outputs[name] = dll
    obj, dll = directory / "wrong-arch.obj", directory / "wrong-arch.dll"
    required_run([str(x86_compiler), "/nologo", "/c", "/O2", "/GS-", "/Zl", "/W4", f"/Fo{obj}", str(wrong_source)], directory, "compile-wrong-arch")
    required_run([str(linker), "/nologo", "/DLL", "/NODEFAULTLIB", "/NOENTRY", "/MACHINE:X86",
                  f"/OUT:{dll}", f"/IMPLIB:{directory / 'wrong-arch.lib'}", str(obj)], directory, "link-wrong-arch")
    outputs["wrong-arch"] = dll
    for name, path in outputs.items():
        required_run([str(dumpbin), "/headers", "/exports", "/dependents", str(path)], directory, f"inspect-{name}")
    write_json(directory / "manifest.json", {name: {"path": str(path), "sha256": sha256(path)} for name, path in outputs.items()})
    return outputs


def check_imports(directory: Path, dumpbin: Path, binary: Path, name: str) -> dict:
    result = required_run([str(dumpbin), "/imports", str(binary)], directory, f"imports-{name}")
    text = result["stdout"].decode("utf-8", errors="replace").lower()
    imported = sorted(set(re.findall(r"(?im)^\s*([a-z0-9_.-]+\.dll)\s*$", text)))
    if not imported:
        raise RuntimeError(f"dumpbin reported no import DLLs for {binary}")
    forbidden = [item for item in imported if item in {"dxcompiler.dll", "dxil.dll", "libclang.dll", "ucrtbase.dll", "ucrtbased.dll", "msvcrt.dll"}
                 or item.startswith(("vcruntime", "msvcp140", "msvcr1", "api-ms-win-crt-", "llvm"))]
    if forbidden:
        raise RuntimeError(f"forbidden imports in {binary}: {forbidden}")
    return {"path": str(binary), "sha256": sha256(binary), "imports": imported}


def extended(path: Path) -> str:
    text = str(path)
    if text.startswith("\\\\?\\"):
        return text
    return "\\\\?\\UNC\\" + text[2:] if text.startswith("\\\\") else "\\\\?\\" + text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--dumpbin", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Windows x64 is required")
    binary_dir, output, dumpbin = args.bin.resolve(), args.output.absolute(), args.dumpbin.resolve()
    checker, dll = binary_dir / "dxc-runtime-check.exe", binary_dir / "dxcompiler.dll"
    for path in (checker, dll, dumpbin):
        if not path.is_file():
            parser.error(f"required input missing: {path}")
    if output.exists():
        parser.error("--output must be a new directory; existing evidence is preserved")
    output.mkdir(parents=True)
    results: list[dict] = []
    try:
        faults = build_faults(output / "faults", dumpbin)
        imports = [check_imports(output, dumpbin, checker, "checker")]
        product_path = binary_dir / "product-info.json"
        engine_name = "Selen2D.exe"
        if product_path.is_file():
            product = json.loads(product_path.read_text(encoding="utf-8-sig"))
            engine_name = product.get("runtimeExecutable", engine_name)
        if Path(engine_name).name != engine_name:
            raise RuntimeError("product runtimeExecutable must be a filename")
        engine = binary_dir / engine_name
        if not engine.is_file():
            raise RuntimeError(f"engine executable missing: {engine}")
        imports.append(check_imports(output, dumpbin, engine, "engine"))
        write_json(output / "inputs.json", {"checker": str(checker), "checker_sha256": sha256(checker),
                   "dll": str(dll), "dll_sha256": sha256(dll), "imports": imports})
        system32 = str(Path(os.environ["SystemRoot"]) / "System32")
        cases = [
            ("correct", "good", True, "--self-test"),
            ("missing", "missing", False, "--load-only"),
            ("renamed", "renamed", False, "--load-only"),
            ("corrupt", "corrupt", False, "--load-only"),
            ("wrong-arch", "wrong-arch", False, "--load-only"),
            ("no-export", "no-export", False, "--load-only"),
            ("cwd-decoy-positive", "good", True, "--load-only"),
            ("cwd-decoy-negative", "missing", False, "--load-only"),
            ("path-decoy-positive", "good", True, "--load-only"),
            ("path-decoy-negative", "missing", False, "--load-only"),
            ("unicode-valid", "good", True, "--load-only"),
            ("unicode-missing", "missing", False, "--load-only"),
            ("long-path", "good", True, "--load-only"),
            ("sticky-repair", "missing", False, "--sticky-repair"),
        ]
        for name, flavor, expect_success, mode in cases:
            case = output / name
            case.mkdir()
            deployment = case / ("部署-シェーダー-Ω" if name.startswith("unicode") else "deployment")
            if name == "long-path":
                while len(str(deployment)) < 330:
                    deployment /= "deployment-segment-0123456789-0123456789"
                # The Python host may lack longPathAware/registry opt-in. Use
                # the Win32 extended spelling for fixture I/O as well as launch;
                # otherwise mkdir/copy can fail before the loader is exercised.
                deployment = Path(extended(deployment))
            deployment.mkdir(parents=True)
            installed = deployment / checker.name
            shutil.copy2(checker, installed)
            local_dll = deployment / "dxcompiler.dll"
            if flavor == "good":
                shutil.copy2(dll, local_dll)
            elif flavor == "renamed":
                shutil.copy2(dll, deployment / "dxcompiler-renamed.dll")
            elif flavor == "corrupt":
                local_dll.write_bytes(b"This is a deliberately invalid test-owned PE image.\r\n")
            elif flavor in faults:
                shutil.copy2(faults[flavor], local_dll)
            foreign = case / "foreign-cwd"
            foreign.mkdir()
            decoy_dir = case / "path-decoy"
            decoy_dir.mkdir()
            marker = case / "fault-dll-loaded.txt"
            env = os.environ.copy()
            env["PATH"] = system32
            env["ENGINE_DXC_CHECK_MARKER"] = str(marker)
            for key in ("DXC_PATH", "LUA_PATH", "LUA_CPATH", "LUA_INIT", "PYTHONPATH"):
                env.pop(key, None)
            if name.startswith("cwd-decoy"):
                shutil.copy2(faults["decoy"], foreign / "dxcompiler.dll")
            if name.startswith("path-decoy"):
                shutil.copy2(faults["decoy"], decoy_dir / "dxcompiler.dll")
                env["PATH"] = str(decoy_dir) + os.pathsep + system32
            command = [extended(installed) if name == "long-path" else str(installed), mode]
            if mode == "--self-test":
                command.append(str(case / "compiler-fixture"))
            if mode == "--sticky-repair":
                replacement = case / "replacement.dll"
                shutil.copy2(dll, replacement)
                command.append(str(replacement))
            process = run_record(command, case, "checker", cwd=foreign, env=env)
            records = []
            errors = []
            try:
                stdout = process["stdout"].decode("utf-8", errors="strict")
                process["stderr"].decode("utf-8", errors="strict")
                records = [json.loads(line[len("DXC_CHECK "):]) for line in stdout.splitlines() if line.startswith("DXC_CHECK ")]
            except (UnicodeError, json.JSONDecodeError) as error:
                errors.append(f"invalid UTF-8/structured output: {error}")
            summaries = [record for record in records if record.get("kind") == "summary"]
            if process["timed_out"]:
                errors.append("process timed out")
            if len(summaries) != 1 or summaries[0].get("ok") is not expect_success:
                errors.append("summary does not match expected outcome")
            if process["exit_code"] != (0 if expect_success else 1):
                errors.append("unexpected process exit code")
            load_errors = [record for record in records if record.get("kind") == "loader_failure"]
            win32_error = None
            if not expect_success:
                if len(load_errors) != 1:
                    errors.append("expected one production loader failure record")
                else:
                    failure = load_errors[0]
                    message = failure.get("message", "")
                    code = re.search(r"Win32 error ([1-9][0-9]*)", message)
                    win32_error = int(code.group(1)) if code else None
                    if not code or str(local_dll) not in message:
                        errors.append("loader diagnostic lacks exact absolute UTF-8 path/nonzero Win32 error")
                    if not failure.get("sticky") or not failure.get("cache_unchanged"):
                        errors.append("loader failure was not sticky or polluted Lua history")
                    operation = "GetProcAddress(DxcCreateInstance)" if flavor == "no-export" else "LoadLibraryExW"
                    if operation not in message:
                        errors.append("wrong failing loader operation")
                    if name == "sticky-repair" and (not failure.get("repaired") or not local_dll.is_file() or sha256(local_dll) != sha256(dll)):
                        errors.append("sticky test did not install the actual correct DLL after failure")
            elif load_errors:
                errors.append("unexpected production loader failure")
            modules = [record for record in records if record.get("kind") == "modules"]
            if expect_success and (len(modules) < 2 or any(not r.get("found") or r.get("dxil") or r.get("forbidden") for r in modules)):
                errors.append("module evidence missing or violates runtime allowlist")
            if name.startswith(("cwd-decoy", "path-decoy")) and marker.exists():
                errors.append("foreign decoy DLL executed")
            if flavor == "no-export" and not marker.is_file():
                errors.append("no-export test DLL did not execute its marker entrypoint")
            if name == "correct" and not any(r.get("name") == "production-four-thread-repeated-compile" and r.get("ok") for r in records):
                errors.append("full production compiler checks did not finish")
            result = {"name": name, "ok": not errors, "expected_success": expect_success,
                      "exit_code": process["exit_code"], "win32_error": win32_error,
                      "deployment": str(deployment), "dll_path": str(local_dll), "errors": errors,
                      "records": records}
            write_json(case / "result.json", result)
            results.append(result)
            print(f"{'PASS' if not errors else 'FAIL'} {name}: {'; '.join(errors) if errors else 'expected outcome observed'}", flush=True)
        # These are actual production GUI-subsystem engine processes. Every
        # deployment has a valid public-default entry but a failing runtime DLL,
        # so a correct implementation never reaches SDL, Lua, jobs or GPU init.
        # Existing Engine::Init failures return 1; startup/Log failures return 2.
        for name, flavor in (("engine-missing", "missing"), ("engine-corrupt", "corrupt"),
                             ("engine-wrong-arch", "wrong-arch"), ("engine-no-export", "no-export"),
                             ("engine-unicode-missing", "missing")):
            case = output / name
            deployment = case / ("部署-シェーダー-Ω" if "unicode" in name else "deployment")
            deployment.mkdir(parents=True)
            installed, local_dll = deployment / engine.name, deployment / "dxcompiler.dll"
            shutil.copy2(engine, installed)
            (deployment / "main.lua").write_text("Debug.Log('UNEXPECTED_DXC_LUA_ENTRY')\n", encoding="utf-8")
            if flavor == "corrupt":
                local_dll.write_bytes(b"Deliberately invalid test-owned DXC image.\r\n")
            elif flavor in faults:
                shutil.copy2(faults[flavor], local_dll)
            foreign = case / "foreign-cwd"
            foreign.mkdir()
            env = os.environ.copy()
            env["PATH"] = system32
            env["ENGINE_DXC_CHECK_MARKER"] = str(case / "fault-dll-loaded.txt")
            process = run_record([str(installed), "--no-error-dialog", "--frames", "1"], case, "engine", cwd=foreign, env=env, timeout=30)
            errors = []
            win32_error = None
            try:
                stdout, stderr = process["stdout"].decode("utf-8", errors="strict"), process["stderr"].decode("utf-8", errors="strict")
                diagnostics = [line for line in stderr.splitlines() if "Engine initialization failed: DXC runtime " in line]
                if len(diagnostics) != 1:
                    errors.append("expected one native Engine initialization DXC failure")
                else:
                    diagnostic = diagnostics[0]
                    code = re.search(r"Win32 error ([1-9][0-9]*)", diagnostic)
                    win32_error = int(code.group(1)) if code else None
                    if not code or str(local_dll) not in diagnostic:
                        errors.append("native error lacks exact UTF-8 absolute DLL path/Win32 code")
                    operation = "GetProcAddress(DxcCreateInstance)" if flavor == "no-export" else "LoadLibraryExW"
                    if operation not in diagnostic:
                        errors.append("wrong native loader failure operation")
                for forbidden in ("UNEXPECTED_DXC_LUA_ENTRY", "SDL runtime initialized", "Lua runtime initialized", "Engine initialized"):
                    if forbidden in stdout + stderr:
                        errors.append(f"unexpected post-loader activity: {forbidden}")
                if "Startup error:" in stderr:
                    errors.append("engine failed during Startup instead of the production DXC initialization")
            except UnicodeError as error:
                errors.append(f"native diagnostic is not UTF-8: {error}")
            if process["exit_code"] != 1 or process["timed_out"]:
                errors.append("actual engine did not exit 1 promptly for Engine::Init failure")
            if (deployment / "engine.log").exists():
                errors.append("engine created an automatic log file")
            result = {"name": name, "ok": not errors, "expected_success": False,
                      "exit_code": process["exit_code"], "win32_error": win32_error,
                      "deployment": str(deployment), "dll_path": str(local_dll), "errors": errors,
                      "evidence_boundary": "actual production engine native early-init failure; Lua-history bytes verified separately by production checker"}
            write_json(case / "result.json", result)
            results.append(result)
            print(f"{'PASS' if not errors else 'FAIL'} {name}: {'; '.join(errors) if errors else 'actual Engine::Init exit 1 before SDL/Lua/GPU'}", flush=True)
        ok = all(result["ok"] for result in results)
        summary = {"ok": ok, "cases": len(results), "passed": sum(r["ok"] for r in results),
                   "failed": sum(not r["ok"] for r in results), "results": [{k: v for k, v in r.items() if k != "records"} for r in results]}
        write_json(output / "summary.json", summary)
        return 0 if ok else 1
    except Exception as error:
        write_json(output / "summary.json", {"ok": False, "fatal": str(error), "completed_cases": len(results)})
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
