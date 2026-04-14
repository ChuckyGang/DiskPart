import struct, sys

HDF = "/home/john/Documents/Code/AmigaPart/DiagDisk.img"

# --- Known values from the last successful grow dialog ---
PART_ABS   = 256
NEW_ROOT   = 363136
BM_BLK_FOR_ROOT = 186458   # bm block covering new_root (partition-relative)
BM_OFF     = 1438           # bit offset of new_root within that bm block
NEW_BLKS   = 726272
OLD_ROOT   = 186368
RESERVED   = 2
BPBM       = 127 * 32       # 4064 blocks per bm block

def read_block(f, n, bsz=512):
    f.seek(n * bsz)
    return struct.unpack_from(f'>{bsz//4}I', f.read(bsz))

def checksum_ok(blk):
    return (sum(blk) & 0xFFFFFFFF) == 0

def bm_bit_free(blk, off):
    """Returns True if the bit at offset 'off' is FREE (=1) in an FFS bitmap block."""
    # FFS: bit 31 of each longword = offset 0, bit 0 = offset 31
    # bm block: L[0]=checksum, data starts at L[1]
    word_idx = 1 + off // 32
    bit_pos  = 31 - (off % 32)
    return bool((blk[word_idx] >> bit_pos) & 1)

with open(HDF, 'rb') as f:

    # ----------------------------------------------------------------
    # 1. Boot block
    # ----------------------------------------------------------------
    print("=" * 60)
    print("BOOT BLOCK (part_abs=%d, abs=%d)" % (PART_ABS, PART_ABS))
    bb = read_block(f, PART_ABS)
    print("  DosType = 0x%08X" % bb[0])
    print("  Checksum= 0x%08X  (%s)" % (bb[1], "ok" if checksum_ok(bb) else "FAIL"))
    print("  bb[2]   = %d  (root ptr — want %d, %s)" % (
        bb[2], NEW_ROOT, "OK" if bb[2] == NEW_ROOT else "MISMATCH"))

    # ----------------------------------------------------------------
    # 2. New root block
    # ----------------------------------------------------------------
    print()
    print("=" * 60)
    print("NEW ROOT at rel=%d abs=%d" % (NEW_ROOT, PART_ABS + NEW_ROOT))
    rb = read_block(f, PART_ABS + NEW_ROOT)
    cs_ok = checksum_ok(rb)
    print("  L[0]  type     = 0x%X  (want 2=T_SHORT)" % rb[0])
    print("  L[1]  own_key  = %d  (want 0)" % rb[1])
    print("  L[2]  seq_num  = %d  (want 0)" % rb[2])
    print("  L[3]  ht_size  = %d  (want 72)" % rb[3])
    print("  L[4]  nothing1 = %d  (want 0)" % rb[4])
    print("  L[5]  checksum = 0x%08X  (%s)" % (rb[5], "OK" if cs_ok else "FAIL"))
    print("  L[78] bm_flag  = 0x%08X  (want 0xFFFFFFFF)" % rb[78])
    print("  L[79] bm[0]    = %d" % rb[79])
    print("  L[104]bm_ext   = %d" % rb[104])
    print("  L[125]parent   = %d  (want 0)" % rb[125])
    print("  L[127]sec_type = 0x%08X  (want 1=ST_ROOT)" % rb[127])
    ht_nz = [(i, rb[6+i]) for i in range(72) if rb[6+i] != 0]
    print("  Hash table: %d non-zero entries" % len(ht_nz))
    looks_root = (rb[0]==2 and rb[1]==0 and rb[4]==0 and rb[127]==1 and cs_ok)
    print("  ==> %s" % ("ROOT IS VALID" if looks_root else "ROOT IS CORRUPT/NOT A ROOT"))

    # ----------------------------------------------------------------
    # 3. BM block covering new_root (Stage 2b equivalent)
    # ----------------------------------------------------------------
    print()
    print("=" * 60)
    print("BM BLOCK FOR new_root: rel=%d abs=%d  (bm_off=%d)" % (
        BM_BLK_FOR_ROOT, PART_ABS + BM_BLK_FOR_ROOT, BM_OFF))
    bm = read_block(f, PART_ABS + BM_BLK_FOR_ROOT)
    bm_cs_ok = checksum_ok(bm)
    print("  Checksum = 0x%08X  (%s)" % (bm[0], "OK" if bm_cs_ok else "FAIL — bm block corrupted"))
    is_free = bm_bit_free(bm, BM_OFF)
    word_idx = 1 + BM_OFF // 32
    bit_pos  = 31 - (BM_OFF % 32)
    print("  new_root bit: L[%d] bit %d = %d  (%s)" % (
        word_idx, bit_pos, (bm[word_idx] >> bit_pos) & 1,
        "FREE — FFS can allocate new_root!" if is_free else "USED — protected"))

    # ----------------------------------------------------------------
    # 4. Old root block (should be data or free, not ST_ROOT)
    # ----------------------------------------------------------------
    print()
    print("=" * 60)
    print("OLD ROOT at rel=%d abs=%d" % (OLD_ROOT, PART_ABS + OLD_ROOT))
    oldrb = read_block(f, PART_ABS + OLD_ROOT)
    old_cs_ok = checksum_ok(oldrb)
    print("  L[0]  type     = 0x%X" % oldrb[0])
    print("  L[1]  own_key  = %d" % oldrb[1])
    print("  L[78] bm_flag  = 0x%08X" % oldrb[78])
    print("  L[127]sec_type = 0x%08X" % oldrb[127])
    print("  Checksum = %s" % ("OK" if old_cs_ok else "FAIL"))
    if oldrb[0]==2 and oldrb[127]==1:
        print("  ==> Still looks like a ROOT block (old root not freed?)")
    else:
        print("  ==> NOT a root block (expected — freed by grow)")

    # ----------------------------------------------------------------
    # 5. Root bm_pages chain check
    # ----------------------------------------------------------------
    print()
    print("=" * 60)
    print("BM CHAIN from new root (checking FFS will find bm_off=%d for new_root)" % BM_OFF)
    expected_bm_idx = (NEW_ROOT - RESERVED) // BPBM
    print("  Expected bm_idx for new_root=%d: %d" % (NEW_ROOT, expected_bm_idx))

    if looks_root:
        # Walk bm chain and find entry at expected_bm_idx
        chain = []
        for i in range(25):
            v = rb[79 + i]
            if v == 0:
                break
            chain.append(v)
        ext_ptr = rb[104]
        ext_num = 0
        while ext_ptr not in (0, 0xFFFFFFFF) and ext_num < 32:
            eb = read_block(f, PART_ABS + ext_ptr)
            for i in range(127):
                if eb[i] == 0:
                    break
                chain.append(eb[i])
            ext_ptr = eb[127]
            ext_num += 1

        print("  bm chain length = %d (want ~%d)" % (len(chain), NEW_BLKS // BPBM + 1))
        if expected_bm_idx < len(chain):
            actual_blk = chain[expected_bm_idx]
            print("  chain[%d] = %d  (bm_blk_for_root was %d, %s)" % (
                expected_bm_idx, actual_blk, BM_BLK_FOR_ROOT,
                "MATCH" if actual_blk == BM_BLK_FOR_ROOT else "MISMATCH!"))
            if actual_blk != BM_BLK_FOR_ROOT:
                print("  MISMATCH: FFS will read a DIFFERENT bm block than Stage 2b verified!")
                print("  Checking actual block FFS would use: %d" % actual_blk)
                bm2 = read_block(f, PART_ABS + actual_blk)
                bm2_cs_ok = checksum_ok(bm2)
                is_free2 = bm_bit_free(bm2, BM_OFF)
                print("    checksum = %s" % ("OK" if bm2_cs_ok else "FAIL"))
                print("    new_root bit = %s" % ("FREE!" if is_free2 else "USED"))
        else:
            print("  ERROR: chain too short (%d entries), can't reach bm_idx=%d" % (
                len(chain), expected_bm_idx))
    else:
        print("  (Skipped — new root is not valid)")

print()
print("SUMMARY:")
if looks_root:
    print("  Root block at new_root: VALID")
else:
    print("  Root block at new_root: CORRUPT")
if bm_cs_ok and not is_free:
    print("  BM block for new_root: OK (new_root marked USED)")
elif not bm_cs_ok:
    print("  BM block for new_root: BAD CHECKSUM")
else:
    print("  BM block for new_root: new_root marked FREE")
