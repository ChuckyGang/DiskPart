#!/usr/bin/env python3
"""DiskPart grow/shrink fuzz loop (v2).

Per iteration, all randomized:
  geometry     heads in {1,2,4,8,16} x sectors in {32,63} (blocks/cylinder!)
  disk size    up to ~1.2 GB
  partition    random start cylinder + random length
  neighbor     sometimes a second partition after a random gap, filled with
               fingerprinted raw blocks - GROW must clamp at it and never
               touch its content
  filesystem   ffs (real, xdftool, 512-byte blocks)
               ffssyn (synthesized real-layout FFS, 512- or 1024-byte blocks)
               pfs (synth, reserved_blksize 512/1024)
               sfs (synth, blocksize 512/1024/2048)
  content      random files / random-content data blocks (hash-verified)
  ops          GROW (amount or END, clamp-checked) -> verify ->
               SHRINKINFO -> SHRINK (MIN / target size / -amount) -> verify
               -> sometimes a second round
Failures keep fail_itN_seedM.hdf; every iteration logs its seed.

FFS note: grow and shrink both leave bm_flag=0 for the OS validator, which
does not exist under vamos - ffs_revalidate() below rebuilds the bitmap
from the directory tree between operations (parametrized for both 512- and
1024-byte FS blocks).
"""
import os, sys, struct, random, hashlib, subprocess, shutil, time

DP    = "/home/john/Documents/Code/DiskPart/out/DiskPart"
HERE  = os.path.dirname(os.path.abspath(__file__))
LOG   = os.path.join(HERE, "testloop.log")

def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f: f.write(line + "\n")

def run_dp(args):
    r = subprocess.run(["vamos", "-q", "-O", "locale.library=mode:off", DP] + args,
                       capture_output=True, text=True, timeout=300, cwd=HERE)
    return r.returncode, r.stdout + r.stderr

def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=HERE)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout

class Img:
    """Partition-relative block access (dev = 512-byte device blocks)."""
    def __init__(self, path, pbase): self.path, self.pbase = path, pbase
    def rd(self, blk, n=512):
        with open(self.path, "rb") as f:
            f.seek((self.pbase + blk) * 512); return f.read(n)
    def wr(self, blk, data):
        with open(self.path, "r+b") as f:
            f.seek((self.pbase + blk) * 512); f.write(data)

def rd_longs(img, fsblk, spb):
    return list(struct.unpack(f">{128*spb}I", img.rd(fsblk * spb, 512 * spb)))
def wr_longs(img, fsblk, spb, L):
    img.wr(fsblk * spb, struct.pack(f">{len(L)}I", *L))

def ffs_cksum(L, ckidx=5):
    s = 0
    for i, v in enumerate(L):
        if i != ckidx: s = (s + v) & 0xFFFFFFFF
    return (-s) & 0xFFFFFFFF

