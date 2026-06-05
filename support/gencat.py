#!/usr/bin/env python3
"""
gencat.py - DiskPart catalog toolchain (CatComp/FlexCat-compatible .cd workflow).

The .cd "catalog description" file is the single source of truth for every
translatable string.  This script replaces the parts of the FlexCat/CatComp
toolchain we need so the build is reproducible without those tools installed:

  header   <cd> <out.h>            Generate the C string-id header + the
                                   built-in (English) default string table.
  ct       <cd> <out.ct> [lang]    Emit a translation template for a translator.
  catalog  <cd> <in.ct> <out.catalog>
                                   Compile a translator's .ct into a binary
                                   locale.library .catalog (IFF FORM CTLG).

The .cd / .ct format is the usual CatComp one:

    #language english          ; control lines start with '#'
    #version 1
    ;
    ; a comment                ; comment lines start with ';'
    MSG_OK (//)                ; identifier, optional (ID/MinLen/MaxLen)
    OK                         ; the default string (one logical line)

IDs are assigned sequentially from 0 unless an explicit ID is given in the
(ID/MinLen/MaxLen) field, exactly like CatComp.  Escapes \\n \\t \\r \\e \\\\
\\" and \\xNN are understood.  A trailing backslash continues a long string
onto the next physical line.
"""

import sys
import struct


# --------------------------------------------------------------------------
# .cd / .ct parsing
# --------------------------------------------------------------------------

class Msg:
    __slots__ = ("ident", "num", "minlen", "maxlen", "text", "comment")

    def __init__(self, ident, num):
        self.ident = ident
        self.num = num
        self.minlen = 0
        self.maxlen = 0
        self.text = None        # decoded bytes (the actual string value)
        self.comment = []       # preceding ';' comment lines (for templates)


def _unescape(s):
    """Decode CatComp escapes in a source line into raw bytes."""
    out = bytearray()
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c != "\\":
            out.extend(c.encode("latin-1"))
            i += 1
            continue
        i += 1
        if i >= n:                      # trailing backslash handled by caller
            out.append(ord("\\"))
            break
        e = s[i]
        i += 1
        if e == "n":
            out.append(0x0A)
        elif e == "t":
            out.append(0x09)
        elif e == "r":
            out.append(0x0D)
        elif e == "e":
            out.append(0x1B)
        elif e == "0":
            out.append(0x00)
        elif e == "\\":
            out.append(0x5C)
        elif e == '"':
            out.append(0x22)
        elif e == "x":
            hexs = s[i:i + 2]
            out.append(int(hexs, 16) & 0xFF)
            i += 2
        else:
            out.append(ord(e))          # unknown escape -> literal char
    return bytes(out)


def parse_cd(path, want_text=True):
    """Parse a .cd (or .ct) file.  Returns (header_dict, [Msg, ...])."""
    header = {"language": "english", "version": "0", "name": "DiskPart"}
    msgs = []
    next_id = 0
    pending_comments = []
    cur = None
    expect_string = False

    with open(path, "r", encoding="latin-1") as fh:
        raw_lines = fh.read().split("\n")

    i = 0
    while i < len(raw_lines):
        line = raw_lines[i]
        i += 1
        stripped = line.strip()

        if expect_string:
            # The line(s) immediately following an identifier are its string.
            text = line
            # backslash line continuation
            while text.endswith("\\") and not text.endswith("\\\\") and i < len(raw_lines):
                text = text[:-1] + raw_lines[i]
                i += 1
            cur.text = _unescape(text)
            expect_string = False
            cur = None
            continue

        if stripped == "":
            continue
        if stripped.startswith(";"):
            pending_comments.append(stripped[1:].strip())
            continue
        if stripped.startswith("#"):
            parts = stripped[1:].split(None, 1)
            if parts:
                key = parts[0].lower()
                val = parts[1].strip() if len(parts) > 1 else ""
                if key in ("language", "version", "name", "basename"):
                    header["name" if key == "basename" else key] = val
            pending_comments = []
            continue

        # identifier line: IDENT optionally followed by (ID/MinLen/MaxLen)
        ident = stripped
        num = None
        minlen = maxlen = 0
        if "(" in stripped:
            ident = stripped[:stripped.index("(")].strip()
            spec = stripped[stripped.index("(") + 1:]
            if ")" in spec:
                spec = spec[:spec.index(")")]
            fields = spec.split("/")
            if fields and fields[0].strip():
                num = int(fields[0].strip())
            if len(fields) > 1 and fields[1].strip():
                minlen = int(fields[1].strip())
            if len(fields) > 2 and fields[2].strip():
                maxlen = int(fields[2].strip())

        if num is None:
            num = next_id
        next_id = num + 1

        m = Msg(ident, num)
        m.minlen, m.maxlen = minlen, maxlen
        m.comment = pending_comments
        pending_comments = []
        msgs.append(m)
        cur = m
        expect_string = True

    if want_text:
        for m in msgs:
            if m.text is None:
                m.text = b""
    return header, msgs


# --------------------------------------------------------------------------
# C header / default-table generation
# --------------------------------------------------------------------------

