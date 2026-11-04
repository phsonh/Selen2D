#!/usr/bin/env python3
"""Build, inspect and verify SLPK resources; see --help for development usage."""
import argparse
import json
import sys
from launch_common import ConfigError, read_json, secret_bytes
from resource_package import Package, PackageError, derive_key, pack

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='operation', required=True)
    build = commands.add_parser('pack', help='atomically create an SLPK v1 or encrypted v2 package')
    build.add_argument('--source', required=True)
    build.add_argument('--output', required=True)
    build.add_argument('--block-size', type=int, default=262144)
    build.add_argument('--level', type=int, default=6)
    build.add_argument('--encrypt', action='store_true')
    build.add_argument('--config')
    for name in ('inspect','verify'):
        command = commands.add_parser(name)
        command.add_argument('--input', required=True)
        command.add_argument('--config')
    args = parser.parse_args(argv)
    try:
        config = read_json(args.config) if args.config else {}
        required = args.operation == 'pack' and args.encrypt
        secret = secret_bytes(config, required=required)
        key = derive_key(secret) if secret is not None else None
        if args.operation == 'pack':
            if args.config and not args.encrypt:
                raise PackageError('--config for pack requires --encrypt')
            result = pack(args.source,args.output,block_size=args.block_size,level=args.level,
                          key=key if args.encrypt else None)
        else:
            with Package(args.input,key) as package:
                result = dict(package.public_header)
                if args.operation == 'verify':
                    package.verify(); result['verified'] = True
                else:
                    result['index_authenticated'] = package.encrypted and package.entries is not None
                    result['payload_verified'] = False
                    if package.entries is None:
                        result['warning'] = 'UNAUTHENTICATED public header only; supply --config to authenticate the index'
                    else:
                        result['files'] = [{'path':e.path,'type':'directory' if e.directory else 'file','size':e.size,'blocks':len(e.blocks)} for e in package.entries]
        print(json.dumps(result,ensure_ascii=False,indent=2))
        return 0
    except (OSError,ValueError) as error:
        print(f'{args.operation}: {error}',file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
