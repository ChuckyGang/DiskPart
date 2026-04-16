# DiskPart

A native AmigaOS 3.x hard disk partition editor with full RDB (Rigid Disk Block) support.

## Download

**[Latest build](https://github.com/ChuckyGang/DiskPart/releases/latest)**

---

## About

DiskPart is a partition management tool for the Amiga, built as a clean GadTools application that runs directly on Kickstart 3.x with no external library dependencies beyond the ROM. It was created out of a simple conviction: the Amiga deserves a good, modern partition editor — and it is better to have one now than never.

> *"Vibecoded software might be argued with, but this is an experiment in what AI-assisted development can produce when given a clear goal and a demanding user."*

**Director:** John Hertell — john(at)hertell.nu  
**Code:** Claude Code (Anthropic)

---


## Requirements

- AmigaOS 3.x (Kickstart 3.1 or later recommended)
- Intuition, GadTools, DOS libraries (all standard ROM)
- ASL library optional (enables the Browse file requester in the filesystem driver dialog)

---

## Usage

Run `DiskPart` from the Shell or double-click from Workbench.

1. A device selector appears listing all detected disk controllers.
   Use **Filter / Show All** to toggle between storage-only and full device lists.
2. Select a device and click **Select** — a progress window shows each unit being probed.
3. Select a unit and click **Select** to open the partition editor.
4. Use the buttons along the bottom to manage partitions and filesystem drivers.
5. Click **Write** when satisfied to commit changes to disk.

---

## Building

Supports two m68k toolchains, auto-detected by `make`:

- **Bebbo** (`m68k-amigaos-gcc`, default `/opt/amiga`)
- **Bartman/Abyss** (`m68k-amiga-elf-gcc`)

```sh
make                       # auto-detect (prefers Bebbo if /opt/amiga exists)
make TOOLCHAIN=bebbo       # force Bebbo
make TOOLCHAIN=bartman     # force Bartman
```

Or build with the Bebbo toolchain via Docker (no host install required):

```sh
./docker.sh
```

Output: `out/DiskPart.exe`

---

## A Note on Vibecoding

This project was developed through AI-assisted ("vibecoded") collaboration — the architecture, decisions, and direction came from a human; the implementation was written by an AI. Whether that makes the software more or less trustworthy is a fair question. The answer offered here is: judge it by what it does, read the source if you want, and always keep a backup before touching partition tables.

---

## License

MIT License

Copyright (c) 2026 John Hertell

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
