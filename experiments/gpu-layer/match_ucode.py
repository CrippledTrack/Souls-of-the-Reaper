"""Check dumped shader containers against the microcode the GPU actually received.

Usage: python match_ucode.py <container_dump_dir> <rexglue_dump_shaders_dir>
For each `*.ucode.bin.{vert,frag}` written by `--dump_shaders`, look for the same words in
the physical (microcode) part of a container. Prints how many matched and which did not.
"""
import os
import struct
import sys

containers, gpu = sys.argv[1:3]

microcode = {}  # big-endian microcode bytes -> container file
for name in os.listdir(containers):
    data = open(os.path.join(containers, name), "rb").read()
    flags, vsize, psize, _, _, _, shader_offset = struct.unpack_from(">7I", data, 0)
    physical_offset, size = struct.unpack_from(">2I", data, shader_offset)
    start = vsize + physical_offset
    microcode[data[start:start + size]] = name

found = missing = 0
for name in sorted(os.listdir(gpu)):
    if ".ucode.bin." not in name:
        continue
    words = open(os.path.join(gpu, name), "rb").read()
    # rexglue stores host-endian (little-endian) words; containers are big-endian.
    swapped = b"".join(struct.pack(">I", w) for w in struct.unpack(f"<{len(words) // 4}I", words))
    match = microcode.get(swapped) or next((c for m, c in microcode.items() if swapped in m), None)
    if match:
        found += 1
    else:
        missing += 1
        print("no container for", name, len(words), "bytes")
print(f"{found} GPU shaders matched a container, {missing} did not")