def c_escape(b):
    out = []
    for ch in b:
        if ch == 0x22:
            out.append('\\"')
        elif ch == 0x5C:
            out.append("\\\\")
        elif ch == 0x0A:
            out.append("\\n")
        elif ch == 0x09:
            out.append("\\t")
        elif ch == 0x0D:
            out.append("\\r")
        elif ch == 0x1B:
            out.append("\\033")
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)        # octal keeps it byte-exact
    return "".join(out)


def gen_header(cd, out):
    header, msgs = parse_cd(cd)
    count = len(msgs)
    lines = []
    lines.append("/* Auto-generated from %s by support/gencat.py. DO NOT EDIT. */" % cd)
    lines.append("#ifndef DISKPART_STRINGS_H")
    lines.append("#define DISKPART_STRINGS_H")
    lines.append("")
    lines.append("/* Message ids - also the locale.library catalog string numbers. */")
    for m in msgs:
        lines.append("#define %-28s %d" % (m.ident, m.num))
    lines.append("")
    lines.append("#define MSG_COUNT %d" % count)
    lines.append("")
    lines.append("#endif /* DISKPART_STRINGS_H */")
    lines.append("")
    lines.append("/* Built-in (English) defaults - used when no catalog is loaded, e.g.")
    lines.append(" * on Kickstart 2.04 where locale.library is absent.  The table is")
    lines.append(" * indexed directly by message id (ids are contiguous from 0).")
    lines.append(" * Defined out here (past the include guard) so locale_support.c can")
    lines.append(" * emit the one instance with DPSTRINGS_DEFINE_TABLE on a 2nd include. */")
    lines.append("#ifdef DPSTRINGS_DEFINE_TABLE")
    lines.append("#undef DPSTRINGS_DEFINE_TABLE")
    lines.append("const char *const DPStringDefaults[MSG_COUNT] = {")
    # emit indexed so explicit/sparse ids still line up
    by_id = {m.num: m for m in msgs}
    maxid = max(by_id) if by_id else -1
    for idx in range(maxid + 1):
        m = by_id.get(idx)
        if m is None:
            lines.append('    "",')
        else:
            lines.append('    /* %3d %s */ "%s",' % (m.num, m.ident, c_escape(m.text)))
    lines.append("};")
    lines.append("#endif /* DPSTRINGS_DEFINE_TABLE */")
    with open(out, "w", encoding="latin-1") as fh:
        fh.write("\n".join(lines) + "\n")
    sys.stderr.write("gencat: wrote %s (%d strings)\n" % (out, count))


# --------------------------------------------------------------------------
# .ct translation template
# --------------------------------------------------------------------------

def gen_ct(cd, out, lang):
    header, msgs = parse_cd(cd)
    lines = []
    lines.append("## version $VER: %s.catalog %s (%s)"
                 % (header["name"], header["version"], "DD.MM.YYYY"))
    lines.append("## language %s" % lang)
    lines.append("## codeset 0")
    lines.append(";")
    lines.append("; Translation template generated from %s." % cd)
    lines.append("; Translate the line under each identifier; keep %%-placeholders intact.")
    lines.append(";")
    for m in msgs:
        for c in m.comment:
            lines.append("; %s" % c)
        lines.append("%s (%d//)" % (m.ident, m.num))
        lines.append(c_escape(m.text))          # english default as a starting point
        lines.append(";")
    with open(out, "w", encoding="latin-1") as fh:
        fh.write("\n".join(lines) + "\n")
    sys.stderr.write("gencat: wrote %s (%d strings)\n" % (out, len(msgs)))


# --------------------------------------------------------------------------
# .catalog (IFF FORM CTLG) compilation
# --------------------------------------------------------------------------

def _chunk(cid, data):
    out = cid + struct.pack(">I", len(data)) + data
    if len(data) & 1:
        out += b"\x00"                            # IFF chunks are word-aligned
    return out


def gen_catalog(cd, ct, out):
    cd_header, cd_msgs = parse_cd(cd)
    ct_header, ct_msgs = parse_cd(ct)
    lang = ct_header.get("language", "unknown")
    name = cd_header.get("name", "DiskPart")
    ver = cd_header.get("version", "0")

    fver = ("$VER: %s.catalog %s (%s)" % (name, ver, "01.01.2026")).encode("latin-1") + b"\x00"
    langb = lang.encode("latin-1") + b"\x00"
    cset = struct.pack(">I", 0)                   # codeset 0 = ISO-8859-1

    strs = bytearray()
    for m in ct_msgs:
        if m.text is None:
            continue
        s = m.text + b"\x00"
        if len(s) & 1:
            s += b"\x00"
        strs += struct.pack(">II", m.num, len(s))
        strs += s

    body = b"CTLG"
    body += _chunk(b"FVER", fver)
    body += _chunk(b"LANG", langb)
    body += _chunk(b"CSET", cset)
    body += _chunk(b"STRS", bytes(strs))

    form = _chunk(b"FORM", body)
    with open(out, "wb") as fh:
        fh.write(form)
    sys.stderr.write("gencat: wrote %s (%s, %d strings)\n" % (out, lang, len(ct_msgs)))


# --------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "header" and len(sys.argv) == 4:
        gen_header(sys.argv[2], sys.argv[3])
    elif cmd == "ct" and len(sys.argv) in (4, 5):
        gen_ct(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) == 5 else "deutsch")
    elif cmd == "catalog" and len(sys.argv) == 5:
        gen_catalog(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.stderr.write(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
