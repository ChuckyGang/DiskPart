/*
 * sfsresize.c — Experimental SmartFileSystem (SFS) partition grow.
 *
 * TWO STRATEGIES, chosen automatically:
 *
 * A) NATURAL PLACEMENT (preferred when possible):
 *    New bitmap blocks go at bitmapbase+old_bmb .. bitmapbase+new_bmb-1.
 *    Only possible if those SFS blocks are currently FREE in the bitmap.
 *    Updates existing bitmap blocks to mark those positions IN USE and
 *    writes the new BTMP blocks there.
 *
 * B) BITMAP RELOCATION (fallback when natural positions are occupied):
 *    The entire bitmap (old + new blocks) is relocated to the start of
 *    the extended area: new_bitmapbase = old_totalblocks.
 *    Old bitmap data is read, patched, and written to the new location.
 *    New bitmap blocks follow.  The old bitmap positions become free space.
 *    bitmapbase in both root blocks is updated to new_bitmapbase.
 *    The extended area provides guaranteed free space for the new bitmap,
 *    so no existing user data is touched.
 *
 *    Relocation fails only if delta_sfs <= new_bmb_count (not enough room
 *    in the extended area for the relocated bitmap + end root).
 *
 * COMMON FINAL STEPS (both strategies):
 *    - Write new end root at new_totalblocks-1 (seqnum = max+1).
 *    - Write updated start root at block 0  (seqnum = max+2 → authoritative).
 *    - Delay(50) + Inhibit(TRUE)+Inhibit(FALSE) to flush SFS cache.
 *
 * WRITE ORDER:
 *    All writes before Inhibit.  UAE/Amiberry zeroes device write-extent
 *    during Inhibit(DOSTRUE); writes after that fail "out of bounds".
 *
 * REVERSIBILITY:
 *    Strategy A: original existing bitmap blocks are saved before writing.
 *    Strategy B: all new writes go to the extended area; old structure is
 *    untouched until the final start-root write.  Rolling back the start
 *    root restores the original structure completely.
 *
 * REFERENCES (SFS source, /home/john/Downloads/SFS/Smartfilesystem/Sources):
 *   fs/blockstructure.h  — fsRootBlock, fsBlockHeader layout
 *   fs/bitmap.h          — fsBitmap, BITMAP_ID
 *   SFScheck/SFScheck.c  — checkrootblock, blocks_inbitmap formula
 *   SFScheck/asmsupport.s — CALCCHECKSUM: acc=1, add all ULONGs; 0=valid
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <devices/trackdisk.h>
#ifndef TD_WRITE64
#define TD_WRITE64  (CMD_NONSTD + 16)
#endif
#include <dos/dos.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>

extern struct DosLibrary    *DOSBase;
extern struct IntuitionBase *IntuitionBase;

#include "clib.h"
#include "rdb.h"
#include "ffsresize.h"
#include "sfsresize.h"
#include "sfs_util.h"

/* ------------------------------------------------------------------ */
/* SFS on-disk block IDs                                               */
/* ------------------------------------------------------------------ */
#define SFS_ROOT_ID   0x53465300UL   /* 'SFS\0' — root block id field */
#define SFS_BITMAP_ID 0x42544D50UL   /* 'BTMP' — bitmap block id      */
#define SFS_OBJC_ID   0x4F424A43UL   /* 'OBJC' — objectcontainer id   */

/* fsRootInfo is at the END of the rootobjectcontainer (OBJC) block.
 * Layout: deletedblocks(+0), deletedfiles(+4), freeblocks(+8), ...
 * freeblocks byte offset within the block = sfs_blocksize - 36 + 8
 *                                         = sfs_blocksize - 28        */
#define SFS_RI_FREEBLOCKS_FROM_END 28  /* bytes before block end */

/* ------------------------------------------------------------------ */
/* fsRootBlock byte offsets (struct layout from blockstructure.h)      */
/* ------------------------------------------------------------------ */
#define SFS_RB_ID           0   /* ULONG */
#define SFS_RB_CHECKSUM     4   /* ULONG */
#define SFS_RB_OWNBLOCK     8   /* ULONG */
#define SFS_RB_VERSION      12  /* UWORD */
#define SFS_RB_SEQNUM       14  /* UWORD */
#define SFS_RB_DATECREATED  16  /* ULONG */
#define SFS_RB_BITS         20  /* UBYTE */
/* 24-31: reserved1[2] */
#define SFS_RB_FIRSTBYTEH   32  /* ULONG */
#define SFS_RB_FIRSTBYTE    36  /* ULONG */
#define SFS_RB_LASTBYTEH    40  /* ULONG */
#define SFS_RB_LASTBYTE     44  /* ULONG */
#define SFS_RB_TOTALBLOCKS  48  /* ULONG */
#define SFS_RB_BLOCKSIZE    52  /* ULONG */
/* 56-95: reserved2[2] + reserved3[8] */
#define SFS_RB_BITMAPBASE   96  /* ULONG */
#define SFS_RB_ADMINSPACE  100  /* ULONG: adminspacecontainer */
#define SFS_RB_ROOTOBJ     104  /* ULONG: rootobjectcontainer */
#define SFS_RB_EXTBNODE    108  /* ULONG: extentbnoderoot     */
#define SFS_RB_OBJNODE     112  /* ULONG: objectnoderoot      */

#define SFS_RB_STRUCT_SIZE  128 /* sizeof(struct fsRootBlock) */
#define SFS_BM_HEADER_SIZE  12  /* sizeof(struct fsBitmap) header */

/* Maximum distinct existing bitmap blocks modified in natural placement */
#define MAX_MOD_BMB 4

/* ------------------------------------------------------------------ */
/* Big-endian accessors                                                 */
/* ------------------------------------------------------------------ */
/* sfs_getl, sfs_getw, sfs_setl, sfs_set_checksum, sfs_verify_checksum
   are in sfs_util.c / sfs_util.h */

static void sfs_setw(UBYTE *b, ULONG o, UWORD v)
{
    b[o]=(UBYTE)(v>>8); b[o+1]=(UBYTE)v;
}

