/*
 * pfsresize.c - Experimental PFS3/PFS2 filesystem grow.
 *
 * STRATEGY (simple and reversible):
 *   PFS3 auto-creates bitmap blocks on demand (NewBitmapBlock called from
 *   AllocateBlocksAC when a seqnr has no on-disk block yet).  The only
 *   on-disk changes needed to grow the partition are:
 *
 *     1. Update disksize in the rootblock (if MODE_SIZEFIELD is set) and
 *        clear the MODE_SIZEFIELD flag itself.
 *        PFS3 checks at mount: if (MODE_SIZEFIELD && disksize != dg_TotalSectors)
 *        -> fail ("Uninitialized").  dg_TotalSectors comes from the in-memory
 *        DosEnvec de_HighCyl.  After a grow, de_HighCyl is still old until the
 *        user writes the RDB and reboots.  We clear MODE_SIZEFIELD so that PFS
 *        skips the disksize check entirely on the next reboot.  disksize is
 *        still updated to new_disksize so the value is correct going forward.
 *
 * NOTE ON Inhibit:
 *   We call Inhibit(device, DOSTRUE) before any writes to quiesce the PFS
 *   handler.  Without Inhibit, TD_WRITE64 to PFS-owned blocks hangs because
 *   the device driver queues behind PFS's own pending I/O.
 *
 *   Opening the grow confirmation dialog briefly causes Workbench to take a
 *   DosList write lock while scanning volumes.  PFS can't respond to
 *   ACTION_INHIBIT while waiting for the DosList read lock, so calling
 *   Inhibit immediately after the dialog closes can deadlock.
 *
 *   We avoid this by calling Delay(12) (~240ms on PAL) before Inhibit to let
 *   Workbench finish its DosList operation.  Once the write lock is released,
 *   PFS can receive and process ACTION_INHIBIT normally.
 *
 *     2. Update blocksfree += (new_disksize - old_disksize).
 *        This is the user-data free count.  PFS3 trusts it from the
 *        rootblock; if we leave it at the old value Workbench shows the
 *        wrong free space.
 *
 *   Both fields live in the rootblock cluster (the first rblkcluster
 *   sectors of the partition, usually 2 sectors = 1024 bytes).
 *
 * REVERSIBILITY:
 *   The original rootblock cluster is saved to a heap buffer before any
 *   write.  On any failure the saved original is written back sector by
 *   sector, restoring the disk to its pre-grow state.
 *
 * BITMAP BLOCKS:
 *   PFS3 creates new bitmap blocks in the reserved area automatically when
 *   it first tries to allocate a block beyond the old coverage.  We verify
 *   that reserved_free >= number of new bitmap/index blocks that PFS3 will
 *   need so that this auto-creation cannot fail silently.
 *
 * REFERENCES (pfs3aio source):
 *   blocks.h  - rootblock_t, bitmapblock_t, cindexblock_t field layout
 *   allocation.c - InitAllocation, AllocReservedBlock, NewBitmapBlock
 *   volume.c  - GetCurrentRoot (MODE_SIZEFIELD check)
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
#include "pfsresize.h"
#include "shrinkinfo.h"
#include "locale_support.h"

/* ------------------------------------------------------------------ */
/* PFS3 rootblock byte offsets (rootblock_t from blocks.h)            */
/* ------------------------------------------------------------------ */
#define PFS_RB_DISKTYPE         0   /* LONG  */
#define PFS_RB_OPTIONS          4   /* ULONG */
#define PFS_RB_DATESTAMP        8   /* ULONG */
/* bytes 12-51: creation date/time + protection + diskname[32]        */
#define PFS_RB_LASTRESERVED     52  /* ULONG */
#define PFS_RB_FIRSTRESERVED    56  /* ULONG */
#define PFS_RB_RESERVED_FREE    60  /* ULONG */
#define PFS_RB_RESERVED_BLKSIZE 64  /* UWORD (bytes 64-65) */
#define PFS_RB_RBLKCLUSTER      66  /* UWORD (bytes 66-67) */
#define PFS_RB_BLOCKSFREE       68  /* ULONG */
#define PFS_RB_ALWAYSFREE       72  /* ULONG */
/* bytes 76-83: roving_ptr, deldir                                    */
#define PFS_RB_DISKSIZE         84  /* ULONG */
/* bytes 88-95: extension, not_used                                   */
#define PFS_RB_BITMAPINDEX      96  /* ULONG[104] = idx.large.bitmapindex */

