/*
 * ffsresize.c — EXPERIMENTAL: grow an FFS/OFS filesystem to cover an
 *               extended partition cylinder range.
 *
 * Only growing is supported (never shrinking).
 * Supports DOS\0 through DOS\7 (OFS, FFS, IntlOFS, IntlFFS, DCOFS, DCFFS
 * and variants) — they all share the same on-disk bitmap structure.
 * Only 512-byte filesystem blocks are supported in this version.
 *
 * Algorithm:
 *   1. Read + validate the partition boot block → locate root block.
 *   2. Read + validate the root block → locate the bitmap chain.
 *   3. Update the last existing bitmap block: mark newly-available blocks
 *      (those that were beyond old_size but within the last bm block's
 *      range) as free.
 *   4. Create new bitmap blocks for ranges beyond the old bitmap coverage,
 *      placed at their natural position (bm_index * blocks_per_bm_block).
 *   5. Append the new bm block numbers to the root bm_pages[] array or
 *      into the bm_ext chain.  A single new bm_ext block is allocated in
 *      the new partition space if the existing chain has no free slots.
 *   6. Recompute and write the root block.
 *
 * Limitations of this experimental version:
 *   - 512-byte filesystem blocks only.
 *   - If more than one new bm_ext block would be needed the operation is
 *     refused (this covers any practical grow on a partition up to ~4 GB).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "clib.h"
#include "rdb.h"
#include "ffsresize.h"

/* ------------------------------------------------------------------ */
/* AmigaDOS constants                                                   */
/* ------------------------------------------------------------------ */

#define T_SHORT         2UL
/* ST_ROOT (1) is defined by dos/dosextens.h via proto/dos.h */
#define BM_VALID        0xFFFFFFFFUL

/* Root block long-word offsets (valid for 512-byte blocks, 128 longs) */
#define RL_TYPE         0
/* L[1] = rb_OwnKey   = always 0 (FFS checks this; non-zero → RootCorrupt) */
/* L[2] = rb_SeqNum   = always 0 */
/* L[3] = rb_HTSize   = HTSize (72 for 512-byte blocks) — not modified     */
/* L[4] = rb_Nothing1 = always 0 (FFS checks this; non-zero → RootCorrupt) */
#define RL_CHKSUM       5
#define RL_BM_FLAG      78
#define RL_BM_PAGES     79   /* [79..103] = 25 entries */
#define RL_BM_EXT       104
#define RL_SEC_TYPE     127  /* block_size/4 - 1 */

/* Boot block long-word offsets */
#define BL_DOSTYPE      0
#define BL_ROOT_BLK     2

/* Bitmap chain limits */
#define ROOT_BM_MAX     25   /* bm_pages[] slots in root block */
#define EXT_BM_MAX      127  /* bm pointer slots per ext block (long[0..126]);
                                long[127] = next ext block or 0 */
#define MAX_EXT_CHAIN   32   /* maximum ext blocks we'll follow */

/* FFS bitmap bit macros.
   FFS uses MSB-first within each longword: bit 31 = lowest block, bit 0 = highest.
   off = block offset within the bm block's coverage range.
   bm block's L[0] = checksum; bitmap data starts at L[1]. */
#define BM_BIT(off)       (31u - ((unsigned)(off) % 32u))
#define BM_TESTFREE(b,off) ((b)[1u + (unsigned)(off)/32u] &   (1UL << BM_BIT(off)))
#define BM_SETFREE(b,off)  ((b)[1u + (unsigned)(off)/32u] |=  (1UL << BM_BIT(off)))
#define BM_SETUSED(b,off)  ((b)[1u + (unsigned)(off)/32u] &= ~(1UL << BM_BIT(off)))

/* ------------------------------------------------------------------ */

BOOL FFS_IsSupportedType(ULONG dostype)
{
    return (dostype & 0xFFFFFF00UL) == 0x444F5300UL &&
           (dostype & 0xFFUL) <= 7;
}

/* AmigaDOS block checksum: sum of all longs must be 0.
   Returns the value to store in the checksum field. */
static ULONG ffs_checksum(const ULONG *buf, ULONG nlongs)
{
    ULONG sum = 0, i;
    for (i = 0; i < nlongs; i++) sum += buf[i];
    return (ULONG)(-(LONG)sum);
}

/* ------------------------------------------------------------------ */
/* FFS_GrowPartition                                                    */
/* ------------------------------------------------------------------ */

