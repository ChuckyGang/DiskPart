#!/usr/bin/env python3
"""DiskPart grow/shrink fuzz loop.

Per iteration: random FS (ffs/pfs/sfs), random FS blocksize, random disk +
partition geometry, random content -> GROW (random amount) -> [FFS: rebuild
bitmap like the OS validator would] -> SHRINK (random form: MIN / target /
-relative) -> verify content + structure -> discard (keep artifacts on FAIL).

FFS uses real xdftool-formatted volumes with real files (md5-checked).
PFS3/SFS use structurally-correct synthesized volumes whose used data blocks
carry random content (hash-checked); grow/shrink must never touch them.
"""
import os, sys, struct, random, hashlib, subprocess, shutil, time

DP    = "/home/john/Documents/Code/DiskPart/out/DiskPart"
HERE  = os.path.dirname(os.path.abspath(__file__))
LOG   = os.path.join(HERE, "testloop.log")
PBASE = 32          # partition start block (cyl 1, 1 head x 32 sectors)
CYLB  = 32 * 512    # bytes per cylinder

def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")

def run_dp(args):
    r = subprocess.run(
        ["vamos", "-q", "-O", "locale.library=mode:off", DP] + args,
        capture_output=True, text=True, timeout=300, cwd=HERE)
    return r.returncode, r.stdout + r.stderr

def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=HERE)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout

# ---------------------------------------------------------------- image I/O
class Img:
    def __init__(self, path): self.path = path
    def rd(self, blk, n=512):
        with open(self.path, "rb") as f:
            f.seek((PBASE + blk) * 512); return f.read(n)
    def wr(self, blk, data):
        with open(self.path, "r+b") as f:
            f.seek((PBASE + blk) * 512); f.write(data)
    def rd_longs(self, blk):
        return list(struct.unpack(">128I", self.rd(blk)))
    def wr_longs(self, blk, L):
        self.wr(blk, struct.pack(">128I", *L))

def ffs_cksum(L):
    s = 0
    for i, v in enumerate(L):
        if i != 5: s = (s + v) & 0xFFFFFFFF
    return (-s) & 0xFFFFFFFF

