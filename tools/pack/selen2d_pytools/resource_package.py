"""SLPK v1/v2 format implementation. This module never executes launch config.

Package tools require requirements-tools.txt; ordinary engine builds only need
the separate launch dependencies. Runtime needs no Python. Cryptography uses
PyNaCl/libsodium.
"""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import secrets
import stat
import struct
import tempfile
from dataclasses import dataclass, field
import zlib
import zstandard as zstd
from nacl.bindings import (crypto_aead_xchacha20poly1305_ietf_encrypt as encrypt,
                           crypto_aead_xchacha20poly1305_ietf_decrypt as decrypt)
from nacl.exceptions import CryptoError
from .launch_common import normalize_path, fold_path

MAX_POSITION = (1 << 53) - 1
MAX_COUNT = 1048576
MAX_INDEX = 64 * 1024 * 1024
MAX_MEMORY = 256 * 1024 * 1024
MIN_BLOCK, MAX_BLOCK = 65536, 1048576
INDEX_DOMAIN, BLOCK_DOMAIN = b'SLPK2-INDEX-AAD!', b'SLPK2-BLOCK-AAD!'
ENTRY = struct.Struct('<BBHIQII')
BLOCK = struct.Struct('<QIIIB3s')

class PackageError(ValueError):
    pass

class OperationCancelled(PackageError):
    """Cooperative cancellation; an existing output package is left intact."""
    pass

def checkpoint(cancelled):
    if cancelled is not None and cancelled():
        raise OperationCancelled('operation cancelled')

def report(progress, phase, done, total):
    if progress is not None:
        progress(phase, done, total)

def require(condition, message):
    if not condition:
        raise PackageError(message)

def derive_key(secret: bytes) -> bytes:
    return hashlib.sha256(secret).digest()

def block_size_check(value):
    require(isinstance(value, int) and MIN_BLOCK <= value <= MAX_BLOCK and value & (value-1) == 0,
            'block size must be a power of two from 65536 through 1048576')

def header_v1(count, block_size, index_offset, index_size):
    return struct.pack('<4sHHIIIQQQI', b'SLPK', 1, 0, 48, count, block_size,
                       index_offset, index_size, 48, 0)

def header_v2(count, block_size, index_offset, index_size, package_id, nonce):
    return struct.pack('<4sHHIIIHHQQQQ16s24s16s', b'SLPK', 2, 1, 112, count,
                       block_size, 1, 1, 112, index_offset, index_size,
                       index_size+16, package_id, nonce, bytes(16))

def block_aad(header, entry_index, block_index, descriptor):
    return BLOCK_DOMAIN + header[:32] + header[56:72] + struct.pack('<II', entry_index, block_index) + descriptor

@dataclass
class BlockInfo:
    offset: int
    stored: int
    raw: int
    crc: int
    codec: int
    descriptor: bytes

@dataclass
class EntryInfo:
    path: str
    directory: bool
    size: int
    index: int
    blocks: list[BlockInfo] = field(default_factory=list)

