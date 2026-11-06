"""Sanity-check the microcode of dumped shader containers.

Usage: python check_containers.py <container_dump_dir> [results.json]
Walks each shader's control-flow program and checks that every exec block points inside
the shader and that the program ends. Containers whose microcode was not contiguous in
memory when dumped fail this check. With results.json (from run_batch.py), prints how the
recompiler did on sane vs broken containers.
"""
import collections
import json
import os
import struct
import sys

EXEC_LIKE = {1, 2, 3, 4, 5, 6, 13, 14}
ENDS = {2, 4, 6, 14}


def cf_instructions(words):
    """Yield (opcode, address, count) for each 48-bit control-flow instruction."""
    for i in range(0, len(words) - 2, 3):
        w0, w1, w2 = words[i:i + 3]
        lo = w0 | ((w1 & 0xFFFF) << 32)
        hi = (w1 >> 16) | (w2 << 16)
        for value in (lo, hi):
            yield (value >> 44) & 0xF, value & 0xFFF, (value >> 12) & 0x7


def check(data):
    flags, vsize, psize, _, ctab, dtab, shader_offset = struct.unpack_from(">7I", data, 0)
    physical_offset, size = struct.unpack_from(">2I", data, shader_offset)
    start = vsize + physical_offset
    if size == 0 or size % 4 or start + size > len(data):
        return "bad size"
    words = struct.unpack_from(f">{size // 4}I", data, start)
    instructions = size // 12
    limit = instructions
    ended = False
    for count, (op, address, n) in enumerate(cf_instructions(words)):
        if count // 2 >= limit:
            break
        if op in EXEC_LIKE:
            if address == 0 or address + n > instructions:
                return f"exec outside shader ({address}+{n} > {instructions})"
            limit = min(limit, address)
            if op in ENDS:
                ended = True
                break
    return "ok" if ended else "no end"


def main():
    folder = sys.argv[1]
    results = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else {}
    summary = collections.Counter()
    for name in sorted(os.listdir(folder)):
        state = check(open(os.path.join(folder, name), "rb").read())
        state = state if state in ("ok", "bad size", "no end") else "exec outside shader"
        if results:
            passed = "dxil=ok spirv=ok" in results.get(name, {}).get("result", "")
            summary[(state, "compiles" if passed else "fails")] += 1
        else:
            summary[state] += 1
    for key, n in sorted(summary.items(), key=lambda x: -x[1]):
        print(f"{n:5d}  {key}")


if __name__ == "__main__":
    main()