/* PFS3 mode flags (options field) */
#define PFS_MODE_SIZEFIELD      16
#define PFS_MODE_SUPERINDEX    128

/* PFS3 disktype IDs */
#define PFS_ID_PFS1  0x50465301UL   /* 'PFS\1' */
#define PFS_ID_PFS2  0x50465302UL   /* 'PFS\2' */

/* Maximum bitmap index entries in rootblock (MAXBITMAPINDEX+1 = 104) */
#define PFS_MAX_BITMAPINDEX    104

/* ------------------------------------------------------------------ */
/* Big-endian byte accessors (AmigaOS native, but explicit is safer)  */
/* ------------------------------------------------------------------ */
static ULONG pfs_getl(const UBYTE *b, ULONG o)
{
    return ((ULONG)b[o]<<24)|((ULONG)b[o+1]<<16)|((ULONG)b[o+2]<<8)|b[o+3];
}
static UWORD pfs_getw(const UBYTE *b, ULONG o)
{
    return (UWORD)(((UWORD)b[o]<<8)|b[o+1]);
}
static void pfs_setl(UBYTE *b, ULONG o, ULONG v)
{
    b[o]=(UBYTE)(v>>24); b[o+1]=(UBYTE)(v>>16);
    b[o+2]=(UBYTE)(v>>8); b[o+3]=(UBYTE)v;
}

/* ------------------------------------------------------------------ */