class Package:
    """Retains one opened physical source; payloads are verified on demand."""
    def __init__(self, path, key: bytes | None = None, *, encrypted: bool | None = None):
        self.file = open(path, 'rb')
        try:
            self._load(key, encrypted)
        except BaseException:
            self.file.close()
            raise

    def __enter__(self): return self
    def __exit__(self, *_): self.close()
    def close(self): self.file.close()
    def read_at(self, offset, size):
        require(0 <= offset <= self.physical_size and 0 <= size <= self.physical_size-offset,
                'record outside source bounds')
        self.file.seek(offset)
        data = self.file.read(size)
        require(len(data) == size, 'source truncated during read')
        return data

    def _load(self, key, encrypted):
        self.physical_size = os.fstat(self.file.fileno()).st_size
        require(48 <= self.physical_size <= MAX_POSITION, 'invalid physical size')
        first = self.read_at(0, 48)
        require(first[:4] == b'SLPK', 'unsupported magic')
        self.version, flags, header_size, count, self.block_size = struct.unpack_from('<HHIII', first, 4)
        require(self.version in (1, 2), 'unsupported package version')
        self.encrypted = self.version == 2
        if encrypted is not None:
            require(self.encrypted == encrypted, 'package version does not match configured encryption')
        require(header_size == (112 if self.encrypted else 48), 'invalid header size')
        self.header = self.read_at(0, header_size)
        block_size_check(self.block_size)
        require(count <= MAX_COUNT, 'entry count exceeds limit')
        require(flags == (1 if self.encrypted else 0), 'unsupported flags')
        if self.encrypted:
            aead, key_id, data_offset, index_offset, plain_size, stored_size = struct.unpack_from('<HHQQQQ', self.header, 20)
            require(aead == key_id == 1, 'unsupported encryption/key algorithm')
            require(data_offset == 112 and self.header[96:] == bytes(16), 'invalid data offset or reserved fields')
            require(plain_size <= MAX_INDEX and stored_size == plain_size+16, 'invalid encrypted index size')
        else:
            index_offset, plain_size, data_offset, reserved = struct.unpack_from('<QQQI', first, 20)
            stored_size = plain_size
            require(data_offset == 48 and reserved == 0, 'invalid data offset or reserved fields')
            require(plain_size <= MAX_INDEX, 'index exceeds limit')
        require(header_size <= index_offset <= self.physical_size and stored_size == self.physical_size-index_offset,
                'invalid index bounds or trailing bytes')
        self.public_header = dict(version=self.version, encrypted=self.encrypted, entries=count,
                                  block_size=self.block_size, index_offset=index_offset,
                                  index_plain_size=plain_size, physical_size=self.physical_size)
        self.entries = None
        self.key = key
        if self.encrypted and key is None:
            return  # Public header only: explicitly unauthenticated.
        encoded = self.read_at(index_offset, stored_size)
        if self.encrypted:
            require(len(key) == 32, 'key must contain 32 bytes')
            try:
                data = decrypt(encoded, INDEX_DOMAIN+self.header, self.header[72:96], key)
            except CryptoError:
                raise PackageError('authentication failed (key mismatch or damaged/modified content)') from None
        else:
            data = encoded
        self.entries = []
        self.nodes = {}
        folded, explicit = {}, set()
        nonces = {self.header[72:96]} if self.encrypted else set()
        intervals = []
        cursor, total_blocks, next_offset, previous = 0, 0, header_size, b''
        memory = len(encoded)+len(data)
        def take(n):
            nonlocal cursor
            require(0 <= n <= len(data)-cursor, 'truncated index')
            value = data[cursor:cursor+n]
            cursor += n
            return value
        def account(n):
            nonlocal memory
            memory += n
            require(memory <= MAX_MEMORY, 'package parsing memory budget exceeded')
        def add_node(path, directory, size):
            if path in self.nodes:
                require(self.nodes[path][0] == directory, 'file/directory ancestor conflict')
                return
            folded_path = fold_path(path)
            require(folded_path not in folded or folded[folded_path] == path, 'Unicode case-fold path collision')
            account(768+len(path.encode('utf-8'))*8+len(folded_path.encode('utf-8'))*4)
            folded[folded_path] = path
            self.nodes[path] = (directory, size)
        for entry_index in range(count):
            kind, entry_flags, reserved, path_len, file_size, block_count, reserved2 = ENTRY.unpack(take(24))
            require(kind <= 1 and entry_flags == reserved == reserved2 == 0, 'invalid entry kind/flags/reserved fields')
            require(1 <= path_len <= 4096 and file_size <= MAX_POSITION, 'invalid path or file size')
            require(block_count <= MAX_COUNT-total_blocks, 'block count exceeds limit')
            total_blocks += block_count
            path_bytes = take(path_len)
            try: path = path_bytes.decode('utf-8', 'strict')
            except UnicodeError: raise PackageError('invalid UTF-8 package path') from None
            require(normalize_path(path) == path, 'noncanonical package path')
            require(not self.encrypted or previous < path_bytes, 'v2 paths are not strictly byte-sorted')
            previous = path_bytes
            require(path not in explicit, 'duplicate path')
            explicit.add(path)
            require((file_size == block_count == 0) if kind else
                    block_count == (file_size+self.block_size-1)//self.block_size,
                    'invalid directory/file block count')
            add_node(path, bool(kind), file_size)
            components = path.split('/')
            for n in range(1, len(components)): add_node('/'.join(components[:n]), True, 0)
            entry = EntryInfo(path, bool(kind), file_size, entry_index)
            account(512+path_len*4+block_count*512)
            raw_total = 0
            for block_index in range(block_count):
                descriptor = take(48 if self.encrypted else 24)
                offset, stored, raw, crc, codec, padding = BLOCK.unpack(descriptor[:24])
                require(1 <= raw <= self.block_size and (block_index+1 == block_count or raw == self.block_size), 'invalid raw block size')
                require(codec <= 1 and padding == bytes(3), 'invalid codec/reserved fields')
                require(stored > 0 and header_size <= offset <= index_offset and stored <= index_offset-offset, 'block outside payload')
                require(codec != 0 or stored == raw+(16 if self.encrypted else 0), 'raw block stored size mismatch')
                if self.encrypted:
                    require(16 < stored <= raw+16 and (codec == 0 or stored-16 < raw), 'invalid encrypted stored size')
                    require(offset == next_offset, 'v2 blocks are not tightly ordered')
                    next_offset += stored
                    nonce = descriptor[24:]
                    require(nonce not in nonces, 'duplicate nonce')
                    nonces.add(nonce)
                else:
                    intervals.append((offset, offset+stored))
                entry.blocks.append(BlockInfo(offset, stored, raw, crc, codec, descriptor))
                raw_total += raw
            require(raw_total == file_size, 'logical size differs from block sum')
            self.entries.append(entry)
        require(cursor == len(data), 'extra index bytes')
        if self.encrypted:
            require(next_offset == index_offset, 'v2 payload gap')
        else:
            end = header_size
            for start, stop in sorted(intervals):
                require(start >= end, 'overlapping payload blocks')
                end = stop

    def read_block(self, entry: EntryInfo, index: int) -> bytes:
        block = entry.blocks[index]
        require(block.stored <= 64*1024*1024, 'verification work memory budget exceeded')
        encoded = self.read_at(block.offset, block.stored)
        if self.encrypted:
            try:
                encoded = decrypt(encoded, block_aad(self.header, entry.index, index, block.descriptor), block.descriptor[24:], self.key)
            except CryptoError:
                raise PackageError('authentication failed (key mismatch or damaged/modified content)') from None
        if block.codec:
            try:
                frame = zstd.get_frame_parameters(encoded)
                require((frame.content_size == block.raw or not self.encrypted and frame.content_size == zstd.CONTENTSIZE_UNKNOWN)
                        and (not self.encrypted or frame.window_size <= self.block_size) and frame.dict_id == 0,
                        'invalid Zstd frame header')
                require(len(encoded)+2*frame.window_size+block.raw <= 64*1024*1024,
                        'Zstd verification work memory budget exceeded')
                raw = zstd.ZstdDecompressor(max_window_size=max(1024, frame.window_size)).decompress(
                    encoded, max_output_size=block.raw, allow_extra_data=False)
            except zstd.ZstdError as error:
                raise PackageError('invalid Zstd frame: '+str(error)) from None
        else:
            raw = encoded
        require(len(raw) == block.raw, 'decoded size mismatch')
        require(zlib.crc32(raw) == block.crc, 'CRC32 mismatch')
        return raw

    def verify(self, *, progress=None, cancelled=None):
        require(self.entries is not None, 'verification requires an explicit secret for encrypted packages')
        total = sum(e.size for e in self.entries)
        done = 0
        report(progress, 'verify', 0, total)
        for entry in self.entries:
            checkpoint(cancelled)
            for n in range(len(entry.blocks)):
                checkpoint(cancelled)
                done += len(self.read_block(entry, n))
                report(progress, 'verify', done, total)
        checkpoint(cancelled)
        return self.public_header

