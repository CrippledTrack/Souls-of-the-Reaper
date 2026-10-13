#!/usr/bin/env python3
"""Extract an XDVDFS game partition; retain a SHA-256 inventory of every file."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

CHUNK = 4 * 1024 * 1024


class Disc:
    """Read-only view of an Xbox 360 ISO's XDVDFS game partition."""

    def __init__(self, source):
        self.source = source
        source.seek(0, 2)
        self.size = source.tell()
        bases = (0, 0x2080000, 0xFD90000)
        matches = [b for b in bases if self.read(b + 32 * 2048, 20) == b'MICROSOFT*XBOX*MEDIA']
        if len(matches) != 1:
            raise ValueError(f'Expected one game partition, found {matches}')
        self.base = matches[0]
        self.root = struct.unpack_from('<II', self.read(self.base + 32 * 2048, 2048), 20)

    def read(self, offset, length):
        if offset < 0 or length < 0 or offset + length > self.size:
            raise ValueError('Read outside ISO')
        self.source.seek(offset)
        data = self.source.read(length)
        if len(data) != length:
            raise ValueError('Truncated ISO')
        return data

    def entries(self):
        """Yield (relative path, is_directory, sector, length) in disc order."""
        visited = set()

        def directory(sector, size, relative):
            if sector in visited or size > 16 * 1024 * 1024:
                raise ValueError('Invalid or cyclic directory')
            visited.add(sector)
            table = self.read(self.base + sector * 2048, size)
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
                    yield from node(left * 4)
                path = relative / name
                if attr & 0x10:
                    yield path, True, sec, length
                    if length:
                        yield from directory(sec, length, path)
                else:
                    yield path, False, sec, length
                if right:
                    yield from node(right * 4)

            yield from node(0)

        yield from directory(*self.root, Path())

    def chunks(self, sector, length):
        remaining, position = length, self.base + sector * 2048
        while remaining:
            data = self.read(position, min(remaining, CHUNK))
            yield data
            remaining -= len(data)
            position += len(data)


def file_sha256(iso, name):
    """SHA-256 of one top-level file, without extracting the disc."""
    with iso.open('rb') as source:
        disc = Disc(source)
        for path, is_dir, sector, length in disc.entries():
            if not is_dir and path.as_posix().lower() == name.lower():
                digest = hashlib.sha256()
                for data in disc.chunks(sector, length):
                    digest.update(data)
                return digest.hexdigest()
    raise ValueError(f'{name} not found on disc')


def extract(iso, destination):
    records = []
    with iso.open('rb') as source:
        disc = Disc(source)
        destination.mkdir(parents=True, exist_ok=True)
        for path, is_dir, sector, length in disc.entries():
            target = destination / path
            if is_dir:
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            # Existing files are checked, never silently replaced.
            existing = target.exists()
            with target.open('rb' if existing else 'xb') as stream:
                for data in disc.chunks(sector, length):
                    if existing:
                        if stream.read(len(data)) != data:
                            raise ValueError(f'Existing file differs: {path}')
                    else:
                        stream.write(data)
                    digest.update(data)
                if existing and stream.read(1):
                    raise ValueError(f'Existing file too long: {path}')
            records.append(dict(path=path.as_posix(), size=length, sector=sector,
                                sha256=digest.hexdigest()))
            print(f'{path}: {length:,} bytes', flush=True)
        manifest = dict(source=iso.name, source_size=disc.size, partition_offset=disc.base, files=records)
    (destination / 'disc-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    extract(args.iso, args.destination)
