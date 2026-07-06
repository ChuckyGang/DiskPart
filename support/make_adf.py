#!/usr/bin/env python3
"""
Build an autoboot ADF that boots straight into DiskPart (no Workbench).

The floppy's Startup-Sequence runs DiskPart directly against the default
boot CLI screen dos.library opens before running Startup-Sequence, so no
Workbench or other disk-based OS files are needed on the floppy.

Requires amitools (`pip install amitools`) for its xdftool.

Usage:
  python3 support/make_adf.py output.adf path/to/DiskPart
"""

import subprocess
import sys
import os
import tempfile


def main():
    if len(sys.argv) != 3:
        print("Usage: make_adf.py output.adf DiskPart-binary")
        sys.exit(1)

    adf_path, binary_path = sys.argv[1], sys.argv[2]

    if os.path.exists(adf_path):
        os.remove(adf_path)

    with tempfile.TemporaryDirectory() as td:
        startup_path = os.path.join(td, "Startup-Sequence")
        with open(startup_path, "w", newline="\n") as f:
            f.write("DiskPart\n")

        cmd = [
            "xdftool", adf_path,
            "create", "+",
            "format", "diskpart", "ffs", "+",
            "makedir", "S", "+",
            "write", startup_path, "S/Startup-Sequence", "+",
            "write", binary_path, "DiskPart", "+",
            "boot", "install", "boot2x3x",
        ]
        subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
