"""Run the XenosRecomp batch harness over every dumped shader and summarise the results.

Usage: python run_batch.py <exe> <common_header> <dump_dir> <out_dir> [workers]
Splits the shaders over a few long-lived processes; restarts after a hard crash.
"""
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys

exe, header, dump, out = sys.argv[1:5]
workers = int(sys.argv[5]) if len(sys.argv) > 5 else 4
os.makedirs(out, exist_ok=True)


def parse(text):
    """Split '=== path' delimited output into {path: lines}."""
    sections, current = {}, None
    for line in text.splitlines():
        if line.startswith("=== "):
            current = line[4:].strip()
            sections[current] = []
        elif current:
            sections[current].append(line)
    return sections


def run(paths, index):
    results, pending = {}, list(paths)
    while pending:
        listfile = os.path.join(out, f"list{index}.txt")
        with open(listfile, "w") as f:
            f.write("\n".join(pending) + "\n")
        p = subprocess.run([exe, "--batch", listfile, out, header], capture_output=True, text=True,
                           errors="replace")
        so, se = parse(p.stdout), parse(p.stderr)
        done = 0
        for path in pending:
            lines = so.get(path)
            if lines is None:
                break
            result = next((l[7:] for l in lines if l.startswith("RESULT ")), None)
            if result is None:  # the process died on this shader
                result = f"process died 0x{p.returncode & 0xFFFFFFFF:08X}"
            results[path] = (result, "\n".join(se.get(path, [])))
            done += 1
        if done == 0:
            results[pending[0]] = (f"process died 0x{p.returncode & 0xFFFFFFFF:08X}", "")
            done = 1
        pending = pending[done:]
    return results


names = sorted(os.path.join(dump, n).replace("\\", "/") for n in os.listdir(dump) if n.endswith(".bin"))
chunks = [names[i::workers] for i in range(workers)]
results = {}
with concurrent.futures.ThreadPoolExecutor(workers) as pool:
    for r in pool.map(run, chunks, range(workers)):
        results.update(r)


def bucket(result, err):
    if result.startswith(("ps ", "vs ")):
        kind = result[:2]
        dxil = "dxil=ok" in result
        spirv = "spirv=ok" in result
        if dxil and spirv:
            return kind, "ok"
        first = next((l for l in err.splitlines() if "error:" in l and "validation errors" not in l), "")
        first = re.sub(r"^.*?error: ", "", first)
        first = re.sub(r"'[^']*'", "'…'", first)[:100]
        return kind, f"{'dxil ' if not dxil else ''}{'spirv ' if not spirv else ''}fail: {first}"
    return "??", re.sub(r"0x[0-9A-F]{8}", lambda m: m.group(0), result)[:120]


summary = collections.Counter()
for path, (result, err) in results.items():
    summary[bucket(result, err)] += 1

with open(os.path.join(out, "results.json"), "w") as f:
    json.dump({os.path.basename(p): {"result": r, "stderr": e[-4000:]} for p, (r, e) in results.items()},
              f, indent=1)
print(f"{len(results)} shaders")
for (kind, b), n in sorted(summary.items(), key=lambda x: -x[1]):
    print(f"{n:5d}  {kind}  {b}")
