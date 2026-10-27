"""One step from your ROM to buildable sources.

    python tools/build_sources.py "Mighty Morphin Power Rangers - The Movie (USA, Europe, Brazil) (En).gg"

1. checks the ROM is the expected dump (CRC32 B47C19E5)
2. tools/trace.py   -> disasm/   (annotated disassembly + code_map.json)
3. tools/recomp.py  -> generated/ (Z80 -> C, using tools/hints.json and tools/coverage.txt)

Then build with CMake (see README.md).
"""
import os
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXPECTED_CRC = 0xB47C19E5


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    rom = sys.argv[1]
    data = open(rom, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if crc != EXPECTED_CRC:
        sys.exit(f"ROM CRC32 is {crc:08X}, expected {EXPECTED_CRC:08X}. "
                 "Use the (USA, Europe, Brazil) (En) dump; the hints and coverage are specific to it.")
    py = sys.executable
    subprocess.check_call([py, os.path.join(HERE, "trace.py"), rom, os.path.join(ROOT, "disasm")])
    subprocess.check_call([py, os.path.join(HERE, "recomp.py"), rom,
                           os.path.join(ROOT, "disasm", "code_map.json"),
                           os.path.join(ROOT, "generated"),
                           os.path.join(HERE, "coverage.txt")])
    print("Sources generated. Next: cmake -B build && cmake --build build")


if __name__ == "__main__":
    main()