@dataclass
class SourceEntry:
    path: str
    physical: Path
    directory: bool
    identity: tuple

def identity(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns)

def reject_link(path, info):
    require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
            'source/output paths may not use symlinks or reparse points: '+str(path))

def open_source(path):
    """Bind one regular-file generation and deny concurrent Windows writes."""
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        import msvcrt
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        create = kernel.CreateFileW
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                           wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        create.restype = wintypes.HANDLE
        close = kernel.CloseHandle
        close.argtypes = [wintypes.HANDLE]
        close.restype = wintypes.BOOL
        # Read + share-read/share-delete + open-existing + open-reparse-point.
        # A concurrent writable handle makes this fail rather than produce a
        # mixed snapshot whose mtime might update only after that writer closes.
        handle = create(str(path), 0x80000000, 1 | 4, None, 3, 0x00200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise OSError(ctypes.get_last_error(), 'cannot open stable source file', str(path))
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY | os.O_NOINHERIT)
        except BaseException:
            close(handle)
            raise
    else:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    stream = os.fdopen(descriptor, 'rb')
    try:
        info = os.fstat(stream.fileno())
        reject_link(path, info)
        require(stat.S_ISREG(info.st_mode), 'source must remain a regular file: '+str(path))
        return stream
    except BaseException:
        stream.close()
        raise