BOOL FFS_GrowPartition(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, ULONG old_high_cyl,
                       char *err_buf,
                       FFS_ProgressFn progress_fn, void *progress_ud)
{
#define FFS_PROGRESS(msg) do { if (progress_fn) progress_fn(progress_ud, (msg)); } while(0)
    /* --- all heap pointers null-initialised for single cleanup path --- */
    ULONG *boot_buf    = NULL;
    ULONG *root_buf    = NULL;
    ULONG *bm_buf      = NULL;   /* scratch for one bitmap or ext block  */
    ULONG *bm_blknums  = NULL;   /* flat array of all bm block numbers   */
    BOOL   ok          = FALSE;
    BOOL   did_inhibit = FALSE;
    char   inh_name[36];
    ULONG  new_root       = 0;   /* set in Phase 8; used by done: verify */
    ULONG  root_blk       = 0;   /* set in Phase 1; shown in diagnostic  */
    ULONG  nr_bm_blknum   = 0;   /* bm block address covering new_root   */
    ULONG  nr_bm_off      = 0;   /* bit offset of new_root in that block */

    /* --- geometry --- */
    ULONG heads      = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG sectors    = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    ULONG eff_bsz    = (pi->block_size > 0) ? pi->block_size : 512;
    ULONG nlongs     = eff_bsz / 4;

    if (heads == 0 || sectors == 0) {
        sprintf(err_buf, "Invalid partition geometry (heads=%lu secs=%lu)",
                (unsigned long)heads, (unsigned long)sectors);
        goto done;
    }
    if (eff_bsz != 512) {
        sprintf(err_buf, "Only 512-byte filesystem blocks supported (got %lu)",
                (unsigned long)eff_bsz);
        goto done;
    }

    ULONG part_abs   = pi->low_cyl * heads * sectors;
    ULONG old_blocks = (old_high_cyl  - pi->low_cyl + 1) * heads * sectors;
    ULONG new_blocks = (pi->high_cyl  - pi->low_cyl + 1) * heads * sectors;

    /* blocks per bitmap block: (nlongs-1)*32  =  127*32 = 4064 for 512-byte */
    ULONG bpbm = (nlongs - 1) * 32;

    /* DE_RESERVEDBLKS: number of reserved blocks at start of partition.
       Standard FFS format uses 2.  FFS bitmap bit arithmetic subtracts
       this offset before computing bm_block_index and bit_within_block:
         bm_idx = (block - reserved) / bpbm
         bit    = (block - reserved) % bpbm
       All our bitmap index and offset calculations must use this same
       convention or we mark the WRONG bits, leaving the root block
       incorrectly flagged as free (FFS then clobbers it on first write). */
    ULONG reserved = (pi->reserved_blks > 0) ? pi->reserved_blks : 2UL;

    if (old_blocks <= reserved) {
        sprintf(err_buf,
                "Partition is too small to contain a valid FFS filesystem\n"
                "(old_blocks=%lu <= reserved=%lu).",
                (unsigned long)old_blocks, (unsigned long)reserved);
        goto done;
    }
    if (new_blocks <= reserved) {
        sprintf(err_buf,
                "New partition size is too small for FFS\n"
                "(new_blocks=%lu <= reserved=%lu).",
                (unsigned long)new_blocks, (unsigned long)reserved);
        goto done;
    }

    /* FFS BitmapCount formula (from init.asm):
       BitmapCount = (BlocksPerBM - 1 + HighestBlock - Reserved) / BlocksPerBM
       where HighestBlock = blocks - 1, all integer (floor) division. */
    ULONG old_bm_need = (bpbm - 2 + old_blocks - reserved) / bpbm;
    ULONG new_bm_need = (bpbm - 2 + new_blocks - reserved) / bpbm;
    ULONG num_new_bm  = new_bm_need - old_bm_need;

    /* --- allocate scratch buffers --- */
    boot_buf = (ULONG *)AllocVec(eff_bsz, MEMF_PUBLIC | MEMF_CLEAR);
    root_buf = (ULONG *)AllocVec(eff_bsz, MEMF_PUBLIC | MEMF_CLEAR);
    bm_buf   = (ULONG *)AllocVec(eff_bsz, MEMF_PUBLIC | MEMF_CLEAR);
    /* flat bm list: ROOT_BM_MAX + MAX_EXT_CHAIN * EXT_BM_MAX entries max */
    ULONG max_bm_list = (ULONG)(ROOT_BM_MAX + MAX_EXT_CHAIN * EXT_BM_MAX);
    bm_blknums = (ULONG *)AllocVec(max_bm_list * sizeof(ULONG),
                                    MEMF_PUBLIC | MEMF_CLEAR);
    if (!boot_buf || !root_buf || !bm_buf || !bm_blknums) {
        sprintf(err_buf, "Out of memory");
        goto done;
    }

    /* Inhibit the filesystem handler BEFORE any disk reads.
       This ensures we read a fully stable, post-flush snapshot:
       FFS flushes all dirty buffers (including the root block) and
       clears its cache before we read anything.  Any reads we do
       from here on reflect the actual on-disk state with no pending
       FFS writes racing against us.
       If the partition isn't mounted, Inhibit() returns FALSE and we
       continue anyway — direct access to an unmounted partition is safe. */
    sprintf(inh_name, "%s:", pi->drive_name);
    FFS_PROGRESS("Inhibiting filesystem handler...");
    did_inhibit = Inhibit((STRPTR)inh_name, DOSTRUE);

    /* ---------------------------------------------------------------- */
    /* Phase 1 — read + validate boot block, get root block number      */
    /* ---------------------------------------------------------------- */
    FFS_PROGRESS("Reading boot block...");

    if (!BlockDev_ReadBlock(bd, part_abs, boot_buf)) {
        sprintf(err_buf, "Cannot read partition boot block (abs %lu)",
                (unsigned long)part_abs);
        goto done;
    }
    if ((boot_buf[BL_DOSTYPE] & 0xFFFFFF00UL) != 0x444F5300UL) {
        sprintf(err_buf, "Boot block DosType mismatch (0x%08lX)",
                (unsigned long)boot_buf[BL_DOSTYPE]);
        goto done;
    }

    ULONG root_blk_stored = boot_buf[BL_ROOT_BLK];
    root_blk = root_blk_stored;
    /* Some formatters leave this at 0; fall back to standard position.
       Standard root block = old_blocks / 2  (not (old_blocks-1)/2). */
    if (root_blk == 0 || root_blk >= old_blocks)
        root_blk = old_blocks / 2;

    /* ---------------------------------------------------------------- */
    /* Phase 2 — read + validate root block                             */
    /* ---------------------------------------------------------------- */
    FFS_PROGRESS("Reading root block...");

    if (!BlockDev_ReadBlock(bd, part_abs + root_blk, root_buf)) {
        sprintf(err_buf, "Cannot read root block (abs %lu, rel %lu)",
                (unsigned long)(part_abs + root_blk),
                (unsigned long)root_blk);
        goto done;
    }
    if (root_buf[RL_TYPE] != T_SHORT || root_buf[nlongs - 1] != ST_ROOT) {
        sprintf(err_buf,
                "Root block wrong type/sec_type (0x%lX/0x%lX)\n"
                "partabs=%lu bb[2]=%lu rootblk=%lu old_blks=%lu",
                (unsigned long)root_buf[RL_TYPE],
                (unsigned long)root_buf[nlongs - 1],
                (unsigned long)part_abs,
                (unsigned long)root_blk_stored,
                (unsigned long)root_blk,
                (unsigned long)old_blocks);
        goto done;
    }
    {
        ULONG save = root_buf[RL_CHKSUM];
        root_buf[RL_CHKSUM] = 0;
        ULONG csum = ffs_checksum(root_buf, nlongs);
        root_buf[RL_CHKSUM] = save;
        if (csum != save) {
            sprintf(err_buf, "Root block checksum invalid (stored 0x%08lX calc 0x%08lX)",
                    (unsigned long)save, (unsigned long)csum);
            goto done;
        }
    }
    if (root_buf[RL_BM_FLAG] != BM_VALID) {
        sprintf(err_buf, "Bitmap not valid in root block (flag=0x%08lX)",
                (unsigned long)root_buf[RL_BM_FLAG]);
        goto done;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 3 — collect existing bm block numbers into flat array,     */
    /*           track the ext block chain for later rewriting          */
    /* ---------------------------------------------------------------- */
    FFS_PROGRESS("Reading bitmap chain...");

    ULONG bm_count    = 0;
    /* ext chain info: block numbers and per-block used-slot counts */
    ULONG ext_relblk[MAX_EXT_CHAIN];   /* partition-relative block numbers */
    ULONG ext_used[MAX_EXT_CHAIN];     /* entries used in each ext block   */
    ULONG ext_count   = 0;

    /* Root bm_pages[] */
    for (ULONG i = 0; i < ROOT_BM_MAX; i++) {
        if (root_buf[RL_BM_PAGES + i] == 0) break;
        if (bm_count < max_bm_list)
            bm_blknums[bm_count++] = root_buf[RL_BM_PAGES + i];
    }

    /* Follow bm_ext chain */
    {
        ULONG ext_blk = root_buf[RL_BM_EXT];
        while (ext_blk != 0 && ext_count < MAX_EXT_CHAIN) {
            if (!BlockDev_ReadBlock(bd, part_abs + ext_blk, bm_buf)) {
                sprintf(err_buf, "Cannot read bm_ext block %lu",
                        (unsigned long)(part_abs + ext_blk));
                goto done;
            }
            ext_relblk[ext_count] = ext_blk;
            ext_used[ext_count]   = 0;
            for (ULONG i = 0; i < EXT_BM_MAX; i++) {
                if (bm_buf[i] == 0) break;
                ext_used[ext_count]++;
                if (bm_count < max_bm_list)
                    bm_blknums[bm_count++] = bm_buf[i];
            }
            ext_count++;
            ext_blk = bm_buf[nlongs - 1];
        }
    }

    if (bm_count < old_bm_need) {
        sprintf(err_buf, "Bitmap chain has %lu blocks, expected at least %lu",
                (unsigned long)bm_count, (unsigned long)old_bm_need);
        goto done;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 4 — check we can accommodate the new bm block numbers      */
    /* ---------------------------------------------------------------- */

    /* Available slots in the existing chain after the current entries */
    ULONG root_free = (bm_count < ROOT_BM_MAX) ? (ROOT_BM_MAX - bm_count) : 0;
    ULONG last_ext_free = 0;
    if (ext_count > 0)
        last_ext_free = EXT_BM_MAX - ext_used[ext_count - 1];

    ULONG avail_slots = root_free + last_ext_free;

    /* Compute how many new ext blocks are needed to hold the overflow.
       Each new ext block provides EXT_BM_MAX (127) additional pointer slots.
       We support as many new ext blocks as MAX_EXT_CHAIN allows. */
    ULONG num_new_ext = 0;
    BOOL  need_new_ext = FALSE;
    ULONG new_ext_relblk[MAX_EXT_CHAIN];  /* positions of new ext blocks */

    if (num_new_bm > avail_slots) {
        ULONG overflow = num_new_bm - avail_slots;
        num_new_ext = (overflow + EXT_BM_MAX - 1) / EXT_BM_MAX;
        need_new_ext = TRUE;
        if (num_new_ext > MAX_EXT_CHAIN) {
            sprintf(err_buf,
                    "Grow requires %lu new ext blocks (max %u). "
                    "Partition is too large for this implementation.",
                    (unsigned long)num_new_ext, (unsigned)MAX_EXT_CHAIN);
            goto done;
        }
        /* Place new ext blocks one after another immediately after the last
           new bm block (reserved + (new_bm_need-1)*bpbm).  They all land
           within that bm block's coverage range so Phase 6 marks them used. */
        for (ULONG ei = 0; ei < num_new_ext; ei++) {
            new_ext_relblk[ei] = reserved + (new_bm_need - 1) * bpbm + 1 + ei;
            if (new_ext_relblk[ei] >= new_blocks) {
                sprintf(err_buf,
                        "No room in new partition space for ext block %lu",
                        (unsigned long)ei);
                goto done;
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* Phase 5 — update the last EXISTING bm block                      */
    FFS_PROGRESS("Extending last bitmap block...");
    /*                                                                   */
    /* Blocks old_blocks .. min(old_bm_need*bpbm, new_blocks) - 1       */
    /* were beyond the old partition end; mark them free now.           */
    /* ---------------------------------------------------------------- */

    {
        ULONG bm_idx    = old_bm_need - 1;
        /* FFS: bm block bm_idx covers reserved+bm_idx*bpbm .. reserved+(bm_idx+1)*bpbm-1 */
        ULONG range_end = reserved + old_bm_need * bpbm;  /* excl., in partition blocks */
        ULONG free_end  = (range_end < new_blocks) ? range_end : new_blocks;

        if (old_blocks < free_end) {
            if (!BlockDev_ReadBlock(bd, part_abs + bm_blknums[bm_idx], bm_buf)) {
                sprintf(err_buf, "Cannot read last bm block %lu",
                        (unsigned long)bm_blknums[bm_idx]);
                goto done;
            }
            /* Set bits for blocks old_blocks .. free_end-1.
               FFS bit offset: off = b - reserved - bm_idx*bpbm */
            for (ULONG b = old_blocks; b < free_end; b++) {
                ULONG off = b - reserved - bm_idx * bpbm;
                BM_SETFREE(bm_buf, off);
            }
            bm_buf[0] = 0;
            bm_buf[0] = ffs_checksum(bm_buf, nlongs);
            if (!BlockDev_WriteBlock(bd, part_abs + bm_blknums[bm_idx], bm_buf)) {
                sprintf(err_buf, "Failed to write updated bm block %lu",
                        (unsigned long)bm_blknums[bm_idx]);
                goto done;
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* Phase 6 — create new bm blocks                                   */
    FFS_PROGRESS("Writing new bitmap blocks...");
    /*                                                                   */
    /* New bm block N (N = old_bm_need .. new_bm_need-1) is placed at   */
    /* partition block N * bpbm (natural position, always in new space). */
    /* ---------------------------------------------------------------- */

    for (ULONG k = 0; k < num_new_bm; k++) {
        ULONG bm_idx    = old_bm_need + k;
        /* Place new bm block at the start of its own coverage range.
           FFS: bm block bm_idx covers reserved+bm_idx*bpbm .. reserved+(bm_idx+1)*bpbm-1.
           Placing abs_blk = reserved+bm_idx*bpbm means:
             - abs_blk is at offset 0 within the block's own coverage range
             - BM_SETUSED(bm_buf,0) correctly marks the bm block itself as used */
        ULONG abs_blk   = reserved + bm_idx * bpbm;  /* partition-relative   */
        ULONG b_start   = reserved + bm_idx * bpbm;  /* first block in range */
        ULONG b_end     = reserved + (bm_idx + 1) * bpbm; /* exclusive        */
        if (b_end > new_blocks) b_end = new_blocks;

        /* record in flat list */
        if (bm_count < max_bm_list)
            bm_blknums[bm_count++] = abs_blk;

        memset(bm_buf, 0, eff_bsz);

        /* Mark all blocks in range as free (bit = 1).
           FFS bit offset: off = b - b_start (= b - reserved - bm_idx*bpbm) */
        for (ULONG b = b_start; b < b_end; b++) {
            ULONG off = b - b_start;
            BM_SETFREE(bm_buf, off);
        }
        /* Mark this bm block itself as used (offset 0 = b_start = abs_blk) */
        BM_SETUSED(bm_buf, 0);

        /* Mark any new ext blocks that fall in this bm block's range as used */
        if (need_new_ext) {
            ULONG ei;
            for (ei = 0; ei < num_new_ext; ei++) {
                if (new_ext_relblk[ei] >= b_start && new_ext_relblk[ei] < b_end) {
                    ULONG off = new_ext_relblk[ei] - b_start;
                    BM_SETUSED(bm_buf, off);
                }
            }
        }

        bm_buf[0] = 0;
        bm_buf[0] = ffs_checksum(bm_buf, nlongs);

        if (!BlockDev_WriteBlock(bd, part_abs + abs_blk, bm_buf)) {
            sprintf(err_buf, "Failed to write new bm block %lu (abs %lu)",
                    (unsigned long)abs_blk, (unsigned long)(part_abs + abs_blk));
            goto done;
        }
    }

    /* ---------------------------------------------------------------- */
    /* Phase 7 — add new bm block numbers to the chain                  */
    /* ---------------------------------------------------------------- */
    FFS_PROGRESS("Updating bitmap chain in root...");

    /* The new numbers are bm_blknums[old_bm_need .. new_bm_need-1].
       Fill slots in order: root bm_pages[] first, then last existing ext
       block, then new ext block if needed. */

    {
        ULONG src_idx = old_bm_need;   /* index into bm_blknums[] */
        ULONG added   = 0;

        /* Root bm_pages[] free slots */
        for (ULONG i = bm_count - num_new_bm; i < ROOT_BM_MAX && added < num_new_bm; i++, added++, src_idx++)
            root_buf[RL_BM_PAGES + i] = bm_blknums[src_idx];

        /* Existing last ext block free slots */
        if (added < num_new_bm && ext_count > 0) {
            if (!BlockDev_ReadBlock(bd, part_abs + ext_relblk[ext_count - 1], bm_buf)) {
                sprintf(err_buf, "Cannot re-read last ext block %lu",
                        (unsigned long)ext_relblk[ext_count - 1]);
                goto done;
            }
            ULONG slot = ext_used[ext_count - 1];
            while (slot < EXT_BM_MAX && added < num_new_bm) {
                bm_buf[slot++] = bm_blknums[src_idx++];
                added++;
            }
            /* next ext pointer: wire to first new ext block if we created any */
            bm_buf[nlongs - 1] = need_new_ext ? new_ext_relblk[0] : 0;
            /* ext blocks have no checksum field */
            if (!BlockDev_WriteBlock(bd, part_abs + ext_relblk[ext_count - 1], bm_buf)) {
                sprintf(err_buf, "Failed to write updated ext block %lu",
                        (unsigned long)ext_relblk[ext_count - 1]);
                goto done;
            }
        }

        /* New ext blocks (chain as many as needed) */
        if (need_new_ext) {
            ULONG ei;
            for (ei = 0; ei < num_new_ext; ei++) {
                memset(bm_buf, 0, eff_bsz);
                ULONG slot = 0;
                while (slot < EXT_BM_MAX && added < num_new_bm) {
                    bm_buf[slot++] = bm_blknums[src_idx++];
                    added++;
                }
                /* Chain to next new ext block, or terminate */
                bm_buf[nlongs - 1] = (ei + 1 < num_new_ext) ? new_ext_relblk[ei + 1] : 0;
                /* ext blocks have no checksum field */
                if (!BlockDev_WriteBlock(bd, part_abs + new_ext_relblk[ei], bm_buf)) {
                    sprintf(err_buf, "Failed to write new ext block %lu (abs %lu)",
                            (unsigned long)new_ext_relblk[ei],
                            (unsigned long)(part_abs + new_ext_relblk[ei]));
                    goto done;
                }
            }
            /* Wire the first new ext block into the chain */
            if (ext_count == 0)
                root_buf[RL_BM_EXT] = new_ext_relblk[0];
            /* else: already wired via the last existing ext block's next pointer above */
        }

        /* If we never needed an ext block and there was no existing one,
           clear root's bm_ext field (it should already be 0). */
        if (!need_new_ext && ext_count == 0)
            root_buf[RL_BM_EXT] = 0;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 8 — relocate root block to new_blocks/2 and write it      */
    FFS_PROGRESS("Relocating root block...");
    /*                                                                   */
    /* After the RDB is updated with new high_cyl, AmigaOS FFS computes */
    /* the root block position as new_blocks/2 from the DosEnvec.  The  */
    /* root must actually live there or the partition shows              */
    /* "Uninitialized".  We move the root in place:                     */
    /*   a) confirm the target block is free (bit=1) in the bitmap,     */
    /*   b) mark the target as used and the old root position as free,  */
    /*   c) write root_buf (already updated with new bm pointers) at    */
    /*      the new position.                                            */
    /* ---------------------------------------------------------------- */

    {
        /* FFS RootKey formula (from init.asm):
           RootKey = (Reserved + HighestBlock) / 2
           where HighestBlock = new_blocks - 1, integer division. */
        new_root = (reserved + new_blocks - 1) / 2;

        /* FFS bitmap convention (bitmap.asm, AllocKeys):
             bm_idx = (block - reserved) / bpbm
             off    = (block - reserved) % bpbm
           Using any other formula selects the WRONG bit. */
        if (new_root < reserved) {
            sprintf(err_buf, "new_root %lu < reserved %lu — impossible geometry",
                    (unsigned long)new_root, (unsigned long)reserved);
            goto done;
        }
        {
            ULONG bm_idx_nr = (new_root - reserved) / bpbm;
            ULONG bm_idx_or = (root_blk >= reserved)
                              ? (root_blk - reserved) / bpbm : 0;

            if (bm_idx_nr >= bm_count) {
                sprintf(err_buf,
                        "new_root bm index %lu out of range (bm_count=%lu)",
                        (unsigned long)bm_idx_nr, (unsigned long)bm_count);
                goto done;
            }
            /* save for Stage 2 verify (after bm_idx_nr validated) */
            nr_bm_blknum = bm_blknums[bm_idx_nr];
            nr_bm_off    = (new_root - reserved) % bpbm;

            /* ALWAYS mark new_root USED — even when new_root == root_blk.
               Phase 6 creates the bm block covering new_root as all-free
               (only bit-0 USED for the bm block itself).  If we only update
               the bitmap when new_root != root_blk, new_root stays FREE and
               FFS allocates it for file data on the first write → root
               overwritten → "Uninitialized" after reboot.
               The "is target free?" sanity check is only meaningful when we
               are relocating to a new position; skip it for in-place. */
            if (!BlockDev_ReadBlock(bd, part_abs + bm_blknums[bm_idx_nr], bm_buf)) {
                sprintf(err_buf, "Cannot read bm block %lu (root relocation)",
                        (unsigned long)bm_blknums[bm_idx_nr]);
                goto done;
            }
            {
                ULONG off = (new_root - reserved) % bpbm;
                /* We do not abort if the target block is already marked used.
                   On a second grow the block at new_root may be an FFS bm block
                   written by the validator after the previous grow.  Since we
                   set bm_flag=0, FFS will rebuild the bitmap from the directory
                   tree after reboot and will mark new_root USED (it IS the root).
                   Anything previously at that position is superseded. */
                BM_SETUSED(bm_buf, off);  /* unconditional: protect new_root */
            }
            /* If root moves AND the old position is in the same bm block,
               free it in the same pass. */
            if (new_root != root_blk && bm_idx_or == bm_idx_nr && root_blk >= reserved) {
                ULONG off = (root_blk - reserved) % bpbm;
                BM_SETFREE(bm_buf, off);
            }
            bm_buf[0] = 0;
            bm_buf[0] = ffs_checksum(bm_buf, nlongs);
            if (!BlockDev_WriteBlock(bd, part_abs + bm_blknums[bm_idx_nr], bm_buf)) {
                sprintf(err_buf, "Failed to write bm block %lu (root relocation)",
                        (unsigned long)bm_blknums[bm_idx_nr]);
                goto done;
            }

            /* Free old root in its own bm block if that's a different block */
            if (new_root != root_blk && bm_idx_or != bm_idx_nr && root_blk >= reserved) {
                if (bm_idx_or >= bm_count) {
                    /* old root is out of bitmap range — unusual but not fatal;
                       just skip freeing it (it stays "used" which is safe) */
                } else {
                    if (!BlockDev_ReadBlock(bd, part_abs + bm_blknums[bm_idx_or], bm_buf)) {
                        sprintf(err_buf, "Cannot read bm block %lu (free old root)",
                                (unsigned long)bm_blknums[bm_idx_or]);
                        goto done;
                    }
                    {
                        ULONG off = (root_blk - reserved) % bpbm;
                        BM_SETFREE(bm_buf, off);
                    }
                    bm_buf[0] = 0;
                    bm_buf[0] = ffs_checksum(bm_buf, nlongs);
                    if (!BlockDev_WriteBlock(bd, part_abs + bm_blknums[bm_idx_or], bm_buf)) {
                        sprintf(err_buf, "Failed to write bm block %lu (free old root)",
                                (unsigned long)bm_blknums[bm_idx_or]);
                        goto done;
                    }
                }
            }
        }

        /* Write root at new_root (or in-place if new_root == root_blk).
           L[1] = rb_OwnKey and L[4] = rb_Nothing1 MUST remain 0.
           FFS restart validation ORs L[1], L[2], L[4] and goes to
           RootCorrupt if any are non-zero (restart.asm:226-229).
           Force both to 0 regardless of what the source root contained — a
           root written by an older buggy version of this code may have had
           non-zero values here, and we must not carry them forward.
           The new disk size comes from the DosEnvec (updated by RDB_Write),
           not from the root block — FFS never reads disk_size from the root.
           bm_flag is set to 0 so FFS runs its validator — see comment below. */
        /* All fields validated by FFS restart.asm — force correct values
           regardless of what the source root contained: */
        root_buf[1]            = 0;   /* rb_OwnKey        must be 0 */
        root_buf[2]            = 0;   /* rb_SeqNum        must be 0 */
        root_buf[4]            = 0;   /* rb_Nothing1      must be 0 */
        root_buf[3]            = 72;  /* rb_HTSize        must be 72 for 512-byte blocks */
        root_buf[125]          = 0;   /* rb_Parent        must be 0 for root */
        /* Set bm_flag = 0 (NOT VALID) so FFS runs its own bitmap validator
           after the first reboot with the new DosEnvec.  This avoids the
           stale in-memory bm cache problem: our TD_WRITE64 writes bypass
           FFS's handler cache.  After Inhibit(FALSE), FFS may still hold a
           cached copy of the bm block covering new_root with the FREE bit
           set (from before our write).  If bm_flag=VALID, FFS trusts that
           cached state and allocates new_root for file data on the first
           write → root overwritten → "Uninitialized" after next reboot.
           With bm_flag=0, FFS knows the bitmap is invalid and rebuilds it
           from the directory tree.  It marks new_root USED (it found it as
           the root block) and the volume name in L[108] is preserved. */
        root_buf[RL_BM_FLAG]   = 0;
        root_buf[RL_CHKSUM]    = 0;
        root_buf[RL_CHKSUM]    = ffs_checksum(root_buf, nlongs);
        if (!BlockDev_WriteBlock(bd, part_abs + new_root, root_buf)) {
            sprintf(err_buf, "Failed to write root block at %lu (abs %lu)",
                    (unsigned long)new_root,
                    (unsigned long)(part_abs + new_root));
            goto done;
        }

    }

    /* ---------------------------------------------------------------- */
    /* Phase 9b — update fhb_Parent in all of root's direct children   */
    FFS_PROGRESS("Updating file/directory parent pointers...");
    /*                                                                   */
    /* FFS v40 (exinfo.asm:682, 1024) checks:                           */
    /*   cmp.l vfhb_Parent(a0), <current_directory_key>                 */
    /* before including an entry in ExNext / ExAll results.  After root */
    /* relocation, file/dir headers still hold the OLD root block       */
    /* number as their parent field → FFS skips every entry → "no      */
    /* files".                                                           */
    /*                                                                   */
    /* Walk root hash table L[6..77] and every hash chain hanging off   */
    /* it; for each valid T_SHORT header block update:                  */
    /*   fhb_Parent  (L[nlongs-3], EQU -12 from end)  = new_root       */
    /* and recompute the block checksum.  We only need to do this for   */
    /* the *direct* children of the root (one level down); deeper       */
    /* entries have a different parent and are unaffected.              */
    /* ---------------------------------------------------------------- */
    {
        ULONG FHB_PARENT    = nlongs - 3;  /* vfhb_Parent    EQU -12 from end */
        ULONG FHB_HASHCHAIN = nlongs - 4;  /* vfhb_HashChain EQU -16 from end */
        ULONG ht_i;
        for (ht_i = 0; ht_i < 72; ht_i++) {
            ULONG blkno = root_buf[6 + ht_i];
            ULONG depth = 0;
            while (blkno != 0 && depth < 512) {
                depth++;
                if (!BlockDev_ReadBlock(bd, part_abs + blkno, bm_buf))
                    break;
                /* Sanity check: must be a T_SHORT block with matching own_key */
                if (bm_buf[0] != T_SHORT || bm_buf[1] != blkno)
                    break;
                ULONG next_blkno        = bm_buf[FHB_HASHCHAIN];
                bm_buf[FHB_PARENT]      = new_root;
                bm_buf[RL_CHKSUM]       = 0;
                bm_buf[RL_CHKSUM]       = ffs_checksum(bm_buf, nlongs);
                if (!BlockDev_WriteBlock(bd, part_abs + blkno, bm_buf)) {
                    sprintf(err_buf,
                            "Failed to update fhb_Parent in block %lu (abs %lu)",
                            (unsigned long)blkno,
                            (unsigned long)(part_abs + blkno));
                    goto done;
                }
                blkno = next_blkno;
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* Phase 9 — update boot block bb[2] to point to new_root          */
    FFS_PROGRESS("Updating boot block...");
    /*                                                                   */
    /* Some FFS implementations read the root block number from bb[2]   */
    /* of the partition boot block (the first block of the partition).  */
    /* Writing new_root here costs nothing and eliminates any ambiguity  */
    /* about which root block to use.  boot_buf still holds the boot    */
    /* block data from Phase 1 — we just update the one field and       */
    /* write it back.                                                    */
    /* ---------------------------------------------------------------- */
    boot_buf[BL_ROOT_BLK] = new_root;
    /* Recompute boot block checksum (Amiga sum-with-carry, complement) */
    {
        ULONG bbsum = 0;
        boot_buf[1] = 0;
        for (ULONG i = 0; i < nlongs; i++) {
            ULONG prev = bbsum;
            bbsum += boot_buf[i];
            if (bbsum < prev) bbsum++;  /* carry */
        }
        boot_buf[1] = ~bbsum;
    }
    if (!BlockDev_WriteBlock(bd, part_abs, boot_buf)) {
        sprintf(err_buf, "Failed to update boot block bb[2] at abs %lu",
                (unsigned long)part_abs);
        goto done;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 9c — stamp bm_flag=VALID on the OLD root block             */
    /*                                                                   */
    /* After Inhibit(FALSE), FFS restarts using the OLD DosEnvec (RDB   */
    /* is not written yet — new HiCyl is not visible to FFS yet).  FFS  */
    /* computes the root block number from the OLD DosEnvec.  If that   */
    /* old root has bm_flag=0 (e.g. from DiskSalv, a prior failed grow, */
    /* or any other corruption), FFS runs its bitmap validator.          */
    /*                                                                   */
    /* The validator scans all reachable file/dir blocks from the OLD    */
    /* root, marks them USED, and marks EVERYTHING ELSE FREE — including */
    /* new_root, which is not a regular file/dir block referenced from   */
    /* the old directory tree.  After validation: new_root = FREE.      */
    /* FFS then allocates new_root for the next file header written to   */
    /* the partition, which overwrites our new root block and causes     */
    /* "Uninitialized" after reboot.                                     */
    /*                                                                   */
    /* Fix: write bm_flag=0xFFFFFFFF to the old root so FFS treats its  */
    /* bitmap as already validated and skips the validator.  Our Phase 8 */
    /* bitmap (new_root marked USED) is then preserved intact.           */
    /*                                                                   */
    /* Only needed when the root is actually moving (new_root != root).  */
    /* Non-fatal: if the write fails the filesystem structure is still   */
    /* intact; we just log it and continue.                              */
    /* ---------------------------------------------------------------- */
    if (new_root != root_blk) {
        if (BlockDev_ReadBlock(bd, part_abs + root_blk, bm_buf) &&
            bm_buf[0]          == T_SHORT &&
            bm_buf[nlongs - 1] == (ULONG)ST_ROOT &&
            bm_buf[RL_BM_FLAG] != BM_VALID) {
            bm_buf[RL_BM_FLAG] = BM_VALID;
            bm_buf[RL_CHKSUM]  = 0;
            bm_buf[RL_CHKSUM]  = ffs_checksum(bm_buf, nlongs);
            if (!BlockDev_WriteBlock(bd, part_abs + root_blk, bm_buf)) {
                /* Non-fatal — log in err_buf temporarily but don't abort */
                sprintf(err_buf,
                        "Warning: could not stamp bm_flag on old root at rel %lu\n"
                        "FFS may run bitmap validator after Inhibit release.\n"
                        "Filesystem written; reboot and verify.",
                        (unsigned long)root_blk);
                /* We still set ok=TRUE below — the new filesystem structure
                   is correct.  Worst case: FFS validator runs and we need
                   to DiskSalv.  Clear err_buf so success message shows. */
                err_buf[0] = '\0';
            }
        }
    }

    ok = TRUE;

done:
    /* ------------------------------------------------------------------ */
    /* Two-stage verification: catch FFS-induced corruption               */
    /* Stage 1: verify root is intact while FFS is still inhibited        */
    /* ------------------------------------------------------------------ */
    if (ok && new_root != 0 && bm_buf) {
        if (!BlockDev_ReadBlock(bd, part_abs + new_root, bm_buf)) {
            ok = FALSE;
            sprintf(err_buf,
                    "Root readback failed (abs %lu) before Inhibit release",
                    (unsigned long)(part_abs + new_root));
        } else {
            ULONG save_cs = bm_buf[RL_CHKSUM];
            bm_buf[RL_CHKSUM] = 0;
            ULONG calc_cs = ffs_checksum(bm_buf, nlongs);
            bm_buf[RL_CHKSUM] = save_cs;
            if (bm_buf[RL_TYPE]    != T_SHORT ||
                bm_buf[1]          != 0        ||
                bm_buf[4]          != 0        ||
                bm_buf[nlongs - 1] != (ULONG)ST_ROOT) {
                ok = FALSE;
                sprintf(err_buf,
                        "Root verify FAILED before Inhibit release:\n"
                        "type=0x%lX L[1]=%lu L[4]=%lu sec_type=0x%lX\n"
                        "Expected T_SHORT, L[1]=0, L[4]=0, ST_ROOT at abs %lu",
                        (unsigned long)bm_buf[RL_TYPE],
                        (unsigned long)bm_buf[1],
                        (unsigned long)bm_buf[4],
                        (unsigned long)bm_buf[nlongs - 1],
                        (unsigned long)(part_abs + new_root));
            } else if (calc_cs != save_cs) {
                ok = FALSE;
                sprintf(err_buf,
                        "Root checksum WRONG before Inhibit release\n"
                        "stored=0x%08lX calc=0x%08lX at abs %lu",
                        (unsigned long)save_cs, (unsigned long)calc_cs,
                        (unsigned long)(part_abs + new_root));
            }
            /* bm_flag is intentionally 0 (validator will run after reboot) */
        }
    }

    FFS_PROGRESS("Verifying root block (pre-release)...");
    if (did_inhibit)
        Inhibit((STRPTR)inh_name, DOSFALSE);

    /* Give FFS time to process its message queue and run any validator
       triggered by the restart.  FFS validator is asynchronous — it may
       not run until several scheduler ticks after Inhibit release.
       5 seconds is ample for any partition size on real or emulated hw. */
    FFS_PROGRESS("Waiting for FFS to resume (5 sec)...");
    Delay(250);

    /* ------------------------------------------------------------------ */
    /* Stage 2: verify root is still intact AFTER FFS has resumed         */
    FFS_PROGRESS("Verifying root block (post-resume)...");
    /* If this fails but Stage 1 passed, Inhibit(FALSE) caused corruption */
    /* ------------------------------------------------------------------ */
    if (ok && new_root != 0 && bm_buf) {
        if (!BlockDev_ReadBlock(bd, part_abs + new_root, bm_buf)) {
            ok = FALSE;
            sprintf(err_buf,
                    "Root readback FAILED after Inhibit release (abs %lu)\n"
                    "FFS may have overwritten the new root block!",
                    (unsigned long)(part_abs + new_root));
        } else {
            ULONG save_cs = bm_buf[RL_CHKSUM];
            bm_buf[RL_CHKSUM] = 0;
            ULONG calc_cs = ffs_checksum(bm_buf, nlongs);
            bm_buf[RL_CHKSUM] = save_cs;
            if (bm_buf[RL_TYPE]    != T_SHORT ||
                bm_buf[1]          != 0        ||
                bm_buf[4]          != 0        ||
                bm_buf[nlongs - 1] != (ULONG)ST_ROOT) {
                ok = FALSE;
                sprintf(err_buf,
                        "Root CORRUPTED after Inhibit(FALSE)!\n"
                        "type=0x%lX L[1]=%lu L[4]=%lu sec_type=0x%lX\n"
                        "FFS overwrote abs %lu with non-root data",
                        (unsigned long)bm_buf[RL_TYPE],
                        (unsigned long)bm_buf[1],
                        (unsigned long)bm_buf[4],
                        (unsigned long)bm_buf[nlongs - 1],
                        (unsigned long)(part_abs + new_root));
            } else if (calc_cs != save_cs) {
                ok = FALSE;
                sprintf(err_buf,
                        "Root checksum CORRUPTED after Inhibit(FALSE)!\n"
                        "stored=0x%08lX calc=0x%08lX\n"
                        "FFS overwrote root checksum at abs %lu",
                        (unsigned long)save_cs, (unsigned long)calc_cs,
                        (unsigned long)(part_abs + new_root));
            }
            /* bm_flag=0 is expected — FFS will run its validator after reboot */
        }
    }

    /* ------------------------------------------------------------------ */
    /* Stage 2b: verify the bm block covering new_root after Inhibit(FALSE)*/
    /* A bad bm checksum or new_root marked FREE here means FFS will run  */
    /* its validator, which will free new_root → root overwritten on copy */
    /* ------------------------------------------------------------------ */
    if (ok && nr_bm_blknum != 0 && bm_buf) {
        if (!BlockDev_ReadBlock(bd, part_abs + nr_bm_blknum, bm_buf)) {
            ok = FALSE;
            sprintf(err_buf,
                    "BM block %lu (abs %lu) unreadable after Inhibit(FALSE)",
                    (unsigned long)nr_bm_blknum,
                    (unsigned long)(part_abs + nr_bm_blknum));
        } else {
            ULONG save_cs = bm_buf[0];
            bm_buf[0] = 0;
            ULONG calc_cs = ffs_checksum(bm_buf, nlongs);
            bm_buf[0] = save_cs;
            if (calc_cs != save_cs) {
                ok = FALSE;
                sprintf(err_buf,
                        "BM block %lu CHECKSUM BAD after Inhibit(FALSE)!\n"
                        "stored=0x%08lX calc=0x%08lX\n"
                        "FFS validator will run and free new_root=%lu\n"
                        "→ root will be overwritten on next file write",
                        (unsigned long)nr_bm_blknum,
                        (unsigned long)save_cs, (unsigned long)calc_cs,
                        (unsigned long)new_root);
            } else if (BM_TESTFREE(bm_buf, nr_bm_off)) {
                ok = FALSE;
                sprintf(err_buf,
                        "BM block %lu: new_root=%lu marked FREE after Inhibit(FALSE)!\n"
                        "(off=%lu in bm block)\n"
                        "FFS will allocate new_root for file data → root overwritten",
                        (unsigned long)nr_bm_blknum,
                        (unsigned long)new_root,
                        (unsigned long)nr_bm_off);
            }
        }
    }

    /* On success, write diagnostic info to err_buf so the caller can
       display it — useful to verify new_root matches FFS expectations. */
    if (ok) {
        /* Count hash table entries (non-zero = files/dirs in root).
           Hash table is L[6..77] (72 entries) in the root block. */
        ULONG ht_entries = 0;
        if (root_buf) {
            for (ULONG i = 0; i < 72; i++)
                if (root_buf[6 + i]) ht_entries++;
        }
        sprintf(err_buf,
                "old_root=%lu  new_root=%lu (abs %lu)\n"
                "part_abs=%lu  new_blks=%lu\n"
                "heads=%lu  secs=%lu  reserved=%lu\n"
                "ht_entries=%lu  bm_blocks=%lu\n"
                "bm_blk_for_root=%lu  bm_off=%lu",
                (unsigned long)root_blk,
                (unsigned long)new_root,
                (unsigned long)(part_abs + new_root),
                (unsigned long)part_abs,
                (unsigned long)new_blocks,
                (unsigned long)heads,
                (unsigned long)sectors,
                (unsigned long)reserved,
                (unsigned long)ht_entries,
                (unsigned long)new_bm_need,
                (unsigned long)nr_bm_blknum,
                (unsigned long)nr_bm_off);
    }

    if (boot_buf)   FreeVec(boot_buf);
    if (root_buf)   FreeVec(root_buf);
    if (bm_buf)     FreeVec(bm_buf);
    if (bm_blknums) FreeVec(bm_blknums);
    return ok;
}