# ------------------------------------------------------- FFS (both sizes)
def ffs_revalidate(img, blocks, spb):
    """Rebuild the bitmap from the tree + set bm_flag=VALID (OS-validator
    stand-in).  nlongs-parametrized: works for 512 (spb=1) and 1024 (spb=2)."""
    nl = 128 * spb
    HT, BMF, BMP, BME = nl - 56, nl - 50, nl - 49, nl - 24
    HCH, EXT, SEC = nl - 4, nl - 2, nl - 1
    boot = rd_longs(img, 0, spb)
    root_blk = boot[2] if 0 < boot[2] < blocks else (2 + blocks - 1) // 2
    used, bm_list = set(), []
    root = rd_longs(img, root_blk, spb); used.add(root_blk)
    if root[0] != 2 or root[SEC] != 1:
        raise RuntimeError(f"revalidate: bad root at {root_blk}")
    for i in range(25):
        p = root[BMP + i]
        if p == 0: break
        bm_list.append(p); used.add(p)
    ext, guard = root[BME], 0
    while ext and guard < 64:
        guard += 1
        eb = rd_longs(img, ext, spb); used.add(ext)
        for s in range(nl - 1):
            if eb[s] == 0: break
            bm_list.append(eb[s]); used.add(eb[s])
        ext = eb[nl - 1]

    def walk(dirblk, depth=0):
        if depth > 16: return
        d = rd_longs(img, dirblk, spb)
        for i in range(6, 6 + HT):
            b, g = d[i], 0
            while b and g < 8192:
                g += 1
                if b in used: break
                h = rd_longs(img, b, spb); used.add(b)
                if h[0] != 2 or h[1] != b: break
                if h[SEC] == 2:
                    walk(b, depth + 1)
                elif h[SEC] == 0xFFFFFFFD:
                    fh, fg = h, 0
                    while fg < 8192:
                        fg += 1
                        for j in range(6, 6 + HT):
                            if fh[j]: used.add(fh[j])
                        nxt = fh[EXT]
                        if not nxt: break
                        fh = rd_longs(img, nxt, spb); used.add(nxt)
                b = h[HCH]
    walk(root_blk)

    bpbm, reserved = (nl - 1) * 32, 2
    need = (bpbm - 2 + blocks - reserved) // bpbm
    if len(bm_list) < need:
        raise RuntimeError(f"revalidate: bm chain {len(bm_list)} < {need}")
    for k in range(need):
        L = [0] * nl
        base = reserved + k * bpbm
        for off in range(bpbm):
            blkno = base + off
            if blkno >= blocks: break
            if blkno not in used:
                L[1 + off // 32] |= 1 << (off % 32)
        s = sum(L[1:]) & 0xFFFFFFFF
        L[0] = (-s) & 0xFFFFFFFF
        wr_longs(img, bm_list[k], spb, L)
    root[BMF] = 0xFFFFFFFF
    root[5] = 0; root[5] = ffs_cksum(root)
    wr_longs(img, root_blk, spb, root)

def build_ffs_real(path, rng, cyls, blks_cyl, pbase, dosn):
    vol = os.path.join(HERE, "fuzzvol.hdf")
    if os.path.exists(vol): os.remove(vol)
    heads, secs = 1, blks_cyl
    while secs > 63: heads *= 2; secs //= 2
    sh(f"xdftool {vol} create chs={cyls},{heads},{secs} + format Fuzz DOS{dosn} >/dev/null")
    files, nfiles = {}, rng.randint(2, 6)
    sh(f"xdftool {vol} makedir sub >/dev/null")
    for i in range(nfiles):
        data = rng.randbytes(rng.randint(1024, 400_000))
        fn = f"f{i}.bin"; tmp = os.path.join(HERE, fn)
        open(tmp, "wb").write(data)
        dest = fn if i % 2 == 0 else f"sub/{fn}"
        sh(f"xdftool {vol} write {fn} {dest} >/dev/null")
        os.remove(tmp)
        files[dest] = hashlib.md5(data).hexdigest()
    sh(f"dd if={vol} of={path} bs=512 seek={pbase} conv=notrunc status=none")
    os.remove(vol)
    return files

def verify_ffs_real(path, cyls, blks_cyl, pbase, files):
    vol = os.path.join(HERE, "fuzzchk.hdf")
    sh(f"dd if={path} of={vol} bs=512 skip={pbase} count={cyls*blks_cyl} status=none")
    out = os.path.join(HERE, "chkout")
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    errs = []
    for dest, md5 in files.items():
        base = os.path.basename(dest)
        try:
            sh(f"cd {out} && xdftool ../fuzzchk.hdf read {dest} >/dev/null")
            got = hashlib.md5(open(os.path.join(out, base), "rb").read()).hexdigest()
            if got != md5: errs.append(f"content mismatch {dest}")
        except Exception as e:
            errs.append(f"read failed {dest}: {e}")
    os.remove(vol); shutil.rmtree(out, ignore_errors=True)
    return errs

def build_ffs_syn(img, rng, blocks_dev, spb):
    """Synthesize a real-layout FFS volume (512- or 1024-byte blocks):
    boot (DOS\\3 + root ptr), root at the computed centre, bm chain, files
    with proper headers/data tables/extension blocks, valid bitmap."""
    nl = 128 * spb
    HT, BMF, BMP, BME = nl - 56, nl - 50, nl - 49, nl - 24
    EXT, SEC = nl - 2, nl - 1
    blocks = blocks_dev // spb
    reserved, bpbm = 2, (nl - 1) * 32
    root_blk = (reserved + blocks - 1) // 2
    used = {root_blk}
    cursor = [root_blk + 1]
    def alloc():
        while cursor[0] in used: cursor[0] += 1
        b = cursor[0]; used.add(b); cursor[0] += 1
        return b
    root = [0] * nl
    root[0] = 2; root[3] = HT; root[SEC] = 1
    root[nl - 20] = struct.unpack(">I", b"\x04Fuzz"[:4])[0]
    files = {}
    for i in range(rng.randint(2, 5)):
        size = rng.randint(1024, 300_000)
        data = rng.randbytes(size)
        nblk_f = (size + 512 * spb - 1) // (512 * spb)
        dblks = [alloc() for _ in range(nblk_f)]
        bsz = 512 * spb
        for j, db in enumerate(dblks):
            chunk = data[j*bsz:(j+1)*bsz]
            img.wr(db * spb, chunk + b"\0" * (bsz - len(chunk)))
        fh_blk = alloc()
        name = f"g{i}.bin"
        hsh = len(name)
        for c in name.upper(): hsh = ((hsh * 13 + ord(c)) & 0x7FF)
        bucket = 6 + (hsh % HT)
        remaining = list(dblks)
        cur_blk, first = fh_blk, True
        while True:
            hdr = [0] * nl
            hdr[0] = 2; hdr[1] = cur_blk
            take = remaining[:HT]; remaining = remaining[len(take):]
            for j, db in enumerate(take):        # table filled from the end
                hdr[6 + HT - 1 - j] = db
            if first:
                hdr[2] = len(take)
                hdr[4] = take[0] if take else 0
                hdr[nl - 47] = size
                bn = bytes([len(name)]) + name.encode()
                for k in range(0, len(bn), 4):
                    hdr[nl - 20 + k // 4] = struct.unpack(
                        ">I", (bn[k:k+4] + b"\0" * 4)[:4])[0]
                hdr[nl - 3] = root_blk
                hdr[SEC] = 0xFFFFFFFD
            else:
                hdr[SEC] = 0x00000010            # ST_LIST extension
                hdr[nl - 3] = fh_blk
            nxt = 0
            if remaining:
                nxt = alloc(); hdr[EXT] = nxt
            hdr[5] = 0; hdr[5] = ffs_cksum(hdr)
            wr_longs(img, cur_blk, spb, hdr)
            if not remaining: break
            cur_blk, first = nxt, False
        root[bucket] = fh_blk
        files[name] = (dblks, size, hashlib.md5(data).hexdigest())
    need = (bpbm - 2 + blocks - reserved) // bpbm
    bm_list = [alloc() for _ in range(need)]
    for i, b in enumerate(bm_list[:25]): root[BMP + i] = b
    if need > 25:
        rem = bm_list[25:]
        chunks = [rem[i:i + nl - 1] for i in range(0, len(rem), nl - 1)]
        ext_blks = [alloc() for _ in chunks]
        root[BME] = ext_blks[0]
        for ci, chunk in enumerate(chunks):
            eb = [0] * nl
            for i, b in enumerate(chunk): eb[i] = b
            eb[nl - 1] = ext_blks[ci + 1] if ci + 1 < len(ext_blks) else 0
            wr_longs(img, ext_blks[ci], spb, eb)
    for k in range(need):
        L = [0] * nl
        base = reserved + k * bpbm
        for off in range(bpbm):
            blkno = base + off
            if blkno >= blocks: break
            if blkno not in used:
                L[1 + off // 32] |= 1 << (off % 32)
        s = sum(L[1:]) & 0xFFFFFFFF
        L[0] = (-s) & 0xFFFFFFFF
        wr_longs(img, bm_list[k], spb, L)
    root[BMF] = 0xFFFFFFFF
    root[5] = 0; root[5] = ffs_cksum(root)
    wr_longs(img, root_blk, spb, root)
    boot = [0] * nl
    boot[0] = 0x444F5303; boot[2] = root_blk
    wr_longs(img, 0, spb, boot)
    return files

def verify_ffs_syn(img, files, spb):
    errs = []
    bsz = 512 * spb
    for name, (dblks, size, md5) in files.items():
        h, left = hashlib.md5(), size
        for db in dblks:
            take = min(bsz, left)
            h.update(img.rd(db * spb, bsz)[:take]); left -= take
        if h.hexdigest() != md5: errs.append(f"content mismatch {name}")
    return errs

# ------------------------------------------------------- synthetic PFS3
def build_pfs(img, rng, nblk, rbsz):
    lpb, rescl = rbsz // 4 - 3, rbsz // 512
    cov = lpb * 32
    lastres, bstart = 200, 201
    nbmb = ((nblk - bstart) + cov - 1) // cov
    nidx = (nbmb + lpb - 1) // lpb
    idx_pos = [100 + rescl * 2 * i for i in range(nidx)]
    bm_base = 100 + rescl * 2 * nidx + rescl * 2
    bm_pos = [bm_base + rescl * 2 * i for i in range(nbmb)]
    lastres = max(200, bm_pos[-1] + rescl + 4) if bm_pos else 200
    bstart = lastres + 1
    nbmb = ((nblk - bstart) + cov - 1) // cov          # recompute w/ new bstart
    nidx = (nbmb + lpb - 1) // lpb
    idx_pos = [100 + rescl * 2 * i for i in range(nidx)]
    bm_pos = bm_pos[:nbmb]
    used = set()
    for _ in range(rng.randint(1, 4)):
        a = rng.randint(bstart, nblk - 2)
        used.update(range(a, a + rng.randint(1, min(3000, nblk - 1 - a))))
    dh = hashlib.md5()
    for b in sorted(used):
        c = rng.randbytes(512); img.wr(b, c); dh.update(c)
    free = (nblk - bstart) - len(used)
    rb = bytearray(rbsz)
    opts = (128 | 16) if rng.random() < 0.8 else 128
    struct.pack_into(">I", rb, 0, 0x50465301); struct.pack_into(">I", rb, 4, opts)
    struct.pack_into(">I", rb, 52, lastres);   struct.pack_into(">I", rb, 56, 2)
    struct.pack_into(">I", rb, 60, rng.choice([60, 8000]))  # reserved_free
    struct.pack_into(">H", rb, 64, rbsz);      struct.pack_into(">H", rb, 66, rescl)
    struct.pack_into(">I", rb, 68, free)
    struct.pack_into(">I", rb, 76, rng.randint(0, nbmb * lpb - 1))
    struct.pack_into(">I", rb, 84, nblk)
    for i, p in enumerate(idx_pos):
        struct.pack_into(">I", rb, 96 + 4 * i, p)
    img.wr(2, bytes(rb))
    for n, ipos in enumerate(idx_pos):
        ib = bytearray(rbsz)
        struct.pack_into(">H", ib, 0, 0x4D49); struct.pack_into(">I", ib, 8, n)
        for i, p in enumerate(bm_pos[n*lpb:(n+1)*lpb]):
            struct.pack_into(">I", ib, 12 + 4 * i, p)
        img.wr(ipos, bytes(ib))
    for n, p in enumerate(bm_pos):
        bm = bytearray(rbsz)
        struct.pack_into(">H", bm, 0, 0x424D); struct.pack_into(">I", bm, 8, n)
        for m in range(lpb):
            v = 0
            for j in range(32):
                blk = bstart + (n * lpb + m) * 32 + j
                if blk not in used:      # out-of-range bits FREE, like real pfs3
                    v |= 1 << (31 - j)
            struct.pack_into(">I", bm, 12 + 4 * m, v)
        img.wr(p, bytes(bm))
    return {"used": used, "hash": dh.hexdigest(), "rbsz": rbsz, "bstart": bstart}

def verify_pfs(img, st, exp_nblk):
    errs = []
    rb = img.rd(2, st["rbsz"])
    g = lambda o: struct.unpack_from(">I", rb, o)[0]
    if g(84) != exp_nblk: errs.append(f"disksize {g(84)} != {exp_nblk}")
    exp_free = (exp_nblk - st["bstart"]) - len(st["used"])
    if g(68) != exp_free: errs.append(f"blocksfree {g(68)} != {exp_free}")
    h = hashlib.md5()
    for b in sorted(st["used"]): h.update(img.rd(b))
    if h.hexdigest() != st["hash"]: errs.append("data content changed")
    return errs

# ------------------------------------------------------- synthetic SFS
def sfs_ck(b, bsz):
    b = bytearray(b); struct.pack_into(">I", b, 4, 0)
    acc = 1
    for i in range(0, bsz, 4):
        acc = (acc + struct.unpack_from(">I", b, i)[0]) & 0xFFFFFFFF
    struct.pack_into(">I", b, 4, (-acc) & 0xFFFFFFFF)
    return bytes(b)

def build_sfs(img, rng, nblk_dev, bsz):
    spb = bsz // 512
    nblk = nblk_dev // spb
    bib = (bsz - 12) * 8
    nbmb = (nblk + bib - 1) // bib
    bmbase = 2
    admin = bmbase + nbmb + 40
    rootobj = admin + 1
    used = set([0, 1]) | set(range(bmbase, bmbase + nbmb)) | \
           {admin, rootobj, admin + 2, admin + 3}
    for _ in range(rng.randint(1, 4)):
        a = rng.randint(rootobj + 4, nblk - 2)
        used.update(range(a, a + rng.randint(1, min(2000, nblk - 1 - a))))
    used.add(nblk - 1)
    data = {b for b in used if b > rootobj + 3 and b != nblk - 1}
    dh = hashlib.md5()
    for b in sorted(data):
        c = rng.randbytes(bsz); img.wr(b * spb, c); dh.update(c)
    def mkroot(own, seq):
        r = bytearray(bsz)
        struct.pack_into(">I", r, 0, 0x53465300); struct.pack_into(">I", r, 8, own)
        struct.pack_into(">H", r, 12, 3); struct.pack_into(">H", r, 14, seq)
        struct.pack_into(">I", r, 36, img.pbase * 512)
        struct.pack_into(">I", r, 44, img.pbase * 512 + nblk * bsz)
        struct.pack_into(">I", r, 48, nblk); struct.pack_into(">I", r, 52, bsz)
        struct.pack_into(">I", r, 96, bmbase); struct.pack_into(">I", r, 100, admin)
        struct.pack_into(">I", r, 104, rootobj)
        struct.pack_into(">I", r, 108, admin + 2); struct.pack_into(">I", r, 112, admin + 3)
        return sfs_ck(r, bsz)
    img.wr(0, mkroot(0, 7)); img.wr((nblk - 1) * spb, mkroot(nblk - 1, 7))
    for k in range(nbmb):
        bm = bytearray(bsz)
        struct.pack_into(">I", bm, 0, 0x42544D50)
        struct.pack_into(">I", bm, 8, bmbase + k)
        for m in range(bib // 32):
            v = 0
            for j in range(32):
                blk = k * bib + m * 32 + j
                if blk < nblk and blk not in used:   # SFS slack stays USED
                    v |= 1 << (31 - j)
            struct.pack_into(">I", bm, 12 + 4 * m, v)
        img.wr((bmbase + k) * spb, sfs_ck(bm, bsz))
    ob = bytearray(bsz)
    struct.pack_into(">I", ob, 0, 0x4F424A43); struct.pack_into(">I", ob, 8, rootobj)
    struct.pack_into(">I", ob, bsz - 36 + 8, nblk - len(used))
    img.wr(rootobj * spb, sfs_ck(ob, bsz))
    return {"bsz": bsz, "spb": spb, "data": data, "hash": dh.hexdigest()}

def verify_sfs(img, st, exp_nblk_dev):
    # The engine derives blocks/cylinder as totalblocks//ncyl - with odd
    # geometries (e.g. 63 sectors x 1024-byte blocks = 31.5 blocks/cyl) it
    # floors, so the FS stays conservatively SMALLER than the envelope.
    errs = []
    bsz, spb = st["bsz"], st["spb"]
    exp_env = exp_nblk_dev // spb
    r0 = img.rd(0, bsz)
    g = lambda b, o: struct.unpack_from(">I", b, o)[0]
    tb = g(r0, 48)
    if not (exp_env - 4096 <= tb <= exp_env):
        errs.append(f"totalblocks {tb} not in ({exp_env-4096}..{exp_env})")
    re = img.rd((tb - 1) * spb, bsz)
    if g(re, 0) != 0x53465300 or g(re, 8) != tb - 1:
        errs.append("end root bad")
    h = hashlib.md5()
    for b in sorted(st["data"]): h.update(img.rd(b * spb, bsz))
    if h.hexdigest() != st["hash"]: errs.append("data content changed")
    return errs

# --------------------------------------------------------------- iteration
def parse_min_high(out):
    for ln in out.splitlines():
        if "HighCyl >=" in ln:
            return int(ln.split("HighCyl >=")[1].strip(" )."))
    return None

def iteration(it, seed):
    rng = random.Random(seed)
    fs = rng.choice(["ffs", "ffssyn", "pfs", "sfs"])
    heads   = rng.choice([1, 2, 4, 8, 16])
    sectors = 32 if fs == "ffs" else rng.choice([32, 63])
    # (real-xdftool FFS: amitools miscomputes the root position on
    #  63-sector geometries - synthetic FFS covers those instead)
    blks_cyl = heads * sectors
    cylb = blks_cyl * 512
    max_cyls = min(6000, (1200 * 1024 * 1024) // cylb)
    disk_cyls = rng.randint(min(900, max_cyls - 1), max_cyls)
    lo = rng.randint(1, max(1, disk_cyls // 8))
    part_cyls = rng.randint(max(64, 3_000_000 // cylb),
                            max(128, (disk_cyls - lo) // 2))
    if fs == "ffs":                     # xdftool practical volume limit
        part_cyls = min(part_cyls, max(64, 300 * 1024 * 1024 // cylb))
    hi = lo + part_cyls - 1
    pbase = lo * blks_cyl
    img_path = os.path.join(HERE, "fuzz.hdf")
    tag = (f"it{it} seed={seed} fs={fs} geo={heads}x{sectors}"
           f" disk={disk_cyls}c part={lo}-{hi}({part_cyls}c)")

    neighbor = None
    if rng.random() < 0.5 and disk_cyls - hi > 40:
        gap = rng.randint(1, max(1, (disk_cyls - hi) // 3))
        nlo = hi + gap + 1
        nhi = min(disk_cyls - 1, nlo + rng.randint(32, 400))
        if nhi > nlo:
            neighbor = (nlo, nhi)

    dosn = rng.choice([0, 1, 2, 3]) if fs == "ffs" else 3   # OFS/FFS/intl mix
    syn_spb = rng.choice([1, 2]) if fs == "ffssyn" else 1
    dostype = {"ffs": f"DOS{dosn}", "ffssyn": "DOS3", "pfs": "PDS3", "sfs": "SFS0"}[fs]
    if os.path.exists(img_path): os.remove(img_path)
    cmd = (f"rdbtool {img_path} create chs={disk_cyls},{heads},{sectors} + init "
           f"+ add name=DH0 start={lo} end={hi} dostype={dostype}")
    if neighbor:
        cmd += f" + add name=DH1 start={neighbor[0]} end={neighbor[1]} dostype=DOS3"
    sh(cmd + " >/dev/null")

    if fs == "ffssyn" and syn_spb == 2:
        # rdbtool cannot set DE_SECSPERBLK - patch DH0's PART block directly
        with open(img_path, "r+b") as f:
            raw = f.read(16 * 512)
            for blkno in range(16):
                b = raw[blkno*512:(blkno+1)*512]
                if b[:4] == b"PART" and b"\x03DH0" in b[32:76]:
                    L = bytearray(b)
                    struct.pack_into(">I", L, 128 + 4 * 4, 2)  # de_SectorPerBlock
                    struct.pack_into(">I", L, 8, 0)
                    n = struct.unpack_from(">I", L, 4)[0]
                    s = sum(struct.unpack_from(f">{n}I", L, 0)) & 0xFFFFFFFF
                    struct.pack_into(">I", L, 8, (-s) & 0xFFFFFFFF)
                    f.seek(blkno * 512); f.write(L)
                    break
            else:
                raise RuntimeError("DH0 PART block not found")

    img = Img(img_path, pbase)
    if fs == "ffs":
        st = build_ffs_real(img_path, rng, part_cyls, blks_cyl, pbase, dosn)
        bs = 512
        tag += f" DOS{dosn}"
    elif fs == "ffssyn":
        st = build_ffs_syn(img, rng,
                           (part_cyls * blks_cyl) & ~(syn_spb - 1) if syn_spb > 1
                           else part_cyls * blks_cyl, syn_spb)
        bs = 512 * syn_spb
    elif fs == "pfs":
        bs = rng.choice([512, 1024])
        st = build_pfs(img, rng, part_cyls * blks_cyl, bs)
    else:
        bs = rng.choice([512, 1024, 2048])
        st = build_sfs(img, rng, part_cyls * blks_cyl, bs)
    tag += f" bs={bs}" + (f" nbr={neighbor[0]}-{neighbor[1]}" if neighbor else "")

    nbr_hash = None
    if neighbor:
        nbi = Img(img_path, neighbor[0] * blks_cyl)
        h = hashlib.md5()
        for i in range(64):
            c = rng.randbytes(512); nbi.wr(i, c); h.update(c)
        nbr_hash = h.hexdigest()

    def verify(cyls):
        if fs == "ffs":    return verify_ffs_real(img_path, cyls, blks_cyl, pbase, st)
        if fs == "ffssyn": return verify_ffs_syn(img, st, syn_spb)
        if fs == "pfs":   return verify_pfs(img, st, cyls * blks_cyl)
        return verify_sfs(img, st, cyls * blks_cyl)

    def revalidate(cyls):
        if fs == "ffs":    ffs_revalidate(img, cyls * blks_cyl, 1)
        if fs == "ffssyn": ffs_revalidate(img, (cyls * blks_cyl) // syn_spb, syn_spb)

    gap_max = (neighbor[0] - 1) if neighbor else (disk_cyls - 1)
    cur_hi = hi
    rounds = 2 if rng.random() < 0.4 else 1
    for rnd in range(rounds):
        room = gap_max - cur_hi
        if room < 1: break
        grow_cyls = rng.randint(1, disk_cyls)      # may exceed room: clamp test
        use_end = rng.random() < 0.2
        garg = "END" if use_end else str(grow_cyls * cylb)
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "GROW", "DH0", garg, "FORCE"])
        gtries = 0
        while "holds data" in out and gtries < 4:
            gtries += 1
            grow_cyls = rng.randint(1, room); use_end = False
            garg = str(grow_cyls * cylb)
            rc, out = run_dp([f"IMAGE=fuzz.hdf", "GROW", "DH0", garg, "FORCE"])
        while "reserved area too small" in out and gtries < 4:
            gtries += 1
            grow_cyls = max(1, rng.randint(1, max(1, room // 4))); use_end = False
            garg = str(grow_cyls * cylb)
            rc, out = run_dp([f"IMAGE=fuzz.hdf", "GROW", "DH0", garg, "FORCE"])
        if "holds data" in out:
            tag += " grow[rootbusy-skip]"; break
        if "reserved area too small" in out:
            tag += " grow[resv-skip]"; break
        if "grown" not in out:
            return tag + f" grow={garg}", [f"GROW failed rc={rc}: " + out.strip()[-200:]]
        new_hi = gap_max if use_end else min(cur_hi + grow_cyls, gap_max)
        new_cyls = new_hi - lo + 1
        tag += f" grow=+{new_hi - cur_hi}c"

        revalidate(new_cyls)
        errs = verify(new_cyls)
        if errs: return tag, ["after-grow: " + e for e in errs]
        info = sh(f"rdbtool fuzz.hdf info | grep \"'DH0'\"")
        if int(info.split()[4]) != new_hi:
            return tag, [f"grow RDB high {info.split()[4]} != {new_hi}"]

        rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINKINFO=DH0"])
        min_high = parse_min_high(out)
        if min_high is None:
            return tag, ["SHRINKINFO gave no floor: " + out.strip()[-200:]]
        if min_high >= new_hi:
            tag += " shrink[at-min-skip]"; cur_hi = new_hi; continue

        mode = rng.choice(["MIN", "target", "rel"])
        if mode == "MIN":
            sarg, exp_hi = "MIN", min_high
        elif mode == "target":
            t = rng.randint(min_high, new_hi - 1)
            sarg, exp_hi = str((t - lo + 1) * cylb), t
        else:
            rem = rng.randint(1, new_hi - min_high)
            sarg, exp_hi = f"-{rem * cylb}", new_hi - rem
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINK=DH0", f"SIZE={sarg}", "FORCE"])
        tries = 0
        while "holds data" in out and tries < 4 and min_high < new_hi - 1:
            tries += 1
            t = rng.randint(min_high, new_hi - 1)
            sarg, exp_hi = str((t - lo + 1) * cylb), t
            rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINK=DH0", f"SIZE={sarg}", "FORCE"])
        if "holds data" in out:
            tag += " shrink[rootbusy-skip]"; cur_hi = new_hi; continue
        if "bitmap lies above" in out:
            tag += " shrink[bmhigh-skip]"; cur_hi = new_hi; continue
        if "shrunk" not in out:
            return tag + f" shrink={sarg}", [f"SHRINK failed rc={rc}: " + out.strip()[-250:]]
        tag += f" shrink[{mode}{'+r' if tries else ''}]->{exp_hi - lo + 1}c"

        info = sh(f"rdbtool fuzz.hdf info | grep \"'DH0'\"")
        if int(info.split()[4]) != exp_hi:
            return tag, [f"RDB high_cyl {info.split()[4]} != {exp_hi}"]
        final_cyls = exp_hi - lo + 1
        errs = verify(final_cyls)
        if errs: return tag, ["after-shrink: " + e for e in errs]
        revalidate(final_cyls)
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINKINFO=DH0"])
        if "Shrink report" not in out:
            return tag, ["post-shrink SHRINKINFO failed: " + out.strip()[-200:]]
        cur_hi = exp_hi

    if neighbor:
        nbi = Img(img_path, neighbor[0] * blks_cyl)
        h = hashlib.md5()
        for i in range(64): h.update(nbi.rd(i))
        if h.hexdigest() != nbr_hash:
            return tag, ["NEIGHBOR PARTITION CONTENT CHANGED"]
    return tag, []

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    base = int(sys.argv[2]) if len(sys.argv) > 2 else random.randrange(1 << 30)
    log(f"=== testloop v2 start: {n} iterations, base seed {base} ===")
    fails = 0
    for it in range(n):
        seed = base + it
        try:
            tag, errs = iteration(it, seed)
        except Exception as e:
            tag, errs = f"it{it} seed={seed}", [f"harness exception: {e!r}"]
        if errs:
            fails += 1
            log(f"FAIL {tag}")
            for e in errs: log(f"     {e}")
            keep = os.path.join(HERE, f"fail_it{it}_seed{seed}.hdf")
            try: shutil.copy(os.path.join(HERE, "fuzz.hdf"), keep)
            except Exception: pass
        else:
            log(f"PASS {tag}")
    log(f"=== done: {n - fails}/{n} passed ===")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