# ------------------------------------------- FFS validator (bitmap rebuild)
def ffs_revalidate(img, blocks):
    """Rebuild the FFS bitmap from the directory tree and set bm_flag=VALID -
    what the OS validator does after our grow/shrink left bm_flag=0."""
    boot = img.rd_longs(0)
    root_blk = boot[2] if 0 < boot[2] < blocks else blocks // 2
    used = set()
    root = img.rd_longs(root_blk); used.add(root_blk)
    if root[0] != 2 or root[127] != 1:
        raise RuntimeError(f"revalidate: bad root at {root_blk}")
    bm_list = []
    for i in range(25):
        p = root[79 + i]
        if p == 0: break
        bm_list.append(p); used.add(p)
    ext = root[104]
    guard = 0
    while ext and guard < 64:
        guard += 1
        eb = img.rd_longs(ext); used.add(ext)
        for s in range(127):
            if eb[s] == 0: break
            bm_list.append(eb[s]); used.add(eb[s])
        ext = eb[127]

    def walk(dirblk, depth=0):
        if depth > 16: return
        d = img.rd_longs(dirblk)
        for i in range(6, 78):
            b = d[i]; g = 0
            while b and g < 4096:
                g += 1
                if b in used: break
                h = img.rd_longs(b); used.add(b)
                if h[0] != 2 or h[1] != b: break
                sec = h[127]
                if sec == 2:
                    walk(b, depth + 1)
                elif sec == 0xFFFFFFFD:            # ST_FILE
                    fh, fg = h, 0
                    while fg < 4096:
                        fg += 1
                        for j in range(6, 78):     # data-block table
                            if fh[j]: used.add(fh[j])
                        nxt = fh[126]              # fh_Extension
                        if not nxt: break
                        fh = img.rd_longs(nxt); used.add(nxt)
                b = h[124]                          # hash chain
    walk(root_blk)

    bpbm, reserved = 127 * 32, 2
    need = (bpbm - 2 + blocks - reserved) // bpbm
    if len(bm_list) < need:
        raise RuntimeError(f"revalidate: bm chain {len(bm_list)} < need {need}")
    for k in range(need):
        L = [0] * 128
        base = reserved + k * bpbm
        for off in range(bpbm):
            blkno = base + off
            if blkno >= blocks: break
            if blkno not in used:
                L[1 + off // 32] |= 1 << (off % 32)
        L[0] = 0; L[0] = ffs_cksum([0] + L[1:]) if False else 0
        # checksum: whole-block sum == 0 with L[0] as the checksum field
        s = sum(L[1:]) & 0xFFFFFFFF
        L[0] = (-s) & 0xFFFFFFFF
        img.wr_longs(bm_list[k], L)
    root[78] = 0xFFFFFFFF                           # bm_flag = VALID
    root[5]  = 0; root[5] = ffs_cksum(root)
    img.wr_longs(root_blk, root)
    return len(used)

# ---------------------------------------------------------------- builders
def build_rdb(path, disk_cyls, lo, hi, dostype):
    if os.path.exists(path): os.remove(path)
    sh(f"rdbtool {path} create chs={disk_cyls},1,32 + init "
       f"+ add name=DH0 start={lo} end={hi} dostype={dostype} >/dev/null")

def build_ffs(path, rng, lo, hi):
    cyls = hi - lo + 1
    vol = os.path.join(HERE, "fuzzvol.hdf")   # xdftool needs a .hdf suffix
    if os.path.exists(vol): os.remove(vol)
    sh(f"xdftool {vol} create chs={cyls},1,32 + format Fuzz ffs >/dev/null")
    files = {}
    nfiles = rng.randint(2, 6)
    sh(f"xdftool {vol} makedir sub >/dev/null")
    for i in range(nfiles):
        data = bytes(rng.getrandbits(8) for _ in range(rng.randint(1024, 400_000)))
        fn = f"f{i}.bin"
        tmp = os.path.join(HERE, fn)
        with open(tmp, "wb") as f: f.write(data)
        dest = fn if i % 2 == 0 else f"sub/{fn}"
        sh(f"xdftool {vol} write {fn} {dest} >/dev/null")
        os.remove(tmp)
        files[dest] = hashlib.md5(data).hexdigest()
    sh(f"dd if={vol} of={path} bs=512 seek={PBASE} conv=notrunc status=none")
    os.remove(vol)
    return files

def verify_ffs(path, cyls, files):
    vol = os.path.join(HERE, "fuzzchk.hdf")   # xdftool needs a .hdf suffix
    sh(f"dd if={path} of={vol} bs=512 skip={PBASE} count={cyls*32} status=none")
    out = os.path.join(HERE, "chkout")
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
    errs = []
    for dest, md5 in files.items():
        base = os.path.basename(dest)
        try:
            sh(f"cd {out} && xdftool ../{os.path.basename(vol)} read {dest} >/dev/null")
            got = hashlib.md5(open(os.path.join(out, base), "rb").read()).hexdigest()
            if got != md5: errs.append(f"content mismatch {dest}")
        except Exception as e:
            errs.append(f"read failed {dest}: {e}")
    os.remove(vol); shutil.rmtree(out, ignore_errors=True)
    return errs

# ---- synthetic PFS3 ----
def build_pfs(img, rng, nblk, rbsz):
    lpb  = rbsz // 4 - 3
    cov  = lpb * 32
    lastres = 200
    bstart  = lastres + 1
    nbmb = ((nblk - bstart) + cov - 1) // cov
    rescl = rbsz // 512
    idx_blk = 100
    bm_pos = [110 + rescl * 2 * i for i in range(nbmb)]
    # random used data ranges + random content
    used = set()
    nranges = rng.randint(1, 4)
    for _ in range(nranges):
        a = rng.randint(bstart, nblk - 2)
        ln = rng.randint(1, min(3000, nblk - 1 - a))
        used.update(range(a, a + ln))
    data_hash = hashlib.md5()
    for b in sorted(used):
        content = bytes(rng.getrandbits(8) for _ in range(512))
        img.wr(b, content); data_hash.update(content)
    free = (nblk - bstart) - len(used)
    rb = bytearray(rbsz)
    opts = (128 | 16) if rng.random() < 0.8 else 128
    struct.pack_into(">I", rb, 0, 0x50465301)
    struct.pack_into(">I", rb, 4, opts)
    struct.pack_into(">I", rb, 52, lastres)
    struct.pack_into(">I", rb, 56, 2)
    struct.pack_into(">I", rb, 60, 60)
    struct.pack_into(">H", rb, 64, rbsz)
    struct.pack_into(">H", rb, 66, rescl)
    struct.pack_into(">I", rb, 68, free)
    struct.pack_into(">I", rb, 76, rng.randint(0, nbmb * lpb - 1))
    struct.pack_into(">I", rb, 84, nblk)
    struct.pack_into(">I", rb, 96, idx_blk)
    img.wr(2, bytes(rb))
    ib = bytearray(rbsz)
    struct.pack_into(">H", ib, 0, 0x4D49)
    struct.pack_into(">I", ib, 8, 0)
    for i, p in enumerate(bm_pos):
        struct.pack_into(">I", ib, 12 + 4 * i, p)
    img.wr(idx_blk, bytes(ib))
    for n, p in enumerate(bm_pos):
        bm = bytearray(rbsz)
        struct.pack_into(">H", bm, 0, 0x424D)
        struct.pack_into(">I", bm, 8, n)
        for m in range(lpb):
            v = 0
            for j in range(32):
                blk = bstart + (n * lpb + m) * 32 + j
                # real pfs3 leaves out-of-range bits FREE (all-free init;
                # the allocator bounds by numblocks) - mirror that
                if blk not in used:
                    v |= 1 << (31 - j)
            struct.pack_into(">I", bm, 12 + 4 * m, v)
        img.wr(p, bytes(bm))
    return {"used": used, "hash": data_hash.hexdigest(), "free": free,
            "rbsz": rbsz, "bstart": bstart, "opts": opts}

def verify_pfs(img, st, exp_nblk):
    errs = []
    rb = img.rd(2, st["rbsz"])
    g = lambda o: struct.unpack_from(">I", rb, o)[0]
    if g(84) != exp_nblk: errs.append(f"disksize {g(84)} != {exp_nblk}")
    exp_free = (exp_nblk - st["bstart"]) - len(st["used"])
    if g(68) != exp_free: errs.append(f"blocksfree {g(68)} != {exp_free}")
    h = hashlib.md5()
    for b in sorted(st["used"]):
        h.update(img.rd(b))
    if h.hexdigest() != st["hash"]: errs.append("data content changed")
    return errs

# ---- synthetic SFS ----
def sfs_ck(b, bsz):
    b = bytearray(b); struct.pack_into(">I", b, 4, 0)
    acc = 1
    for i in range(0, bsz, 4):
        acc = (acc + struct.unpack_from(">I", b, i)[0]) & 0xFFFFFFFF
    struct.pack_into(">I", b, 4, (-acc) & 0xFFFFFFFF)
    return bytes(b)

def build_sfs(img, rng, nblk_dev, bsz):
    spb  = bsz // 512
    nblk = nblk_dev // spb                # SFS blocks
    bib  = (bsz - 12) * 8
    nbmb = (nblk + bib - 1) // bib
    bmbase = 2
    admin  = bmbase + nbmb + 40           # gap after bitmap for strategy-A grow
    rootobj = admin + 1
    used = set([0, 1]) | set(range(bmbase, bmbase + nbmb)) | {admin, rootobj,
             admin + 2, admin + 3}
    nranges = rng.randint(1, 4)
    for _ in range(nranges):
        a = rng.randint(rootobj + 4, nblk - 2)
        ln = rng.randint(1, min(2000, nblk - 1 - a))
        used.update(range(a, a + ln))
    used.add(nblk - 1)                    # end root
    data_blocks = {b for b in used if b > rootobj + 3 and b != nblk - 1}
    data_hash = hashlib.md5()
    for b in sorted(data_blocks):
        content = bytes(rng.getrandbits(8) for _ in range(bsz))
        img.wr(b * spb, content); data_hash.update(content)

    def mkroot(own, seq):
        r = bytearray(bsz)
        struct.pack_into(">I", r, 0, 0x53465300)
        struct.pack_into(">I", r, 8, own)
        struct.pack_into(">H", r, 12, 3)
        struct.pack_into(">H", r, 14, seq)
        struct.pack_into(">I", r, 36, PBASE * 512)
        struct.pack_into(">I", r, 44, PBASE * 512 + nblk * bsz)
        struct.pack_into(">I", r, 48, nblk)
        struct.pack_into(">I", r, 52, bsz)
        struct.pack_into(">I", r, 96, bmbase)
        struct.pack_into(">I", r, 100, admin)
        struct.pack_into(">I", r, 104, rootobj)
        struct.pack_into(">I", r, 108, admin + 2)
        struct.pack_into(">I", r, 112, admin + 3)
        return sfs_ck(r, bsz)
    img.wr(0, mkroot(0, 7))
    img.wr((nblk - 1) * spb, mkroot(nblk - 1, 7))
    for k in range(nbmb):
        bm = bytearray(bsz)
        struct.pack_into(">I", bm, 0, 0x42544D50)
        struct.pack_into(">I", bm, 8, bmbase + k)
        for m in range(bib // 32):
            v = 0
            for j in range(32):
                blk = k * bib + m * 32 + j
                if blk < nblk and blk not in used:
                    v |= 1 << (31 - j)
            struct.pack_into(">I", bm, 12 + 4 * m, v)
        img.wr((bmbase + k) * spb, sfs_ck(bm, bsz))
    ob = bytearray(bsz)
    struct.pack_into(">I", ob, 0, 0x4F424A43)
    struct.pack_into(">I", ob, 8, rootobj)
    struct.pack_into(">I", ob, bsz - 36 + 8, nblk - len(used))
    img.wr(rootobj * spb, sfs_ck(ob, bsz))
    return {"bsz": bsz, "spb": spb, "data": data_blocks, "hash": data_hash.hexdigest(),
            "bmbase": bmbase, "rootobj": rootobj}

def verify_sfs(img, st, exp_nblk_dev):
    errs = []
    bsz, spb = st["bsz"], st["spb"]
    exp_nblk = exp_nblk_dev // spb
    r0 = img.rd(0, bsz)
    g = lambda b, o: struct.unpack_from(">I", b, o)[0]
    if g(r0, 48) != exp_nblk: errs.append(f"totalblocks {g(r0,48)} != {exp_nblk}")
    re = img.rd((exp_nblk - 1) * spb, bsz)
    if g(re, 0) != 0x53465300 or g(re, 8) != exp_nblk - 1:
        errs.append("end root bad")
    h = hashlib.md5()
    for b in sorted(st["data"]):
        h.update(img.rd(b * spb, bsz))
    if h.hexdigest() != st["hash"]: errs.append("data content changed")
    return errs

# ---------------------------------------------------------------- one iter
def parse_min_high(out):
    for ln in out.splitlines():
        if "HighCyl >=" in ln:
            return int(ln.split("HighCyl >=")[1].strip(" )."))
    return None

def iteration(it, seed):
    rng = random.Random(seed)
    fs = rng.choice(["ffs", "pfs", "sfs"])
    disk_cyls = rng.randint(1400, 2500)
    lo = 1
    part_cyls = rng.randint(300, disk_cyls - 400)
    hi = lo + part_cyls - 1
    img_path = os.path.join(HERE, "fuzz.hdf")
    tag = f"it{it} seed={seed} fs={fs} disk={disk_cyls}c part={lo}-{hi} ({part_cyls}c)"

    if fs == "ffs":
        bs = 512
        build_rdb(img_path, disk_cyls, lo, hi, "DOS3")
        st = build_ffs(img_path, rng, lo, hi)
    elif fs == "pfs":
        bs = rng.choice([512, 1024])
        build_rdb(img_path, disk_cyls, lo, hi, "PDS3")
        st = build_pfs(Img(img_path), rng, part_cyls * 32, bs)
    else:
        bs = rng.choice([512, 1024, 2048])
        nblk_dev = (part_cyls * 32 // (bs // 512)) * (bs // 512)
        build_rdb(img_path, disk_cyls, lo, hi, "SFS0")
        st = build_sfs(Img(img_path), rng, part_cyls * 32, bs)
    tag += f" bs={bs}"

    img = Img(img_path)
    cur_hi = hi
    rounds = 2 if rng.random() < 0.4 else 1
    for rnd in range(rounds):
        # ---- GROW ----
        gap = disk_cyls - 1 - cur_hi
        if gap < 1: break
        grow_cyls = rng.randint(1, gap)
        use_end = rng.random() < 0.2
        garg = "END" if use_end else str(grow_cyls * CYLB)
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "GROW", "DH0", garg, "FORCE"])
        gtries = 0
        while "holds data" in out and gtries < 4:
            gtries += 1
            grow_cyls = rng.randint(1, gap); use_end = False
            garg = str(grow_cyls * CYLB)
            rc, out = run_dp([f"IMAGE=fuzz.hdf", "GROW", "DH0", garg, "FORCE"])
        if "holds data" in out:
            tag += " grow[rootbusy-skip]"
            break
        if "grown" not in out:
            return tag + f" grow={garg}", [f"GROW failed rc={rc}: " + out.strip()[-200:]]
        new_hi = disk_cyls - 1 if use_end else cur_hi + grow_cyls
        new_cyls = new_hi - lo + 1
        tag += f" grow=+{new_hi - cur_hi}c"

        if fs == "ffs":
            ffs_revalidate(img, new_cyls * 32)
        errs = (verify_ffs(img_path, new_cyls, st) if fs == "ffs" else
                verify_pfs(img, st, new_cyls * 32) if fs == "pfs" else
                verify_sfs(img, st, new_cyls * 32))
        if errs:
            return tag, ["after-grow: " + e for e in errs]

        # ---- SHRINKINFO -> floor ----
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINKINFO=DH0"])
        min_high = parse_min_high(out)
        if min_high is None:
            return tag, ["SHRINKINFO gave no floor: " + out.strip()[-200:]]
        if min_high >= new_hi:
            tag += " shrink[at-min-skip]"
            cur_hi = new_hi
            continue

        # ---- SHRINK (random form) ----
        mode = rng.choice(["MIN", "target", "rel"])
        if mode == "MIN":
            sarg, exp_hi = "MIN", min_high
        elif mode == "target":
            t = rng.randint(min_high, new_hi - 1)
            sarg, exp_hi = str((t - lo + 1) * CYLB), t
        else:
            rem = rng.randint(1, new_hi - min_high)
            sarg, exp_hi = f"-{rem * CYLB}", new_hi - rem
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINK=DH0", f"SIZE={sarg}", "FORCE"])
        tries = 0
        while "holds data" in out and tries < 4 and min_high < new_hi - 1:
            # legitimate FFS refusal: the computed root centre lands on data.
            # Documented behavior ("choose a slightly different size") - retry.
            tries += 1
            t = rng.randint(min_high, new_hi - 1)
            sarg, exp_hi = str((t - lo + 1) * CYLB), t
            rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINK=DH0", f"SIZE={sarg}", "FORCE"])
        if "holds data" in out:
            tag += " shrink[rootbusy-skip]"
            cur_hi = new_hi
            continue
        if "shrunk" not in out:
            return tag + f" shrink={sarg}", [f"SHRINK failed rc={rc}: " + out.strip()[-250:]]
        tag += f" shrink[{mode}{'+r' if tries else ''}]->{exp_hi - lo + 1}c"

        info = sh(f"rdbtool fuzz.hdf info | grep \"'DH0'\"")
        got_hi = int(info.split()[4])
        if got_hi != exp_hi:
            return tag, [f"RDB high_cyl {got_hi} != expected {exp_hi}"]

        final_cyls = exp_hi - lo + 1
        errs = (verify_ffs(img_path, final_cyls, st) if fs == "ffs" else
                verify_pfs(img, st, final_cyls * 32) if fs == "pfs" else
                verify_sfs(img, st, final_cyls * 32))
        if errs:
            return tag, ["after-shrink: " + e for e in errs]

        if fs == "ffs":
            ffs_revalidate(img, final_cyls * 32)
        rc, out = run_dp([f"IMAGE=fuzz.hdf", "SHRINKINFO=DH0"])
        if "Shrink report" not in out:
            return tag, ["post-shrink SHRINKINFO failed: " + out.strip()[-200:]]
        cur_hi = exp_hi
    return tag, []

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    base = int(sys.argv[2]) if len(sys.argv) > 2 else random.randrange(1 << 30)
    log(f"=== testloop start: {n} iterations, base seed {base} ===")
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
