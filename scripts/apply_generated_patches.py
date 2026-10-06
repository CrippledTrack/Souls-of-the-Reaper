#!/usr/bin/env python3
"""Apply Souls of the Reaper guest fixes by symbol, independent of codegen shard numbers."""
import argparse
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]


def patch_generated(directory):
    replacements = {
        "sub_831583B0": (ROOT / "port/linux/setjmp.cpp.in").read_text().strip(),
        "sub_83158680": """DEFINE_REX_FUNC(sub_83158680) {
    const uint32_t buf = ctx.r3.u32;
    const int value = ctx.r4.s32 ? ctx.r4.s32 : 1;
    d3_jmp_regs()[buf].val = static_cast<uint32_t>(value);
    ppc_longjmp(buf, value);
}""",
        "sub_82632E00": """extern "C" void D3RequestGameExit();
DEFINE_REX_FUNC(sub_82632E00) {
    D3RequestGameExit();
}""",
    }
    edits = {}
    for symbol, replacement in replacements.items():
        pattern = re.compile(r"DEFINE_REX_FUNC\(" + symbol + r"\) \{.*?^\}", re.S | re.M)
        matches = []
        for path in directory.glob("*_recomp.*.cpp"):
            text = edits.get(path)
            if text is None:
                text = path.read_text()
            matches.extend((path, text) for _ in pattern.finditer(text))
        if len(matches) != 1:
            raise ValueError(f"Expected one definition of {symbol}, found {len(matches)}")
        path, text = matches[0]
        # Remove our declaration on repeat runs, then replace the whole body.
        text = text.replace('extern "C" void D3RequestGameExit();\n', '')
        text = pattern.sub(lambda _: replacement, text, count=1)
        if symbol != "sub_82632E00" and '#include "guest_jump.h"' not in text:
            text = '#include "guest_jump.h"\n' + text
        edits[path] = text
    # Validate every anchor before modifying any file.
    for path, text in edits.items():
        if path.read_text() != text:
            path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=pathlib.Path,
                        default=ROOT / "port/generated/linux")
    args = parser.parse_args()
    try:
        patch_generated(args.directory)
    except ValueError as error:
        parser.exit(1, f"Guest patches failed: {error}\n")


if __name__ == "__main__":
    main()