/* ------------------------------------------------------------------ */
/* Block I/O                                                            */
/* ------------------------------------------------------------------ */
static BOOL sfs_read_block(struct BlockDev *bd, ULONG phys_base,
                            ULONG sfs_blknum, ULONG sfs_phys, UBYTE *buf)
{
    ULONG start = phys_base + sfs_blknum * sfs_phys;
    ULONG i;
    for (i = 0; i < sfs_phys; i++)
        if (!BlockDev_ReadBlock(bd, start + i, buf + i * 512)) return FALSE;
    return TRUE;
}

static BOOL sfs_write_block(struct BlockDev *bd, ULONG phys_base,
                              ULONG sfs_blknum, ULONG sfs_phys, const UBYTE *buf)
{
    ULONG start = phys_base + sfs_blknum * sfs_phys;
    ULONG i;
    for (i = 0; i < sfs_phys; i++)
        if (!BlockDev_WriteBlock(bd, start + i, (const void *)(buf + i*512)))
            return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Bitmap bit helpers.                                                  */
/* Bitmap block covers [base .. base+blocks_inbitmap-1].               */
/* MSB of each ULONG = first block (big-endian bit order).             */
/* bit 1=free, bit 0=in-use.                                           */
/* ------------------------------------------------------------------ */
static void sfs_bm_set_free(UBYTE *bmb, ULONG base, ULONG B)
{
    ULONG bit_off = B - base;
    ULONG *bm = (ULONG *)(bmb + SFS_BM_HEADER_SIZE);
    bm[bit_off/32] |= 1UL << (31 - (bit_off%32));
}
static void sfs_bm_set_used(UBYTE *bmb, ULONG base, ULONG B)
{
    ULONG bit_off = B - base;
    ULONG *bm = (ULONG *)(bmb + SFS_BM_HEADER_SIZE);
    bm[bit_off/32] &= ~(1UL << (31 - (bit_off%32)));
}
static int sfs_bm_is_free(const UBYTE *bmb, ULONG base, ULONG B)
{
    ULONG bit_off = B - base;
    const ULONG *bm = (const ULONG *)(bmb + SFS_BM_HEADER_SIZE);
    return (bm[bit_off/32] & (1UL << (31 - (bit_off%32)))) ? 1 : 0;
}

/* Mark a range of blocks [lo..hi] intersected with coverage [base..base+bpbm-1]. */
static void sfs_bm_mark_range(UBYTE *bmb, ULONG base, ULONG bpbm,
                               ULONG lo, ULONG hi, int set_free)
{
    ULONG B;
    ULONG r_lo = (lo > base) ? lo : base;
    ULONG r_hi_cov = base + bpbm - 1;
    ULONG r_hi = (hi < r_hi_cov) ? hi : r_hi_cov;
    if (r_lo > r_hi) return;
    for (B = r_lo; B <= r_hi; B++) {
        if (set_free) sfs_bm_set_free(bmb, base, B);
        else          sfs_bm_set_used(bmb, base, B);
    }
}

/* ------------------------------------------------------------------ */

BOOL SFS_IsSupportedType(ULONG dostype)
{
    ULONG prefix = dostype >> 8;
    UBYTE ver    = (UBYTE)dostype;
    if (prefix == 0x534653UL && ver <= 3) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */

BOOL SFS_GrowPartition(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, ULONG old_high_cyl,
                       char *err_buf,
                       FFS_ProgressFn progress_fn, void *progress_ud)
{
#define SFS_PROGRESS(msg) do { if (progress_fn) progress_fn(progress_ud,(msg)); } while(0)

    /* --- root block buffers --- */
    UBYTE *buf_root0   = NULL;
    UBYTE *buf_orig0   = NULL;
    UBYTE *buf_rootend = NULL;
    UBYTE *buf_newend  = NULL;

    /* --- bitmap scratch buffers --- */
    UBYTE *buf_bmb_work = NULL;  /* work buffer for one bitmap block  */
    UBYTE *buf_bmb_read = NULL;  /* temp read buffer (relocation)     */

    /* --- natural placement: existing bitmap block cache --- */
    UBYTE *mod_buf[MAX_MOD_BMB];
    UBYTE *mod_orig[MAX_MOD_BMB];
    ULONG  mod_idx[MAX_MOD_BMB];
    int    mod_written[MAX_MOD_BMB];
    int    num_mod;

    BOOL   ok             = FALSE;
    BOOL   did_inhibit    = FALSE;
    int    root0_written  = 0;
    int    rootend_written = 0;
    int    old_end_inv    = 0;  /* 1 = old end root successfully invalidated */
    ULONG  ri_free_counted = 0; /* free block count from bitmap (for OBJC update) */
    int    objc_updated   = 0; /* 1 = fsRootInfo.freeblocks updated in OBJC block */
    ULONG  objc_old_free  = 0; /* old cached freeblocks (for diagnostic) */
    char   inh_name[44];

    ULONG  sfs_blocksize = 0;
    ULONG  sfs_phys      = 0;
    ULONG  phys_base     = 0;
    ULONG  phys_per_lb;
    ULONG  heads, sectors;

    /* Root block fields */
    ULONG  totalblocks, firstbyteh, firstbyte;
    ULONG  lastbyteh, lastbyte_lo;
    ULONG  bitmapbase, adminspace, rootobj, extbnode, objnode;
    UWORD  seq_start, seq_end, new_seqnum;
    BOOL   end_root_valid;

    /* Grow computation */
    ULONG  old_ncyl, cyl_diff, bpc, delta_sfs, new_totalblocks;
    ULONG  blocks_inbitmap, old_bmb_count, new_bmb_count, num_new_bmb;
    ULONG  new_lastbyteh, new_lastbyte_lo;

    /* Which bitmapbase to write into root blocks */
    ULONG  new_bitmapbase_for_root;

    /* Strategy flag */
    int    use_relocation = 0;

    /* Loop vars */
    UBYTE  scratch[512];
    ULONG  i, k;
    int    mi;

    for (mi = 0; mi < MAX_MOD_BMB; mi++) {
        mod_buf[mi]  = NULL; mod_orig[mi] = NULL;
        mod_idx[mi]  = 0xFFFFFFFFUL; mod_written[mi] = 0;
    }
    num_mod = 0;

    heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
    sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    if (heads == 0 || sectors == 0) {
        sprintf(err_buf, "Invalid geometry (h=%lu s=%lu)",
                (unsigned long)heads, (unsigned long)sectors);
        return FALSE;
    }

    phys_per_lb = (pi->block_size >= 1024) ? (pi->block_size / 512) : 1;
    phys_base   = pi->low_cyl * heads * sectors * phys_per_lb;

    /* ---------------------------------------------------------------- */
    /* Phase 1-2: read first 512 bytes, validate id, extract blocksize  */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Reading SFS root block...");
    if (!BlockDev_ReadBlock(bd, phys_base, scratch)) {
        sprintf(err_buf, "Cannot read SFS block 0 (phys %lu)", (unsigned long)phys_base);
        return FALSE;
    }
    if (sfs_getl(scratch, SFS_RB_ID) != SFS_ROOT_ID) {
        sprintf(err_buf,
                "SFS block 0 id=0x%08lX (expected 0x%08lX).\n"
                "Not SFS or wrong partition start.\n"
                "phys=%lu low=%lu h=%lu s=%lu",
                (unsigned long)sfs_getl(scratch, SFS_RB_ID),
                (unsigned long)SFS_ROOT_ID,
                (unsigned long)phys_base,
                (unsigned long)pi->low_cyl,
                (unsigned long)heads,
                (unsigned long)sectors);
        return FALSE;
    }
    sfs_blocksize = sfs_getl(scratch, SFS_RB_BLOCKSIZE);
    if (sfs_blocksize < 512 || (sfs_blocksize & (sfs_blocksize - 1)) ||
        (sfs_blocksize % 512) != 0) {
        sprintf(err_buf, "SFS blocksize=%lu invalid.", (unsigned long)sfs_blocksize);
        return FALSE;
    }
    sfs_phys = sfs_blocksize / 512;

    /* ---------------------------------------------------------------- */
    /* Phase 3: allocate buffers                                         */
    /* ---------------------------------------------------------------- */
    buf_root0    = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
    buf_orig0    = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
    buf_rootend  = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
    buf_newend   = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
    buf_bmb_work = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
    buf_bmb_read = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
    if (!buf_root0 || !buf_orig0 || !buf_rootend || !buf_newend ||
        !buf_bmb_work || !buf_bmb_read) {
        sprintf(err_buf, "Out of memory (6x%lu bytes)", (unsigned long)sfs_blocksize);
        goto done;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 3b: Inhibit SFS handler BEFORE reading root                 */
    /* Must be quiescent so seqnum on disk is final (no more SFS         */
    /* flushes can race with our writes and win on seqnum).              */
    /* FFS uses the same pattern: Inhibit first, then read+write.        */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Inhibiting SFS handler...");
    if (pi->drive_name[0]) {
        sprintf(inh_name, "%s:", pi->drive_name);
        if (Inhibit((STRPTR)inh_name, DOSTRUE)) did_inhibit = TRUE;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 4: read + validate start root block                         */
    /* ---------------------------------------------------------------- */
    if (!sfs_read_block(bd, phys_base, 0, sfs_phys, buf_root0)) {
        sprintf(err_buf, "Cannot read SFS root block 0"); goto done;
    }
    if (!sfs_verify_checksum(buf_root0, sfs_blocksize)) {
        sprintf(err_buf, "SFS root block 0 bad checksum."); goto done;
    }
    if (sfs_getw(buf_root0, SFS_RB_VERSION) != 3) {
        sprintf(err_buf, "SFS root version=%u (expected 3).",
                (unsigned)sfs_getw(buf_root0, SFS_RB_VERSION)); goto done;
    }
    if (sfs_getl(buf_root0, SFS_RB_OWNBLOCK) != 0) {
        sprintf(err_buf, "SFS root block 0 ownblock=%lu (expected 0).",
                (unsigned long)sfs_getl(buf_root0, SFS_RB_OWNBLOCK)); goto done;
    }

    totalblocks = sfs_getl(buf_root0, SFS_RB_TOTALBLOCKS);
    firstbyteh  = sfs_getl(buf_root0, SFS_RB_FIRSTBYTEH);
    firstbyte   = sfs_getl(buf_root0, SFS_RB_FIRSTBYTE);
    lastbyteh   = sfs_getl(buf_root0, SFS_RB_LASTBYTEH);
    lastbyte_lo = sfs_getl(buf_root0, SFS_RB_LASTBYTE);
    bitmapbase  = sfs_getl(buf_root0, SFS_RB_BITMAPBASE);
    adminspace  = sfs_getl(buf_root0, SFS_RB_ADMINSPACE);
    rootobj     = sfs_getl(buf_root0, SFS_RB_ROOTOBJ);
    extbnode    = sfs_getl(buf_root0, SFS_RB_EXTBNODE);
    objnode     = sfs_getl(buf_root0, SFS_RB_OBJNODE);
    seq_start   = sfs_getw(buf_root0, SFS_RB_SEQNUM);

    if (totalblocks < 2) {
        sprintf(err_buf, "SFS totalblocks=%lu too small.", (unsigned long)totalblocks);
        goto done;
    }
    for (k = 0; k < sfs_blocksize; k++) buf_orig0[k] = buf_root0[k];

    /* ---------------------------------------------------------------- */
    /* Phase 5: read end root at totalblocks-1                           */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Reading SFS end root...");
    end_root_valid = FALSE; seq_end = 0;
    if (sfs_read_block(bd, phys_base, totalblocks-1, sfs_phys, buf_rootend))
        if (sfs_verify_checksum(buf_rootend, sfs_blocksize) &&
            sfs_getl(buf_rootend, SFS_RB_ID) == SFS_ROOT_ID &&
            sfs_getw(buf_rootend, SFS_RB_VERSION) == 3 &&
            sfs_getl(buf_rootend, SFS_RB_OWNBLOCK) == (totalblocks-1)) {
            seq_end = sfs_getw(buf_rootend, SFS_RB_SEQNUM);
            end_root_valid = TRUE;
        }
    if (end_root_valid && (UWORD)(seq_end - seq_start) < 0x8000u)
        new_seqnum = seq_end;
    else
        new_seqnum = seq_start;

    /* ---------------------------------------------------------------- */
    /* Phase 6: compute grow sizes                                       */
    /* ---------------------------------------------------------------- */
    old_ncyl = (old_high_cyl >= pi->low_cyl) ? (old_high_cyl - pi->low_cyl + 1) : 1;
    cyl_diff = pi->high_cyl - old_high_cyl;
    bpc = (old_ncyl > 0 && totalblocks > 0) ? (totalblocks / old_ncyl)
                                             : (heads * sectors);
    if (bpc == 0) bpc = 1;
    delta_sfs      = cyl_diff * bpc;
    new_totalblocks = totalblocks + delta_sfs;
    {
        UQUAD old_lb = ((UQUAD)lastbyteh << 32) | (UQUAD)lastbyte_lo;
        UQUAD new_lb = old_lb + (UQUAD)delta_sfs * (UQUAD)sfs_blocksize;
        new_lastbyteh   = (ULONG)(new_lb >> 32);
        new_lastbyte_lo = (ULONG)(new_lb & 0xFFFFFFFFUL);
    }

    /* ---------------------------------------------------------------- */
    /* Phase 7: bitmap coverage counts                                   */
    /* ---------------------------------------------------------------- */
    blocks_inbitmap = (sfs_blocksize - (ULONG)SFS_BM_HEADER_SIZE) * 8;
    if (blocks_inbitmap == 0) {
        sprintf(err_buf, "SFS blocksize too small for bitmap."); goto done;
    }
    old_bmb_count = (totalblocks     + blocks_inbitmap - 1) / blocks_inbitmap;
    new_bmb_count = (new_totalblocks + blocks_inbitmap - 1) / blocks_inbitmap;
    num_new_bmb   = (new_bmb_count > old_bmb_count) ? (new_bmb_count - old_bmb_count) : 0;

    /* Default: bitmapbase unchanged in root blocks */
    new_bitmapbase_for_root = bitmapbase;

    /* ================================================================ */
    /* STRATEGY SELECTION                                                */
    /* ================================================================ */
    if (num_new_bmb > 0) {

        /* ---- Phase 8a: read bitmap block(s) covering natural positions ---- */
        SFS_PROGRESS("Checking new bitmap block positions...");
        {
            ULONG need_idx[1 + MAX_MOD_BMB];
            int   need_n = 0, dup, j;

            /* Always need: block covering old end root */
            need_idx[need_n++] = (totalblocks - 1) / blocks_inbitmap;

            for (k = 0; k < num_new_bmb; k++) {
                ULONG pos     = bitmapbase + old_bmb_count + k;
                ULONG bmb_for = pos / blocks_inbitmap;
                if (bmb_for >= old_bmb_count) { use_relocation = 1; break; }
                dup = 0;
                for (j = 0; j < need_n; j++) if (need_idx[j] == bmb_for) { dup=1; break; }
                if (!dup) {
                    if (need_n >= MAX_MOD_BMB) { use_relocation = 1; break; }
                    need_idx[need_n++] = bmb_for;
                }
            }

            if (!use_relocation) {
                /* Read and cache needed existing bitmap blocks */
                num_mod = need_n;
                for (mi = 0; mi < num_mod && !use_relocation; mi++) {
                    ULONG blk = bitmapbase + need_idx[mi];
                    mod_idx[mi]  = need_idx[mi];
                    mod_buf[mi]  = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
                    mod_orig[mi] = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
                    if (!mod_buf[mi] || !mod_orig[mi]) {
                        sprintf(err_buf, "Out of memory (bmb cache)"); goto done;
                    }
                    if (!sfs_read_block(bd, phys_base, blk, sfs_phys, mod_buf[mi])) {
                        sprintf(err_buf, "Cannot read bitmap block %lu", (unsigned long)blk);
                        goto done;
                    }
                    if (!sfs_verify_checksum(mod_buf[mi], sfs_blocksize) ||
                        sfs_getl(mod_buf[mi], SFS_RB_ID) != SFS_BITMAP_ID) {
                        sprintf(err_buf, "Bad bitmap block %lu", (unsigned long)blk);
                        goto done;
                    }
                    for (j = 0; (ULONG)j < sfs_blocksize; j++)
                        mod_orig[mi][j] = mod_buf[mi][j];
                }

                if (!use_relocation) {
                    /* ---- Phase 8b: check natural positions are FREE ---- */
                    for (k = 0; k < num_new_bmb && !use_relocation; k++) {
                        ULONG pos      = bitmapbase + old_bmb_count + k;
                        ULONG bmb_for  = pos / blocks_inbitmap;
                        ULONG base_blk = bmb_for * blocks_inbitmap;
                        for (mi = 0; mi < num_mod; mi++)
                            if (mod_idx[mi] == bmb_for) break;
                        if (mi >= num_mod || !sfs_bm_is_free(mod_buf[mi], base_blk, pos))
                            use_relocation = 1;
                    }
                }
            }
        }

        if (!use_relocation) {
            /* ============================================================ */
            /* STRATEGY A: NATURAL PLACEMENT                                 */
            /* ============================================================ */

            /* Apply bit changes to existing bitmap caches */
            {
                /* Old end root → FREE; also free the bitmap block's slack tail.
                   oe_bmb == old_bmb_count-1 always.  The tail [totalblocks ..
                   oe_base+blocks_inbitmap-1] was marked 0 (non-existent) but
                   is now valid free space after grow. */
                ULONG oe_bmb  = (totalblocks-1) / blocks_inbitmap;
                ULONG oe_base = oe_bmb * blocks_inbitmap;
                ULONG oe_tail = oe_base + blocks_inbitmap - 1;
                for (mi = 0; mi < num_mod; mi++)
                    if (mod_idx[mi] == oe_bmb) break;
                if (mi < num_mod) {
                    sfs_bm_set_free(mod_buf[mi], oe_base, totalblocks-1);
                    if (oe_tail >= totalblocks)
                        sfs_bm_mark_range(mod_buf[mi], oe_base, blocks_inbitmap,
                                          totalblocks, oe_tail, 1);
                }

                /* New bitmap block positions → IN USE */
                for (k = 0; k < num_new_bmb; k++) {
                    ULONG pos      = bitmapbase + old_bmb_count + k;
                    ULONG bmb_for  = pos / blocks_inbitmap;
                    ULONG base_blk = bmb_for * blocks_inbitmap;
                    for (mi = 0; mi < num_mod; mi++)
                        if (mod_idx[mi] == bmb_for) break;
                    if (mi < num_mod) sfs_bm_set_used(mod_buf[mi], base_blk, pos);
                }

                /* New end root in existing range (if it fits) */
                {
                    ULONG ne_bmb = (new_totalblocks-1) / blocks_inbitmap;
                    if (ne_bmb < old_bmb_count) {
                        ULONG ne_base = ne_bmb * blocks_inbitmap;
                        for (mi = 0; mi < num_mod; mi++)
                            if (mod_idx[mi] == ne_bmb) break;
                        if (mi < num_mod)
                            sfs_bm_set_used(mod_buf[mi], ne_base, new_totalblocks-1);
                    }
                }
            }

            /* Write updated existing bitmap blocks */
            SFS_PROGRESS("Writing bitmap blocks (natural)...");
            for (mi = 0; mi < num_mod; mi++) {
                ULONG blk = bitmapbase + mod_idx[mi];
                sfs_set_checksum(mod_buf[mi], sfs_blocksize);
                if (!sfs_write_block(bd, phys_base, blk, sfs_phys, mod_buf[mi])) {
                    sprintf(err_buf, "Cannot write bitmap block %lu.", (unsigned long)blk);
                    goto done;
                }
                mod_written[mi] = 1;
            }

            /* Write new bitmap blocks in natural positions */
            SFS_PROGRESS("Writing new bitmap blocks...");
            for (k = 0; k < num_new_bmb; k++) {
                ULONG blk      = bitmapbase + old_bmb_count + k;
                ULONG bmb_idx  = old_bmb_count + k;
                ULONG base_blk = bmb_idx * blocks_inbitmap;
                ULONG blk_end  = base_blk + blocks_inbitmap - 1;
                ULONG ul_count = (sfs_blocksize - SFS_BM_HEADER_SIZE) / 4;
                ULONG j;
                ULONG *bm;

                for (j = 0; j < sfs_blocksize; j++) buf_bmb_work[j] = 0;
                sfs_setl(buf_bmb_work, SFS_RB_ID,      SFS_BITMAP_ID);
                sfs_setl(buf_bmb_work, SFS_RB_OWNBLOCK, blk);
                bm = (ULONG *)(buf_bmb_work + SFS_BM_HEADER_SIZE);
                for (j = 0; j < ul_count; j++) bm[j] = 0xFFFFFFFFUL;

                /* New end root → IN USE */
                if (new_totalblocks-1 >= base_blk && new_totalblocks-1 <= blk_end)
                    sfs_bm_set_used(buf_bmb_work, base_blk, new_totalblocks-1);
                /* Non-existent blocks → 0 */
                if (blk_end >= new_totalblocks)
                    sfs_bm_mark_range(buf_bmb_work, base_blk, blocks_inbitmap,
                                      new_totalblocks, blk_end, 0);

                sfs_set_checksum(buf_bmb_work, sfs_blocksize);
                if (!sfs_write_block(bd, phys_base, blk, sfs_phys, buf_bmb_work)) {
                    sprintf(err_buf, "Cannot write new bitmap block %lu.", (unsigned long)blk);
                    goto done;
                }
            }
            /* bitmapbase unchanged for root blocks */

        } else {
            /* ============================================================ */
            /* STRATEGY B: BITMAP RELOCATION                                 */
            /*                                                               */
            /* Relocate entire bitmap (old + new) to start of extended area. */
            /* new_bitmapbase = old_totalblocks                               */
            /* New bitmap blocks: old_totalblocks .. old_totalblocks+new_bmb_count-1 */
            /* ============================================================ */
            ULONG new_bitmapbase = totalblocks;

            /* Sanity: need at least new_bmb_count + 1 blocks in extended area
               (new_bmb_count for relocated bitmap, 1 for new end root).  The
               new end root can share the last block (new_totalblocks-1) which
               is always beyond new_bitmapbase+new_bmb_count if delta_sfs > new_bmb_count. */
            if (delta_sfs <= new_bmb_count) {
                sprintf(err_buf,
                        "Extended area (%lu blks) too small for relocated\n"
                        "bitmap (%lu bmb blocks). Need delta > %lu.\n"
                        "Grow by a larger amount to use bitmap relocation.",
                        (unsigned long)delta_sfs,
                        (unsigned long)new_bmb_count,
                        (unsigned long)new_bmb_count);
                goto done;
            }

            new_bitmapbase_for_root = new_bitmapbase;

            SFS_PROGRESS("Relocating SFS bitmap...");

            /* Write all new_bmb_count bitmap blocks to new positions */
            for (k = 0; k < new_bmb_count; k++) {
                ULONG new_blk  = new_bitmapbase + k;  /* write here */
                ULONG base_blk = k * blocks_inbitmap; /* first block covered */
                ULONG blk_end  = base_blk + blocks_inbitmap - 1;
                ULONG ul_count = (sfs_blocksize - SFS_BM_HEADER_SIZE) / 4;
                ULONG j;
                ULONG *bm;

                /* Initialize buffer */
                for (j = 0; j < sfs_blocksize; j++) buf_bmb_work[j] = 0;
                sfs_setl(buf_bmb_work, SFS_RB_ID,      SFS_BITMAP_ID);
                sfs_setl(buf_bmb_work, SFS_RB_OWNBLOCK, new_blk);

                if (k < old_bmb_count) {
                    /* Copy old bitmap data for this index */
                    if (!sfs_read_block(bd, phys_base, bitmapbase+k, sfs_phys,
                                         buf_bmb_read)) {
                        sprintf(err_buf, "Cannot read old bitmap block %lu.",
                                (unsigned long)(bitmapbase+k));
                        goto done;
                    }
                    /* copy data portion only */
                    for (j = SFS_BM_HEADER_SIZE; j < sfs_blocksize; j++)
                        buf_bmb_work[j] = buf_bmb_read[j];
                } else {
                    /* New coverage area: initialize all bits to 1 (free) */
                    bm = (ULONG *)(buf_bmb_work + SFS_BM_HEADER_SIZE);
                    for (j = 0; j < ul_count; j++) bm[j] = 0xFFFFFFFFUL;
                }

                /* ---- Apply patches ---- */

                /* 1. Free the old bitmap block positions (they become user space) */
                sfs_bm_mark_range(buf_bmb_work, base_blk, blocks_inbitmap,
                                  bitmapbase, bitmapbase + old_bmb_count - 1, 1);

                /* 2. Free the old end root (becomes user space) */
                if (totalblocks-1 >= base_blk && totalblocks-1 <= blk_end)
                    sfs_bm_set_free(buf_bmb_work, base_blk, totalblocks-1);

                /* 3. Extended area blocks in coverage: set correctly */
                if (blk_end >= totalblocks) {
                    /* For k < old_bmb_count: bits >= totalblocks were 0 in old bitmap;
                       set them free now (they're valid new blocks). */
                    if (k < old_bmb_count) {
                        ULONG ext_lo = (totalblocks > base_blk) ? totalblocks : base_blk;
                        ULONG ext_hi = (new_totalblocks-1 < blk_end) ? new_totalblocks-1 : blk_end;
                        sfs_bm_mark_range(buf_bmb_work, base_blk, blocks_inbitmap,
                                          ext_lo, ext_hi, 1);
                    }
                    /* Mark new bitmap block positions IN USE */
                    sfs_bm_mark_range(buf_bmb_work, base_blk, blocks_inbitmap,
                                      new_bitmapbase,
                                      new_bitmapbase + new_bmb_count - 1, 0);
                    /* Mark new end root IN USE */
                    if (new_totalblocks-1 >= base_blk && new_totalblocks-1 <= blk_end)
                        sfs_bm_set_used(buf_bmb_work, base_blk, new_totalblocks-1);
                    /* Clear bits beyond new_totalblocks (non-existent) */
                    if (blk_end >= new_totalblocks)
                        sfs_bm_mark_range(buf_bmb_work, base_blk, blocks_inbitmap,
                                          new_totalblocks, blk_end, 0);
                }

                sfs_set_checksum(buf_bmb_work, sfs_blocksize);
                if (!sfs_write_block(bd, phys_base, new_blk, sfs_phys, buf_bmb_work)) {
                    sprintf(err_buf, "Cannot write relocated bitmap block %lu.",
                            (unsigned long)new_blk);
                    goto done;
                }
            } /* for k */

        } /* end strategy B */

    } else {
        /* ================================================================ */
        /* num_new_bmb == 0: update only the existing bitmap blocks for     */
        /* old end root → free and new end root → in use.                  */
        /* ================================================================ */
        ULONG oe_bmb  = (totalblocks-1) / blocks_inbitmap;
        ULONG ne_bmb  = (new_totalblocks-1) / blocks_inbitmap;
        ULONG need_idx[2];
        int   need_n = 0, dup, j;

        need_idx[need_n++] = oe_bmb;
        dup = (ne_bmb == oe_bmb) ? 1 : 0;
        if (!dup) need_idx[need_n++] = ne_bmb;

        num_mod = need_n;
        for (mi = 0; mi < num_mod; mi++) {
            ULONG blk = bitmapbase + need_idx[mi];
            mod_idx[mi]  = need_idx[mi];
            mod_buf[mi]  = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
            mod_orig[mi] = (UBYTE *)AllocVec(sfs_blocksize, MEMF_PUBLIC);
            if (!mod_buf[mi] || !mod_orig[mi]) {
                sprintf(err_buf, "Out of memory (bmb cache)"); goto done;
            }
            SFS_PROGRESS("Reading bitmap block...");
            if (!sfs_read_block(bd, phys_base, blk, sfs_phys, mod_buf[mi])) {
                sprintf(err_buf, "Cannot read bitmap block %lu", (unsigned long)blk);
                goto done;
            }
            if (!sfs_verify_checksum(mod_buf[mi], sfs_blocksize) ||
                sfs_getl(mod_buf[mi], SFS_RB_ID) != SFS_BITMAP_ID) {
                sprintf(err_buf, "Bad bitmap block %lu", (unsigned long)blk); goto done;
            }
            for (j = 0; (ULONG)j < sfs_blocksize; j++)
                mod_orig[mi][j] = mod_buf[mi][j];
        }
        /* Old end root → FREE; free new blocks [totalblocks .. new_totalblocks-2].
           When num_new_bmb==0, both old and new end roots are always in the same
           bitmap block (oe_bmb==ne_bmb), and all new free blocks are in that block
           too (proven: delta_sfs < blocks_inbitmap in this case). */
        for (mi = 0; mi < num_mod; mi++)
            if (mod_idx[mi] == oe_bmb) break;
        if (mi < num_mod) {
            ULONG bm_base = oe_bmb * blocks_inbitmap;
            sfs_bm_set_free(mod_buf[mi], bm_base, totalblocks-1);
            /* Free the new space (was marked 0/non-existent in old bitmap) */
            sfs_bm_mark_range(mod_buf[mi], bm_base, blocks_inbitmap,
                              totalblocks, new_totalblocks - 2, 1);
        }

        /* New end root → IN USE */
        for (mi = 0; mi < num_mod; mi++)
            if (mod_idx[mi] == ne_bmb) break;
        if (mi < num_mod)
            sfs_bm_set_used(mod_buf[mi], ne_bmb * blocks_inbitmap, new_totalblocks-1);

        SFS_PROGRESS("Writing bitmap blocks...");
        for (mi = 0; mi < num_mod; mi++) {
            ULONG blk = bitmapbase + mod_idx[mi];
            sfs_set_checksum(mod_buf[mi], sfs_blocksize);
            if (!sfs_write_block(bd, phys_base, blk, sfs_phys, mod_buf[mi])) {
                sprintf(err_buf, "Cannot write bitmap block %lu.", (unsigned long)blk);
                goto done;
            }
            mod_written[mi] = 1;
        }
    }

    /* ---------------------------------------------------------------- */
    /* Write new end root at new_totalblocks-1                           */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Writing new SFS end root...");
    {
        ULONG j;
        for (j = 0; j < sfs_blocksize; j++) buf_newend[j] = buf_root0[j];
        sfs_setl(buf_newend, SFS_RB_OWNBLOCK,    new_totalblocks - 1);
        sfs_setl(buf_newend, SFS_RB_TOTALBLOCKS,  new_totalblocks);
        sfs_setl(buf_newend, SFS_RB_LASTBYTEH,    new_lastbyteh);
        sfs_setl(buf_newend, SFS_RB_LASTBYTE,     new_lastbyte_lo);
        sfs_setl(buf_newend, SFS_RB_BITMAPBASE,   new_bitmapbase_for_root);
        sfs_setw(buf_newend, SFS_RB_SEQNUM, (UWORD)(new_seqnum + 1));
        sfs_set_checksum(buf_newend, sfs_blocksize);
        if (!sfs_write_block(bd, phys_base, new_totalblocks-1, sfs_phys, buf_newend)) {
            sprintf(err_buf, "Cannot write new SFS end root at block %lu.",
                    (unsigned long)(new_totalblocks-1));
            goto done;
        }
        rootend_written = 1;
    }

    /* ---------------------------------------------------------------- */
    /* Write updated start root (highest seqnum → authoritative)        */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Writing updated SFS start root...");
    sfs_setl(buf_root0, SFS_RB_TOTALBLOCKS, new_totalblocks);
    sfs_setl(buf_root0, SFS_RB_LASTBYTEH,   new_lastbyteh);
    sfs_setl(buf_root0, SFS_RB_LASTBYTE,    new_lastbyte_lo);
    sfs_setl(buf_root0, SFS_RB_BITMAPBASE,  new_bitmapbase_for_root);
    sfs_setw(buf_root0, SFS_RB_SEQNUM, (UWORD)(new_seqnum + 2));
    sfs_set_checksum(buf_root0, sfs_blocksize);
    if (!sfs_write_block(bd, phys_base, 0, sfs_phys, buf_root0)) {
        sprintf(err_buf, "Cannot write updated SFS start root.\n"
                "Run 'SFScheck %s:' after reboot.", pi->drive_name);
        goto done;
    }
    root0_written = 1;

    /* ---------------------------------------------------------------- */
    /* Invalidate old end root (totalblocks-1).                          */
    /* After a grow the old end root remains valid on disk.  On a warm   */
    /* reboot (Ctrl-Amiga-Amiga) the device driver may keep the OLD      */
    /* DosEnvec; SFS then finds the old end root (totalblocks matches),  */
    /* rejects our new start root (totalblocks mismatch), and uses the   */
    /* old bitmapbase → shows old free space.  Zeroing the block makes   */
    /* it permanently invalid regardless of DosEnvec.                    */
    /* Failure is non-fatal: the grow succeeded; warn in diagnostic.     */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Invalidating old end root...");
    {
        ULONG inv_j;
        for (inv_j = 0; inv_j < sfs_blocksize; inv_j++) buf_bmb_work[inv_j] = 0;
        /* Zero the immediate old end root (totalblocks-1). */
        if (sfs_write_block(bd, phys_base, totalblocks-1, sfs_phys, buf_bmb_work))
            old_end_inv = 1;
        /* If bitmapbase was relocated by a previous Strategy B grow,
           bitmapbase == that grow's old_totalblocks, so bitmapbase-1 is an
           EVEN OLDER end root that was never zeroed.  Zero it too if valid. */
        if (bitmapbase > 0 && (bitmapbase - 1) != (totalblocks - 1)) {
            ULONG older = bitmapbase - 1;
            if (sfs_read_block(bd, phys_base, older, sfs_phys, buf_bmb_read) &&
                sfs_getl(buf_bmb_read, SFS_RB_ID) == SFS_ROOT_ID &&
                sfs_verify_checksum(buf_bmb_read, sfs_blocksize) &&
                sfs_getl(buf_bmb_read, SFS_RB_OWNBLOCK) == older) {
                /* Valid old end root — zero it */
                if (sfs_write_block(bd, phys_base, older, sfs_phys, buf_bmb_work))
                    old_end_inv |= 2;
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* Update fsRootInfo.freeblocks in rootobjectcontainer (OBJC block).  */
    /* SFS caches the free block count here and uses it for ACTION_DISK_INFO */
    /* (Workbench Info requester).  Count from the now-updated bitmap so   */
    /* the value is exact.  Failure is non-fatal.                          */
    /* ---------------------------------------------------------------- */
    SFS_PROGRESS("Updating free block cache...");
    {
        ULONG k2, j2;
        ULONG freeblocks_off = sfs_blocksize - (ULONG)SFS_RI_FREEBLOCKS_FROM_END;

        /* Count set bits (=free) across all new_bmb_count bitmap blocks */
        for (k2 = 0; k2 < new_bmb_count; k2++) {
            const ULONG *bm32;
            ULONG n_ul;
            if (!sfs_read_block(bd, phys_base,
                                new_bitmapbase_for_root + k2,
                                sfs_phys, buf_bmb_work))
                break;
            bm32 = (const ULONG *)(buf_bmb_work + SFS_BM_HEADER_SIZE);
            n_ul = (sfs_blocksize - (ULONG)SFS_BM_HEADER_SIZE) / 4;
            for (j2 = 0; j2 < n_ul; j2++) {
                ULONG w = bm32[j2];
                w = w - ((w >> 1) & 0x55555555UL);
                w = (w & 0x33333333UL) + ((w >> 2) & 0x33333333UL);
                w = (w + (w >> 4)) & 0x0F0F0F0FUL;
                ri_free_counted += (w * 0x01010101UL) >> 24;
            }
        }

        if (k2 == new_bmb_count) {
            /* Read rootobjectcontainer block, patch freeblocks, write back */
            if (sfs_read_block(bd, phys_base, rootobj, sfs_phys, buf_bmb_read) &&
                sfs_verify_checksum(buf_bmb_read, sfs_blocksize) &&
                sfs_getl(buf_bmb_read, 0) == SFS_OBJC_ID) {
                objc_old_free = sfs_getl(buf_bmb_read, freeblocks_off);
                sfs_setl(buf_bmb_read, freeblocks_off, ri_free_counted);
                sfs_set_checksum(buf_bmb_read, sfs_blocksize);
                if (sfs_write_block(bd, phys_base, rootobj, sfs_phys, buf_bmb_read))
                    objc_updated = 1;
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* Post-write readback: verify root and count free bits in bitmap    */
    /* (done before Inhibit so we see what's actually on disk)          */
    /* ---------------------------------------------------------------- */
    {
        ULONG vrf_tb = 0, vrf_bm = 0, vrf_free = 0;
        ULONG vrf_j, vrf_n;
        const ULONG *vrf_bmp;

        SFS_PROGRESS("Verifying writes...");
        /* Read back root block 0 */
        if (sfs_read_block(bd, phys_base, 0, sfs_phys, buf_bmb_read)) {
            vrf_tb = sfs_getl(buf_bmb_read, SFS_RB_TOTALBLOCKS);
            vrf_bm = sfs_getl(buf_bmb_read, SFS_RB_BITMAPBASE);
        }
        /* Count free bits (=1 bits) in LAST OLD bitmap block — this is where
           the newly freed blocks appear (tail of old coverage + new space). */
        if (sfs_read_block(bd, phys_base,
                           new_bitmapbase_for_root + old_bmb_count - 1,
                           sfs_phys, buf_bmb_work)) {
            vrf_n   = (sfs_blocksize - (ULONG)SFS_BM_HEADER_SIZE) / 4;
            vrf_bmp = (const ULONG *)(buf_bmb_work + SFS_BM_HEADER_SIZE);
            for (vrf_j = 0; vrf_j < vrf_n; vrf_j++) {
                ULONG w = vrf_bmp[vrf_j];
                w = w - ((w >> 1) & 0x55555555UL);
                w = (w & 0x33333333UL) + ((w >> 2) & 0x33333333UL);
                w = (w + (w >> 4)) & 0x0F0F0F0FUL;
                vrf_free += (w * 0x01010101UL) >> 24;
            }
        }

        /* VRF2: verify first new bitmap block (if any) */
        {
            ULONG vrf2_free = 0;
            if (num_new_bmb > 0) {
                ULONG vrf2_blk = new_bitmapbase_for_root + old_bmb_count;
                if (sfs_read_block(bd, phys_base, vrf2_blk, sfs_phys,
                                   buf_bmb_work)) {
                    ULONG vrf2_n = (sfs_blocksize - (ULONG)SFS_BM_HEADER_SIZE) / 4;
                    const ULONG *vrf2_bmp =
                        (const ULONG *)(buf_bmb_work + SFS_BM_HEADER_SIZE);
                    ULONG vrf2_j;
                    for (vrf2_j = 0; vrf2_j < vrf2_n; vrf2_j++) {
                        ULONG w = vrf2_bmp[vrf2_j];
                        w = w - ((w >> 1) & 0x55555555UL);
                        w = (w & 0x33333333UL) + ((w >> 2) & 0x33333333UL);
                        w = (w + (w >> 4)) & 0x0F0F0F0FUL;
                        vrf2_free += (w * 0x01010101UL) >> 24;
                    }
                }
            }

            sprintf(err_buf,
                    "%s\n"
                    "cyl+%lu bpc=%lu delta=%lu\n"
                    "blks %lu->%lu\n"
                    "bmb_base %lu->%lu\n"
                    "VRF:tb=%lu bm=%lu bmbLFree=%lu\n"
                    "VRF2:newbmFree=%lu (exp %lu)\n"
                    "OldRoot: %s\n"
                    "FreeBlks: %s (%lu->%lu)\n"
                    "DH0: stays inhibited until reboot.",
                    use_relocation ? "Bitmap relocated."
                                   : "Bitmap extended.",
                    (unsigned long)cyl_diff,
                    (unsigned long)bpc,
                    (unsigned long)delta_sfs,
                    (unsigned long)totalblocks,
                    (unsigned long)new_totalblocks,
                    (unsigned long)bitmapbase,
                    (unsigned long)new_bitmapbase_for_root,
                    (unsigned long)vrf_tb,
                    (unsigned long)vrf_bm,
                    (unsigned long)vrf_free,
                    (unsigned long)vrf2_free,
                    (unsigned long)(num_new_bmb > 0 ?
                        blocks_inbitmap : 0),
                    old_end_inv ? "invalidated" : "WARN: not invalidated",
                    objc_updated ? "OK" : "WARN:fail",
                    (unsigned long)objc_old_free,
                    (unsigned long)ri_free_counted);
        }
    }

    ok = TRUE;

done:
    /* On success: leave partition INHIBITED so SFS cannot flush its stale
       in-memory root (old totalblocks) and overwrite our updated root block
       before the user reboots.  On cold boot SFS re-reads from disk and
       picks up our new root (highest sequencenumber).
       On failure: release inhibit so the partition is accessible again. */
    if (!ok && did_inhibit) Inhibit((STRPTR)inh_name, DOSFALSE);

    /* Rollback for natural placement: restore modified existing bitmap blocks */
    if (!ok && !use_relocation) {
        for (mi = num_mod - 1; mi >= 0; mi--)
            if (mod_written[mi] && mod_orig[mi])
                sfs_write_block(bd, phys_base, bitmapbase + mod_idx[mi],
                                sfs_phys, mod_orig[mi]);
        /* Rollback relocation: new bitmap blocks in extended area don't need
           rollback — restoring start root (below) makes old structure authoritative */
    }
    /* Always restore start root if it was overwritten */
    if (!ok && root0_written && buf_orig0)
        sfs_write_block(bd, phys_base, 0, sfs_phys, buf_orig0);

    if (buf_root0)    FreeVec(buf_root0);
    if (buf_orig0)    FreeVec(buf_orig0);
    if (buf_rootend)  FreeVec(buf_rootend);
    if (buf_newend)   FreeVec(buf_newend);
    if (buf_bmb_work) FreeVec(buf_bmb_work);
    if (buf_bmb_read) FreeVec(buf_bmb_read);
    for (mi = 0; mi < MAX_MOD_BMB; mi++) {
        if (mod_buf[mi])  FreeVec(mod_buf[mi]);
        if (mod_orig[mi]) FreeVec(mod_orig[mi]);
    }
    return ok;

#undef SFS_PROGRESS
}
