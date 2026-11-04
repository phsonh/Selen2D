#!/usr/bin/env python3
"""Selen2D Tools: one public entry for the desktop UI and automation."""
import argparse
import os
from pathlib import Path
import sys

TOOL_DIRECTORY = Path(__file__).resolve().parent
PROJECT_DIRECTORY = TOOL_DIRECTORY.parents[1]

def gui_python():
    return PROJECT_DIRECTORY / ('build/tools-venv/Scripts/python.exe' if os.name == 'nt'
                                else 'build/tools-venv/bin/python')

def dependency_help(requirement, error):
    dependency = TOOL_DIRECTORY / requirement
    interpreter = gui_python()
    def quoted(path):
        return "'" + str(path).replace("'", "''") + "'"
    print('Selen2D Tools 缺少开发依赖：' + str(error) + '\n'
          '依赖清单：' + str(dependency) + '\n'
          '请在 PowerShell 执行（路径与当前工作目录无关）：\n'
          'python -m venv ' + quoted(PROJECT_DIRECTORY / 'build/tools-venv') + '\n'
          '& ' + quoted(interpreter) + ' -m pip install -r ' + quoted(dependency) + '\n'
          '& ' + quoted(interpreter) + ' -B ' + quoted(TOOL_DIRECTORY / 'main.py'), file=sys.stderr)

def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__,
        epilog='No arguments: open the PyQt6 GUI.\n'
               'launch build|inspect|verify: validate/generate compiled launch data.\n'
               'pack|inspect|verify: create, inspect or verify SLPK resources.\n'
               'Use <command> --help for command-specific options.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', nargs='?', choices=['launch', 'pack', 'inspect', 'verify'])
    # Help is intentionally standard-library-only, including on build machines
    # without any Qt, compression, crypto or Unicode tool dependencies.
    if args and args[0] in ('-h', '--help'):
        parser.print_help()
        return 0
    if not args:
        interpreter = gui_python()
        if interpreter.is_file() and Path(sys.executable).resolve() != interpreter.resolve():
            # Explicitly installed repository environment, no network or global
            # pip mutation. Works equally from repo root or tools/pack.
            try:
                os.execv(str(interpreter), [str(interpreter), '-B', str(TOOL_DIRECTORY / 'main.py')])
            except OSError as error:
                dependency_help('requirements-gui.txt', error)
                return 1
    try:
        if not args:
            from selen2d_pytools.tools_gui import run
            return run([sys.argv[0]])
        if args[0] == 'launch':
            from selen2d_pytools.make_launch import main as launch
            return launch(args[1:])
        if args[0] in ('pack', 'inspect', 'verify'):
            from selen2d_pytools.pack_resources import main as package
            return package(args)
        parser.error('unknown command; run main.py --help')
    except (ImportError, RuntimeError) as error:
        requirement = 'requirements-gui.txt' if not args else 'requirements-launch.txt' if args[0] == 'launch' else 'requirements-tools.txt'
        dependency_help(requirement, error)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
