#!/usr/bin/env python3
"""
Generate DiskPart.info — Amiga WBTool icon.

Converts hdicon.png (in support/) to a 36x40, 2-bitplane Amiga .info file.

Colour mapping (Workbench 3.x default palette):
  0 = grey  (Workbench background — used for transparent/outer area)
  1 = black (dark pixels from source image)
  2 = white (light pixels from source image)
  3 = blue  (selection highlight — used for selected state background)

Usage:  python3 support/make_icon.py [output_path]
Output: out/DiskPart.info  (default), or the path passed as the first arg.
"""

import struct, os, sys
from PIL import Image

# --- Config -----------------------------------------------------------
W, H   = 36, 40
DEPTH  = 2
WPL    = (W + 15) // 16   # words per line = 3

REPO_ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_PNG         = os.path.join(REPO_ROOT, 'support', 'hdicon.png')
DEFAULT_OUTPUT  = os.path.join(REPO_ROOT, 'out', 'DiskPart.info')


# --- Image conversion -------------------------------------------------

def load_grid(path: str, w: int, h: int):
    """Load PNG, resize to w×h, return 2-D list of colour indices 0-2."""
    img = Image.open(path).convert('RGBA')
    img = img.resize((w, h), Image.LANCZOS)

    grid = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            # Treat fully-transparent as white background
            if a < 128:
                lum = 255
            else:
                lum = int(0.299 * r + 0.587 * g + 0.114 * b)
            # Dark pixels → colour 1 (black), light pixels → colour 2 (white)
            row.append(1 if lum < 128 else 2)
        grid.append(row)
    return grid


def invert_grid(grid):
    """Selected state: swap colours 1 and 2, leave 0 and 3 alone."""
    swap = {0: 0, 1: 2, 2: 1, 3: 3}
    return [[swap[c] for c in row] for row in grid]


def grid_to_bitplanes(grid) -> bytes:
    """Encode as planar data: all rows of plane-0, then all rows of plane-1."""
    p0 = bytearray()
    p1 = bytearray()
    for row in grid:
        for w in range(WPL):
            b0 = b1 = 0
            for bit in range(16):
                px = w * 16 + bit
                if px < W:
                    c = row[px]
                    if c & 1: b0 |= 0x8000 >> bit
                    if c & 2: b1 |= 0x8000 >> bit
            p0 += struct.pack('>H', b0)
            p1 += struct.pack('>H', b1)
    return bytes(p0) + bytes(p1)


# --- Amiga struct helpers ---------------------------------------------

def pack_image_struct(w, h, depth, has_data, has_next) -> bytes:
    """Image struct — 20 bytes."""
    return struct.pack('>hhhhhIBBI',
        0, 0, w, h, depth,
        1 if has_data else 0,
        (1 << depth) - 1,   # PlanePick
        0,                   # PlaneOnOff
        1 if has_next else 0,
    )


def pack_gadget_struct(w, h) -> bytes:
    """Gadget struct — 44 bytes."""
    return struct.pack('>IhhHHHHHIIIIIHI',
        0,          # NextGadget
        0, 0,       # LeftEdge, TopEdge
        w, h,       # Width, Height
        0x0006,     # Flags: GADGIMAGE | GADGHIMAGE
        0x0000,     # Activation
        0x0001,     # GadgetType: GTYP_BOOLGADGET
        1,          # GadgetRender (non-NULL → image follows)
        1,          # SelectRender (non-NULL → selected image follows)
        0, 0, 0,    # GadgetText, MutualExclude, SpecialInfo
        0, 0,       # GadgetID, UserData
    )


# --- Build .info file -------------------------------------------------

def build_info(default_grid, select_grid) -> bytes:
    default_data = grid_to_bitplanes(default_grid)
    select_data  = grid_to_bitplanes(select_grid)

    out = bytearray()

    # DiskObject header (78 bytes)
    out += struct.pack('>HH', 0xE310, 1)    # Magic, Version
    out += pack_gadget_struct(W, H)          # Gadget (44 bytes)
    out += bytes([3, 0])                     # do_Type=WBTOOL + 1 pad byte
    out += struct.pack('>IIIIII',
        0,           # do_DefaultTool  (NULL)
        0,           # do_ToolTypes    (NULL)
        0x80000000,  # do_CurrentX     (NO_ICON_POSITION)
        0x80000000,  # do_CurrentY     (NO_ICON_POSITION)
        0,           # do_DrawerData   (NULL)
        0,           # do_ToolWindow   (NULL)
    )
    out += struct.pack('>I', 8192)           # do_StackSize
    assert len(out) == 78

    # Default image
    out += pack_image_struct(W, H, DEPTH, has_data=True, has_next=False)
    out += default_data

    # Selected image (colours 1 and 2 swapped)
    out += pack_image_struct(W, H, DEPTH, has_data=True, has_next=False)
    out += select_data

    return bytes(out)


# --- Main -------------------------------------------------------------

if __name__ == '__main__':
    src = SRC_PNG
    if not os.path.exists(src):
        print(f"Source PNG not found: {src}", file=sys.stderr)
        sys.exit(1)

    out_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUTPUT
    out_dir  = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    default_grid = load_grid(src, W, H)
    select_grid  = invert_grid(default_grid)
    data         = build_info(default_grid, select_grid)

    with open(out_path, 'wb') as f:
        f.write(data)

    print(f"Written {out_path} ({len(data)} bytes, {W}x{H} px, {DEPTH} planes)")