def inventory(source: Path, *, cancelled=None):
    root_info = source.lstat()
    reject_link(source, root_info)
    require(stat.S_ISDIR(root_info.st_mode), 'source must be a directory')
    entries, paths, folded = [], set(), {}
    def visit(directory, components):
        for native in os.scandir(directory):
            checkpoint(cancelled)
            physical = Path(native.path)
            # Windows DirEntry.stat uses enumeration metadata with zero file
            # identity fields. lstat obtains the actual volume/file identity.
            info = physical.lstat()
            reject_link(physical, info)
            is_directory = stat.S_ISDIR(info.st_mode)
            require(is_directory or stat.S_ISREG(info.st_mode), 'source entry must be a regular file or directory: '+str(physical))
            parts = components+[native.name]
            name = normalize_path('/'.join(parts), 'source path')
            require(name not in paths, 'NFC duplicate source path: '+name)
            folded_name = fold_path(name)
            require(folded_name not in folded, 'Unicode case-fold source collision: '+name)
            paths.add(name); folded[folded_name] = name
            entries.append(SourceEntry(name, physical, is_directory, identity(info)))
            require(len(entries) <= MAX_COUNT, 'entry count exceeds limit')
            if is_directory: visit(physical, parts)
    visit(source, [])
    entries.sort(key=lambda item: item.path.encode('utf-8'))
    return entries

def unchanged(before, after):
    return [(e.path,e.directory,e.identity) for e in before] == [(e.path,e.directory,e.identity) for e in after]

def check_output(source, output):
    # Resolve existing parents, then reject aliases into the source tree by
    # components (never a string-prefix test). No recursive deletion is used.
    source_resolved = source.resolve(strict=True)
    output_resolved = output.parent.resolve(strict=True)/output.name
    try:
        inside = os.path.commonpath([source_resolved, output_resolved]) == str(source_resolved)
    except ValueError:  # Different Windows volumes cannot contain one another.
        inside = False
    require(not inside, 'output must be outside the source tree')
    for parent in [source, *source.parents, output.parent, *output.parent.parents]:
        reject_link(parent, parent.lstat())
    if output.exists() or output.is_symlink():
        info = output.lstat(); reject_link(output, info)
        require(stat.S_ISREG(info.st_mode), 'existing output must be a regular file')
    return output_resolved

