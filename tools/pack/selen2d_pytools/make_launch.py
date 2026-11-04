#!/usr/bin/env python3
"""Validate build-only launch declarations and emit inert C++ constant data."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
from .launch_common import ConfigError, load_launch_config, secret_bytes


def _text(value: str) -> str:
    raw = value.encode("utf-8")
    literal = "".join(f"\\x{byte:02x}" for byte in raw)
    return f'std::string_view{{"{literal}", {len(raw)}}}'


def render(config: dict) -> bytes:
    secret = secret_bytes(config)
    lines = ['#pragma once', '#include "engine/LaunchConfig.h"', '#include <array>', '',
             '// Generated build input; never load or distribute this as runtime configuration.',
             'namespace engine::launch_config {']
    if secret is not None:
        lines.append(f'inline constexpr std::array<std::uint8_t, {len(secret)}> secret{{' + ','.join(str(b) for b in secret) + '};')
    lines.append(f'inline constexpr std::array<launch::Mount, {len(config["mounts"])}> mounts{{{{')
    for mount in config['mounts']:
        lines.append(f'    {{{_text(mount["source"])}, {_text(mount["target"])}, {mount["parts"]}u, {str(mount["encrypted"]).lower()}}},')
    lines.append('}};')
    lines.append(f'inline constexpr std::array<launch::Directory, {len(config["directories"])}> directories{{{{')
    for directory in config['directories']:
        base = {'root': 'Root', 'local_app_data': 'LocalAppData'}[directory['base']]
        lines.append(f'    {{launch::Base::{base}, {_text(directory["path"])}, {_text(directory["target"])} }},')
    lines.append('}};')
    secret_ref = 'std::span<const std::uint8_t>{secret}' if secret is not None else 'std::nullopt'
    lines.append(f'inline constexpr launch::Config value{{mounts, directories, {_text(config["entry"])}, {secret_ref}}};')
    lines.append('}')
    return ('\n'.join(lines) + '\n').encode('ascii')


def build(config: dict, destination: Path):
    data = render(config)
    # Parent must already exist: do not guess or create an unrelated project tree.
    descriptor, temporary = tempfile.mkstemp(prefix=destination.name + '.', suffix='.tmp', dir=destination.parent)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(data)
        if Path(temporary).read_bytes() != data:
            raise ConfigError('generated header verification failed')
        os.replace(temporary, destination)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('build', 'inspect', 'verify'):
        option = sub.add_parser(command)
        option.add_argument('--config', required=True)
        option.add_argument('--public-default', action='store_true',
                            help='Require the secret-free example layout used by the standard release')
        if command == 'build':
            option.add_argument('--output', required=True)
        elif command == 'verify':
            option.add_argument('--header', required=True)
    args = parser.parse_args(argv)
    try:
        config = load_launch_config(args.config)
        if args.public_default:
            expected = dict(entry='main.lua', mounts=[], directories=[
                dict(base='root', path='.', target='.')])
            if config != expected:
                raise ConfigError('public release requires the exact secret-free example configuration')
        if args.command == 'build':
            build(config, Path(args.output))
            print('Header generated; configuration-only validation, deployment and package data not checked.')
        elif args.command == 'inspect':
            public = {k: v for k, v in config.items() if k != 'secret'}
            public['secretConfigured'] = 'secret' in config
            public['verification'] = 'configuration only; no deployment or package checks'
            print(json.dumps(public, ensure_ascii=False, indent=2))
        else:
            expected = render(config)
            with Path(args.header).open('rb') as stream:
                actual = stream.read(len(expected) + 1)
            if actual != expected:
                raise ConfigError('header does not match the fixed generated data; no C++ was executed')
            # Do not publish a fingerprint of bytes that contain a secret.
            print('Header matches configuration exactly; deployment and package data not checked.')
        return 0
    except (ConfigError, OSError, UnicodeError, OverflowError) as error:
        # Config exceptions never quote values; OS diagnostics refer to explicit
        # input/output names, not the private source contents or key material.
        print(f'make_launch: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