BOOL PFS_IsSupportedType(ULONG dostype)
{
    ULONG prefix = dostype >> 8;
    UBYTE ver    = (UBYTE)dostype;

    /* 'PFS\0'-'PFS\3' (0x50465300-0x50465303) */
    if (prefix == 0x504653UL && ver <= 3) return TRUE;
    /* 'PDS\0'-'PDS\3' (0x50445300-0x50445303) - most common for pfs3aio */
    if (prefix == 0x504453UL && ver <= 3) return TRUE;

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Read / write helper: transfer 'count' consecutive 512-byte sectors  */
/* starting at absolute block 'start'.  Returns FALSE on first error.  */
/* ------------------------------------------------------------------ */
static BOOL pfs_read_cluster(struct BlockDev *bd, ULONG start,
                              UBYTE *buf, UWORD count)
{
    UWORD i;
    for (i = 0; i < count; i++) {
        if (!BlockDev_ReadBlock(bd, start + i, buf + (ULONG)i * 512))
            return FALSE;
    }
    return TRUE;
}

/* Write the rootblock cluster using the already-open bd device handle.
   A fresh OpenDevice for writing reports a tiny virtual device size in
   UAE/Amiberry and every write returns "out of bounds".  Re-using the
   handle that BlockDev_Open already opened (the same one used for reads)
   avoids that problem. */
static BOOL pfs_write_cluster(struct BlockDev *bd, ULONG start,
                               const UBYTE *buf, UWORD count)
{
    UWORD i;
    for (i = 0; i < count; i++) {
        if (!BlockDev_WriteBlock(bd, start + i,
                                 (const void *)(buf + (ULONG)i * 512)))
            return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */

BOOL PFS_GrowPartition(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, ULONG old_high_cyl,
                       char *err_buf,
                       FFS_ProgressFn progress_fn, void *progress_ud)
{
#define PFS_PROGRESS(msg) do { if (progress_fn) progress_fn(progress_ud,(msg)); } while(0)

    UBYTE  first_sector[512];  /* scratch for initial read before alloc */
    UBYTE *cluster_buf  = NULL;
    UBYTE *original_buf = NULL;
    BOOL   ok            = FALSE;
    BOOL   did_inhibit   = FALSE;
    BOOL   write_ok      = FALSE;  /* TRUE only when all writes succeeded */
    char   inh_name[44];           /* "drivename:" */
    UWORD  cluster_phys  = 0;      /* physical 512-byte sector count for rootblock cluster */

    ULONG heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;

    if (heads == 0 || sectors == 0) {
        sprintf(err_buf, GS(MSG_PFS_INVALID_GEOMETRY),
                (unsigned long)heads, (unsigned long)sectors);
        return FALSE;
    }

    /* sectors (= de_BlocksPerTrack) is in LOGICAL blocks.
       BlockDev_ReadBlock addresses physical 512-byte sectors.
       When pi->block_size > 512 (e.g. 1024-byte logical blocks, de_SizeBlock=256),
       each logical block = block_size/512 physical sectors, so addresses must be
       scaled to physical sector addresses.
       disksize values are kept in logical blocks to match what PFS3 stores. */
    ULONG phys_per_lblock = (pi->block_size >= 1024) ? (pi->block_size / 512) : 1;

    /* PFS3 partition layout:
         Logical blocks 0 .. reserved_blks-1 : PFS bootblock
           (disktype='PFS\1', options=0 - NOT the rootblock)
         Logical block reserved_blks (typically 2) : PFS rootblock cluster
           (disktype='PFS\1', options != 0, rblkcluster != 0)
       The rootblock is at logical block de_ReservedBlks from the start of
       the partition.  pi->reserved_blks stores de_ReservedBlks (usually 2).
       disksize covers the WHOLE partition (including boot blocks) to match
       what PFS3 stores in the rootblock disksize field. */
    ULONG rb_lblock    = pi->reserved_blks > 0 ? pi->reserved_blks : 2;
    ULONG part_abs     = (pi->low_cyl * heads * sectors + rb_lblock) * phys_per_lblock;

    /* delta_blocks is computed in Phase 4 from PFS3's own disksize field.
       Do NOT compute it here from heads/sectors - the DosEnvec geometry may
       not match PFS3's format geometry (e.g. IDE 255x63 LBA translation vs
       the real CHS used at mkfs time), leading to a grossly wrong delta. */

    /* ---------------------------------------------------------------- */
    /* Phase 1 - read the PFS rootblock (before Inhibit; reads work    */
    /* without inhibit and hang after it due to PFS flush writes still  */
    /* draining in the device queue after Inhibit returns).             */
    /* ---------------------------------------------------------------- */
    PFS_PROGRESS(GS(MSG_PFS_READING_ROOTBLOCK));
    if (!BlockDev_ReadBlock(bd, part_abs, first_sector)) {
        sprintf(err_buf, GS(MSG_PFS_CANNOT_READ_ROOTBLOCK),
                (unsigned long)part_abs);
        goto done;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 2 - validate disktype, extract cluster geometry            */
    /* ---------------------------------------------------------------- */
    {
        ULONG disktype = pfs_getl(first_sector, PFS_RB_DISKTYPE);
        if (disktype != PFS_ID_PFS1 && disktype != PFS_ID_PFS2) {
            sprintf(err_buf,
                    GS(MSG_PFS_NOT_ROOTBLOCK),
                    (unsigned long)disktype,
                    (unsigned long)PFS_ID_PFS1,
                    (unsigned long)PFS_ID_PFS2);
            goto done;
        }
    }

    UWORD rblkcluster    = pfs_getw(first_sector, PFS_RB_RBLKCLUSTER);
    UWORD reserved_blksize = pfs_getw(first_sector, PFS_RB_RESERVED_BLKSIZE);

    if (rblkcluster == 0) {
        sprintf(err_buf,
                GS(MSG_PFS_RBLKCLUSTER_ZERO),
                (unsigned long)part_abs,
                (unsigned long)(pi->block_size),
                (unsigned long)phys_per_lblock,
                (unsigned long)pi->low_cyl,
                (unsigned long)heads,
                (unsigned long)sectors,
                first_sector[60], first_sector[61],
                first_sector[62], first_sector[63],
                first_sector[64], first_sector[65],
                first_sector[66], first_sector[67],
                (unsigned long)pfs_getl(first_sector, PFS_RB_DISKTYPE),
                (unsigned long)pfs_getl(first_sector, PFS_RB_OPTIONS));
        goto done;
    }
    if (reserved_blksize < 512 || (reserved_blksize & 3)) {
        sprintf(err_buf, GS(MSG_PFS_UNEXPECTED_BLKSIZE),
                (unsigned)reserved_blksize);
        goto done;
    }

    /* ---------------------------------------------------------------- */
    /* Phase 3 - read and save full rootblock cluster                   */
    /* ---------------------------------------------------------------- */
    PFS_PROGRESS(GS(MSG_PFS_READING_CLUSTER));
    {
        /* rblkcluster is in logical blocks; convert to physical 512-byte sectors */
        cluster_phys = (UWORD)((ULONG)rblkcluster * phys_per_lblock);
        ULONG cluster_bytes = (ULONG)cluster_phys * 512;
        cluster_buf  = (UBYTE *)AllocVec(cluster_bytes, MEMF_PUBLIC);
        original_buf = (UBYTE *)AllocVec(cluster_bytes, MEMF_PUBLIC);
        if (!cluster_buf || !original_buf) {
            sprintf(err_buf, GS(MSG_PFS_OUT_OF_MEMORY),
                    (unsigned long)cluster_bytes);
            goto done;
        }
        if (!pfs_read_cluster(bd, part_abs, cluster_buf, cluster_phys)) {
            sprintf(err_buf, GS(MSG_PFS_CANNOT_READ_CLUSTER),
                    (unsigned)cluster_phys, (unsigned long)part_abs);
            goto done;
        }
        /* save original for rollback */
        {
            ULONG i;
            for (i = 0; i < cluster_bytes; i++)
                original_buf[i] = cluster_buf[i];
        }
    }

    /* ---------------------------------------------------------------- */
    /* Phase 4 - read fields from the in-memory cluster buffer          */
    /* ---------------------------------------------------------------- */
    {
        /* All locals declared up-front for C89 compatibility */
        ULONG options, lastreserved, reserved_free, blocksfree, cur_disksize;
        ULONG old_ncyl, cyl_diff, bpc, delta_blocks, new_disksize;
        ULONG bitmapstart, longsperbmb, bm_coverage;
        ULONG old_user, new_user, old_num_bmb, new_num_bmb, num_new_bmb;
        ULONG idxperblk, old_num_idxb, new_num_idxb, num_new_idxb, reserved_needed;

        options       = pfs_getl(cluster_buf, PFS_RB_OPTIONS);
        lastreserved  = pfs_getl(cluster_buf, PFS_RB_LASTRESERVED);
        reserved_free = pfs_getl(cluster_buf, PFS_RB_RESERVED_FREE);
        blocksfree    = pfs_getl(cluster_buf, PFS_RB_BLOCKSFREE);
        cur_disksize  = pfs_getl(cluster_buf, PFS_RB_DISKSIZE);

        /* Derive delta from PFS3's own disksize, not the DosEnvec geometry.
           The DosEnvec may use LBA-translated geometry (e.g. 255x63) that
           differs from the real CHS PFS3 used at format time.  Dividing the
           on-disk disksize by the old cylinder count gives the exact
           blocks-per-cylinder in PFS3's native units - immune to geometry
           mismatch and to reserved_blksize differences (512 vs 1024). */
        old_ncyl     = (old_high_cyl >= pi->low_cyl)
                       ? (old_high_cyl - pi->low_cyl + 1) : 1;
        cyl_diff     = pi->high_cyl - old_high_cyl;
        bpc          = (old_ncyl > 0 && cur_disksize > 0)
                       ? (cur_disksize / old_ncyl)
                       : (heads * sectors);    /* fallback if disksize=0 */
        delta_blocks = cyl_diff * bpc;
        new_disksize = cur_disksize + delta_blocks;

        /* bitmapstart: user data (and bitmap coverage) starts here */
        bitmapstart = lastreserved + 1;

        /* blocks per bitmap block coverage: (reserved_blksize/4 - 3) * 32  */
        longsperbmb = (ULONG)(reserved_blksize / 4) - 3;
        bm_coverage = longsperbmb * 32;

        /* Number of bitmap blocks needed for old and new sizes */
        old_user    = (cur_disksize > bitmapstart) ? cur_disksize - bitmapstart : 0;
        new_user    = (new_disksize > bitmapstart) ? new_disksize - bitmapstart : 0;
        old_num_bmb = (old_user + bm_coverage - 1) / bm_coverage;
        new_num_bmb = (new_user + bm_coverage - 1) / bm_coverage;
        num_new_bmb = (new_num_bmb > old_num_bmb) ? new_num_bmb - old_num_bmb : 0;

        /* Index blocks (each covers longsperbmb bitmap blocks) */
        idxperblk   = longsperbmb;
        old_num_idxb = (old_num_bmb == 0) ? 0 :
                       (old_num_bmb + idxperblk - 1) / idxperblk;
        new_num_idxb = (new_num_bmb == 0) ? 0 :
                       (new_num_bmb + idxperblk - 1) / idxperblk;
        num_new_idxb = (new_num_idxb > old_num_idxb) ?
                       new_num_idxb - old_num_idxb : 0;
        reserved_needed = num_new_bmb + num_new_idxb;

        /* MODE_SUPERINDEX (0x80) is part of the standard format mask on
           essentially every pfs3 hard-disk partition: it selects the
           104-entry idx.large.bitmapindex[] array (g->supermode in pfs3aio
           -> GetBitmapIndex uses MAXBITMAPINDEX).  PFS3 auto-extends that
           array on demand via NewBitmapIndexBlock(), pulling index/bitmap
           blocks from the reserved area, exactly like the small-index case.
           So growing WITHIN the large array needs no special handling here.

           The genuinely unsupported layout is the 3-level extension
           superindex (rootblockextension.superindex[], MAXSUPER+1 entries),
           which pfs3 only engages once the index exceeds MAXBITMAPINDEX
           (~109 GB at a 1K reserved block size).  We refuse that case:
           if the partition ALREADY needs more index blocks than the large
           array holds, idx.large.bitmapindex[] is not the real top level
           and the minimal-edit grow strategy would be invalid.  The
           new_num_idxb > PFS_MAX_BITMAPINDEX check below additionally
           refuses a grow that would cross that boundary. */
        if (old_num_idxb > PFS_MAX_BITMAPINDEX) {
            sprintf(err_buf,
                    GS(MSG_PFS_SUPERINDEX),
                    (unsigned long)options);
            goto done;
        }

        if (reserved_needed > reserved_free) {
            /* Estimate the maximum safe high cylinder reachable with the
               free reserved blocks that are actually available.  Conservative
               (all reserved_free used for bitmaps, no index overhead), so the
               real limit may be a cylinder or two lower. */
            ULONG safe_new_bmb  = old_num_bmb + reserved_free;
            ULONG safe_disksize = bitmapstart + safe_new_bmb * bm_coverage;
            ULONG safe_cyls     = (bpc > 0) ? (safe_disksize / bpc) : 0;
            ULONG safe_high     = (safe_cyls > 0)
                                  ? (pi->low_cyl + safe_cyls - 1)
                                  : old_high_cyl;
            if (safe_high > pi->high_cyl) safe_high = pi->high_cyl;
            sprintf(err_buf,
                    GS(MSG_PFS_RESERVED_TOO_SMALL),
                    (unsigned long)reserved_needed,
                    (unsigned long)reserved_free,
                    (unsigned long)safe_high,
                    (unsigned long)(safe_high > old_high_cyl
                                    ? safe_high - old_high_cyl : 0),
                    (unsigned long)cyl_diff);
            goto done;
        }

        if (new_num_idxb > PFS_MAX_BITMAPINDEX) {
            /* PFS3 rootblock holds at most 104 bitmap index pointers.
               This is a hard limit of the PFS3 on-disk format. */
            ULONG blksz      = (pi->block_size >= 512) ? pi->block_size : 512;
            ULONG max_blocks = (ULONG)PFS_MAX_BITMAPINDEX * idxperblk
                               * bm_coverage;
            /* max_blocks in logical blocks -> MB: divide by blocks-per-MB */
            ULONG max_mb     = max_blocks / (1048576UL / blksz);
            sprintf(err_buf,
                    GS(MSG_PFS_TOO_LARGE),
                    (unsigned long)max_mb);
            goto done;
        }

        /* Sanity: blocksfree must be <= disksize.  A valid PFS3 filesystem
           can never have more free blocks than total blocks.  If this fires,
           the on-disk blocksfree was corrupted by a previous (buggy) grow
           attempt that computed an oversized delta.  The user must run
           PFSDoctor to rebuild blocksfree before growing again. */
        if (cur_disksize > 0 && blocksfree > cur_disksize) {
            sprintf(err_buf,
                    GS(MSG_PFS_METADATA_CORRUPT),
                    (unsigned long)blocksfree,
                    (unsigned long)cur_disksize,
                    pi->drive_name);
            goto done;
        }

        /* Overflow check: blocksfree + delta must not wrap a ULONG */
        if (delta_blocks > 0xFFFFFFFFUL - blocksfree) {
            sprintf(err_buf,
                    GS(MSG_PFS_OVERFLOW),
                    (unsigned long)blocksfree, (unsigned long)delta_blocks,
                    (unsigned long)heads, (unsigned long)sectors,
                    (unsigned long)(pi->high_cyl - old_high_cyl));
            goto done;
        }

        /* ---------------------------------------------------------------- */
        /* Phase 5 - update rootblock fields in the cluster buffer          */
        /* ---------------------------------------------------------------- */
        if (options & PFS_MODE_SIZEFIELD)
            pfs_setl(cluster_buf, PFS_RB_DISKSIZE, new_disksize);
        pfs_setl(cluster_buf, PFS_RB_BLOCKSFREE, blocksfree + delta_blocks);

        /* ---------------------------------------------------------------- */
        /* Phase 6 - write updated cluster BEFORE any Inhibit               */
        /*                                                                   */
        /* UAE/Amiberry sets the device write-extent to zero while          */
        /* Inhibit(DOSTRUE) is active (to prevent double-writes while PFS   */
        /* is flushing).  Every TD_WRITE64 issued after Inhibit(DOSTRUE)    */
        /* therefore fails with "UAEHF SCSI: out of bounds" because         */
        /* start+length > 0.  Writing BEFORE Inhibit avoids this.           */
        /* ---------------------------------------------------------------- */
        PFS_PROGRESS(GS(MSG_PFS_WRITING_CLUSTER));
        if (!pfs_write_cluster(bd, part_abs, cluster_buf, cluster_phys)) {
            sprintf(err_buf,
                    GS(MSG_PFS_CANNOT_WRITE),
                    pi->drive_name);
            goto done;
        }
        write_ok = TRUE;

        /* ---------------------------------------------------------------- */
        /* Phase 7 - clear MODE_SIZEFIELD (second write, still pre-Inhibit) */
        /* ---------------------------------------------------------------- */
        if (options & PFS_MODE_SIZEFIELD) {
            pfs_setl(cluster_buf, PFS_RB_OPTIONS,
                     options & ~(ULONG)PFS_MODE_SIZEFIELD);
            if (!pfs_write_cluster(bd, part_abs, cluster_buf, cluster_phys)) {
                PFS_PROGRESS(GS(MSG_PFS_SIZEFIELD_CLEAR_FAIL));
            }
        }

        /* ---------------------------------------------------------------- */
        /* Phase 8 - Inhibit(TRUE) + Inhibit(FALSE) to flush PFS cache      */
        /*           and force a re-read of our updated rootblock.           */
        /*                                                                   */
        /* PFS3's GetCurrentRoot() is called on Inhibit(DOSFALSE), which    */
        /* re-reads the rootblock from disk and picks up our new values.    */
        /* Without this, PFS keeps its stale in-memory rootblock and will   */
        /* eventually write it back to disk, losing our blocksfree change.  */
        /*                                                                   */
        /* Delay(50) first so Workbench's brief DosList write lock          */
        /* (taken when the confirmation dialog was dismissed) has time to   */
        /* release; otherwise ACTION_INHIBIT deadlocks.                     */
        /* ---------------------------------------------------------------- */
        PFS_PROGRESS(GS(MSG_PFS_FLUSHING_CACHE));
        Delay(50);
        if (pi->drive_name[0]) {
            DP_SNPRINTF(inh_name, "%s:", pi->drive_name);
            if (Inhibit((STRPTR)inh_name, DOSTRUE)) {
                did_inhibit = TRUE;
            }
        }

        /* ---------------------------------------------------------------- */
        /* Build success message (must fit in caller's 256-byte err_buf)   */
        /* Includes a raw hex dump of original rootblock bytes 52-91 so   */
        /* field offsets can be verified against the actual disk layout.   */
        /* Worst-case length: ~210 chars - fits in 256.                    */
        /* ---------------------------------------------------------------- */
        /* cyl_diff × bpc = delta_blocks; bpc derived from PFS3 disksize,
           not DosEnvec geometry, so it matches PFS3's native block units */
        sprintf(err_buf,
                GS(MSG_PFS_SUCCESS),
                (unsigned long)cyl_diff,
                (unsigned long)bpc,
                (unsigned long)delta_blocks,
                (unsigned long)blocksfree,
                (unsigned long)(blocksfree + delta_blocks),
                (unsigned long)cur_disksize,
                (unsigned long)new_disksize);

        ok = TRUE;
    }

done:
    /* Inhibit(DOSFALSE): tells PFS to resume and re-read the rootblock
       from disk (GetCurrentRoot), picking up our new blocksfree/disksize.
       Only called when writes succeeded (did_inhibit implies write_ok here). */
    if (did_inhibit)
        Inhibit((STRPTR)inh_name, DOSFALSE);
    if (cluster_buf)  FreeVec(cluster_buf);
    if (original_buf) FreeVec(original_buf);
    return ok;

#undef PFS_PROGRESS
}

/* ================================================================== */
/* PFS_ShrinkInfo - READ-ONLY minimum-shrinkable-size scan.            */
/*                                                                     */
/* Unlike the grow above (which never needs to look at bitmap blocks - */
/* PFS3 auto-creates them), this walks the real allocation bitmap:     */
/* rootblock bitmapindex[] (idx.small and idx.large both start at byte */
/* 96, capacities 5 vs 104) -> bitmap index blocks ('MI') ->           */
/* bitmap blocks ('BM'), verified against pfs3aio allocation.c:        */
/* bits are MSB-first (1<<(31-i)), 1 = free, coverage starts at        */
/* bitmapstart = lastreserved+1.  Block numbers are partition-relative */
/* logical blocks; a metadata block spans reserved_blksize bytes.      */
/* A bitmapindex/index entry of 0 means PFS3 has not created that      */
/* bitmap block yet -> the whole covered range is untouched (free).    */
/* All PFS metadata lives in the reserved area at the partition START, */
/* so the tail is pure data: floor = highest used block + 1, nothing   */
/* to relocate.                                                        */
/* ================================================================== */

#define PFS_BMBLKID  0x424D  /* 'BM' bitmapblock id (pfs3aio blocks.h)   */
#define PFS_BMIBLKID 0x4D49  /* 'MI' bitmap index block id               */

BOOL PFS_ShrinkInfo(struct BlockDev *bd, const struct RDBInfo *rdb,
                    const struct PartInfo *pi, struct ShrinkReport *rep,
                    char *err_buf)
{
    UBYTE  first_sector[512];
    UBYTE *idx_buf = NULL, *bm_buf = NULL;
    BOOL   ok = FALSE;

    ULONG heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    if (heads == 0 || sectors == 0) {
        sprintf(err_buf, GS(MSG_PFS_INVALID_GEOMETRY),
                (unsigned long)heads, (unsigned long)sectors);
        return FALSE;
    }

    ULONG phys_per_lblock = (pi->block_size >= 1024) ? (pi->block_size / 512) : 1;
    ULONG rb_lblock       = pi->reserved_blks > 0 ? pi->reserved_blks : 2;
    ULONG part_lbase      = pi->low_cyl * heads * sectors;   /* logical blocks */
    ULONG rb_abs          = (part_lbase + rb_lblock) * phys_per_lblock;

    if (!BlockDev_ReadBlock(bd, rb_abs, first_sector)) {
        sprintf(err_buf, GS(MSG_PFS_CANNOT_READ_ROOTBLOCK),
                (unsigned long)rb_abs);
        return FALSE;
    }
    {
        ULONG disktype = pfs_getl(first_sector, PFS_RB_DISKTYPE);
        if (disktype != PFS_ID_PFS1 && disktype != PFS_ID_PFS2) {
            sprintf(err_buf, GS(MSG_PFS_NOT_ROOTBLOCK),
                    (unsigned long)disktype,
                    (unsigned long)PFS_ID_PFS1,
                    (unsigned long)PFS_ID_PFS2);
            return FALSE;
        }
    }

    UWORD reserved_blksize = pfs_getw(first_sector, PFS_RB_RESERVED_BLKSIZE);
    ULONG options          = pfs_getl(first_sector, PFS_RB_OPTIONS);
    ULONG lastreserved     = pfs_getl(first_sector, PFS_RB_LASTRESERVED);
    ULONG disksize         = pfs_getl(first_sector, PFS_RB_DISKSIZE);

    if (reserved_blksize < 512 || (reserved_blksize % 512) != 0) {
        sprintf(err_buf, GS(MSG_PFS_UNEXPECTED_BLKSIZE),
                (unsigned)reserved_blksize);
        return FALSE;
    }
    if (disksize == 0) {
        sprintf(err_buf, GS(MSG_PFS_METADATA_CORRUPT),
                (unsigned long)0, (unsigned long)0, pi->drive_name);
        return FALSE;
    }

    ULONG bitmapstart = lastreserved + 1;
    ULONG longsperbmb = (ULONG)(reserved_blksize / 4) - 3;
    ULONG bm_coverage = longsperbmb * 32;
    ULONG user        = (disksize > bitmapstart) ? disksize - bitmapstart : 0;
    ULONG num_bmb     = (user + bm_coverage - 1) / bm_coverage;
    ULONG idxperblk   = longsperbmb;
    ULONG num_idxb    = (num_bmb == 0) ? 0
                        : (num_bmb + idxperblk - 1) / idxperblk;
    /* Small-index and supermode share the SAME layout at the same offset
       (pfs3aio blocks.h: bitmapindex is the first member of both idx
       union variants at byte 96) and the same two-level walk - only the
       array capacity differs: 5 entries (MAXSMALLBITMAPINDEX+1) without
       MODE_SUPERINDEX, 104 with it.  Real-world small PFS3 partitions DO
       lack MODE_SUPERINDEX (found on user's A3000 DH3 2026-07-20 - the
       "supermode is always set" assumption from the grow comments only
       holds for large partitions).  Beyond 104 the real top level is the
       3-level extension superindex - refuse, same as the grow. */
    {
        ULONG max_idx = (options & PFS_MODE_SUPERINDEX)
                        ? (ULONG)PFS_MAX_BITMAPINDEX : 5UL;
        if (num_idxb > max_idx) {
            sprintf(err_buf, GS(MSG_PFS_SUPERINDEX), (unsigned long)options);
            return FALSE;
        }
    }

    UWORD n_phys = (UWORD)(reserved_blksize / 512);
    idx_buf = (UBYTE *)AllocVec(reserved_blksize, MEMF_PUBLIC | MEMF_CLEAR);
    bm_buf  = (UBYTE *)AllocVec(reserved_blksize, MEMF_PUBLIC | MEMF_CLEAR);
    if (!idx_buf || !bm_buf) {
        sprintf(err_buf, GS(MSG_PFS_OUT_OF_MEMORY),
                (unsigned long)reserved_blksize);
        goto done;
    }

    ULONG used = 0, highest = 0;
    BOOL  any_used = FALSE;
    ULONG cur_idx = 0xFFFFFFFFUL;
    BOOL  idx_absent = FALSE;

    for (ULONG seq = 0; seq < num_bmb; seq++) {
        ULONG ii = seq / idxperblk;
        if (ii != cur_idx) {
            ULONG inr = pfs_getl(first_sector,
                                 PFS_RB_BITMAPINDEX + 4 * ii);
            cur_idx    = ii;
            idx_absent = (inr == 0);
            if (!idx_absent) {
                if (!pfs_read_cluster(bd, (part_lbase + inr) * phys_per_lblock,
                                      idx_buf, n_phys)) {
                    sprintf(err_buf, GS(MSG_SI_BM_READ_FMT),
                            (unsigned long)inr);
                    goto done;
                }
                if (pfs_getw(idx_buf, 0) != PFS_BMIBLKID) {
                    sprintf(err_buf, GS(MSG_SI_BM_BAD_FMT),
                            (unsigned long)inr);
                    goto done;
                }
            }
        }
        if (idx_absent) continue;              /* untouched region = free */

        ULONG bmnr = pfs_getl(idx_buf, 12 + 4 * (seq % idxperblk));
        if (bmnr == 0) continue;               /* untouched region = free */

        if (!pfs_read_cluster(bd, (part_lbase + bmnr) * phys_per_lblock,
                              bm_buf, n_phys)) {
            sprintf(err_buf, GS(MSG_SI_BM_READ_FMT), (unsigned long)bmnr);
            goto done;
        }
        if (pfs_getw(bm_buf, 0) != PFS_BMBLKID ||
            pfs_getl(bm_buf, 8) != seq) {
            sprintf(err_buf, GS(MSG_SI_BM_BAD_FMT), (unsigned long)bmnr);
            goto done;
        }

        for (ULONG m = 0; m < longsperbmb; m++) {
            ULONG b0 = bitmapstart + (seq * longsperbmb + m) * 32;
            if (b0 >= disksize) break;
            ULONG v = pfs_getl(bm_buf, 12 + 4 * m);
            ULONG in_range = disksize - b0;
            if (in_range > 32) in_range = 32;
            if (v == 0xFFFFFFFFUL) continue;               /* all free */
            if (v == 0 && in_range == 32) {                /* all used */
                used += 32; highest = b0 + 31; any_used = TRUE;
                continue;
            }
            for (ULONG k = 0; k < in_range; k++) {
                if (!(v & (1UL << (31u - k)))) {
                    used++; highest = b0 + k; any_used = TRUE;
                }
            }
        }
    }

    rep->total_blocks    = disksize;
    rep->used_blocks     = used + bitmapstart;  /* boot + reserved area */
    rep->highest_used    = any_used ? highest : 0;
    rep->min_blocks      = any_used ? highest + 1 : bitmapstart;
    if (rep->min_blocks < bitmapstart) rep->min_blocks = bitmapstart;
    rep->fs_block_bytes  = (pi->block_size > 0) ? pi->block_size : 512;
    rep->meta_note_block = 0;      /* PFS metadata all lives at the start */
    rep->deleted_blocks  = 0;
    rep->fresh           = FALSE;  /* read without Inhibit (see grow notes:
                                      reads after Inhibit can hang on PFS) */
    ok = TRUE;

done:
    if (idx_buf) FreeVec(idx_buf);
    if (bm_buf)  FreeVec(bm_buf);
    return ok;
}