def pack(source, output, *, block_size=262144, level=6, key=None, progress=None, cancelled=None):
    """Build to a private sibling, verify all blocks, then atomically replace."""
    block_size_check(block_size)
    require(isinstance(level, int) and -131072 <= level <= zstd.MAX_COMPRESSION_LEVEL, 'unsupported Zstd level')
    require(key is None or isinstance(key, bytes) and len(key) == 32, 'key must contain 32 bytes')
    source, output = Path(source).absolute(), Path(output).absolute()
    output = check_output(source, output)
    checkpoint(cancelled)
    report(progress, 'scan', 0, 0)
    entries = inventory(source, cancelled=cancelled)
    total_bytes = sum(e.identity[3] for e in entries if not e.directory)
    packed_bytes = 0
    report(progress, 'pack', 0, total_bytes)
    encrypted = key is not None
    compressor = zstd.ZstdCompressor(level=level, write_content_size=True, write_checksum=False, write_dict_id=False)
    nonces = set()
    def nonce():
        while True:
            value = secrets.token_bytes(24)
            if value not in nonces:
                nonces.add(value); return value
    package_id = secrets.token_bytes(16) if encrypted else None
    index_nonce = nonce() if encrypted else None
    header = header_v2(len(entries), block_size, 0, 0, package_id, index_nonce) if encrypted else header_v1(len(entries), block_size, 0, 0)
    temporary = None
    try:
        fd, temporary_name = tempfile.mkstemp(prefix='.'+output.name+'.', suffix='.tmp', dir=output.parent)
        temporary = Path(temporary_name)
        with os.fdopen(fd, 'w+b') as destination:
            destination.write(header)
            index_data = bytearray(); total_blocks = 0
            for entry_index, item in enumerate(entries):
                checkpoint(cancelled)
                path = item.path.encode('utf-8')
                size = 0 if item.directory else item.identity[3]
                require(size <= MAX_POSITION, 'logical size exceeds exact-position limit: '+item.path)
                block_count = (size+block_size-1)//block_size
                total_blocks += block_count
                require(total_blocks <= MAX_COUNT, 'block count exceeds limit')
                projected = 24+len(path)+block_count*(48 if encrypted else 24)
                require(len(index_data)+projected <= MAX_INDEX, 'index exceeds limit')
                index_data += ENTRY.pack(int(item.directory), 0, 0, len(path), size, block_count, 0)+path
                if item.directory: continue
                with open_source(item.physical) as source_file:
                    require(identity(os.fstat(source_file.fileno())) == item.identity, 'source changed before opening: '+item.path)
                    for block_index in range(block_count):
                        checkpoint(cancelled)
                        raw = source_file.read(min(block_size, size-block_index*block_size))
                        require(len(raw) == min(block_size, size-block_index*block_size), 'source truncated while packing: '+item.path)
                        compressed = compressor.compress(raw)
                        codec = int(len(compressed) < len(raw))
                        payload = compressed if codec else raw
                        offset = destination.tell()
                        stored = len(payload)+(16 if encrypted else 0)
                        require(offset+stored <= MAX_POSITION, 'output exceeds exact-position limit')
                        descriptor = BLOCK.pack(offset, stored, len(raw), zlib.crc32(raw), codec, bytes(3))
                        if encrypted:
                            block_nonce = nonce(); descriptor += block_nonce
                            payload = encrypt(payload, block_aad(header, entry_index, block_index, descriptor), block_nonce, key)
                        destination.write(payload); index_data += descriptor
                        packed_bytes += len(raw)
                        report(progress, 'pack', packed_bytes, total_bytes)
                    require(not source_file.read(1) and identity(os.fstat(source_file.fileno())) == item.identity,
                            'source changed while packing: '+item.path)
            require(unchanged(entries, inventory(source, cancelled=cancelled)), 'source tree changed while packing')
            index_offset = destination.tell()
            header = header_v2(len(entries), block_size, index_offset, len(index_data), package_id, index_nonce) if encrypted else header_v1(len(entries), block_size, index_offset, len(index_data))
            payload = encrypt(bytes(index_data), INDEX_DOMAIN+header, index_nonce, key) if encrypted else index_data
            require(index_offset+len(payload) <= MAX_POSITION, 'output exceeds exact-position limit')
            destination.write(payload); destination.seek(0); destination.write(header)
            destination.flush(); os.fsync(destination.fileno())
        with Package(temporary, key, encrypted=encrypted) as package:
            package.verify(progress=progress, cancelled=cancelled)
        checkpoint(cancelled)
        os.replace(temporary, output)
        temporary = None
        return {'output': str(output), 'version': 2 if encrypted else 1, 'entries':len(entries), 'blocks':total_blocks}
    finally:
        if temporary is not None: temporary.unlink(missing_ok=True)
