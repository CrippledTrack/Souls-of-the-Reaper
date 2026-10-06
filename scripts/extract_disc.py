#!/usr/bin/env python3
"""Extract an XDVDFS game partition; retain a SHA-256 inventory of every file."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def extract(iso, destination):
    total = iso.stat().st_size
    records = []
    visited = set()
    with iso.open('rb') as source:
        def read(offset, length):
            if offset < 0 or length < 0 or offset + length > total:
                raise ValueError('Read outside ISO')
            source.seek(offset)
            data = source.read(length)
            if len(data) != length:
                raise ValueError('Truncated ISO')
            return data

        bases = (0, 0x2080000, 0xFD90000)
        matches = [b for b in bases if read(b + 32 * 2048, 20) == b'MICROSOFT*XBOX*MEDIA']
        if len(matches) != 1:
            raise ValueError(f'Expected one game partition, found {matches}')
        base = matches[0]
        sector, size = struct.unpack_from('<II', read(base + 32 * 2048, 2048), 20)

        def directory(sector, size, relative):
            if sector in visited or size > 16 * 1024 * 1024:
                raise ValueError('Invalid or cyclic directory')
            visited.add(sector)
            table = read(base + sector * 2048, size)
            nodes = set()

            def node(offset):
                if offset in nodes or offset + 14 > len(table):
                    raise ValueError('Invalid directory entry')
                nodes.add(offset)
                left, right, sec, length, attr, count = struct.unpack_from('<HHIIBB', table, offset)
                if offset + 14 + count > len(table):
                    raise ValueError('Truncated name')
                name = table[offset + 14:offset + 14 + count].decode('ascii')
                if not name or name in ('.', '..') or any(c in name for c in '/\\\0'):
                    raise ValueError('Unsafe filename')
                if left:
                    node(left * 4)
                path = relative / name
                target = destination / path
                if attr & 0x10:
                    target.mkdir(parents=True, exist_ok=True)
                    if length:
                        directory(sec, length, path)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    digest = hashlib.sha256()
                    # Existing files are checked, never silently replaced.
                    existing = target.exists()
                    stream = target.open('rb' if existing else 'xb')
                    with stream:
                        remaining, position = length, base + sec * 2048
                        while remaining:
                            data = read(position, min(remaining, 4 * 1024 * 1024))
                            if existing:
                                if stream.read(len(data)) != data:
                                    raise ValueError(f'Existing file differs: {path}')
                            else:
                                stream.write(data)
                            digest.update(data)
                            remaining -= len(data)
                            position += len(data)
                        if existing and stream.read(1):
                            raise ValueError(f'Existing file too long: {path}')
                    records.append(dict(path=path.as_posix(), size=length, sector=sec,
                                        sha256=digest.hexdigest()))
                    print(f'{path}: {length:,} bytes', flush=True)
                if right:
                    node(right * 4)

            node(0)

        destination.mkdir(parents=True, exist_ok=True)
        directory(sector, size, Path())
    manifest = dict(source=iso.name, source_size=total, partition_offset=base, files=records)
    (destination / 'disc-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    extract(args.iso, args.destination)
