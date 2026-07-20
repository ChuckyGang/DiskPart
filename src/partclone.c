/*
 * partclone.c - Partition dump-to-file, restore-from-file, clone.
 * See partclone.h for the design.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "clib.h"
#include "rdb.h"
#include "partmove.h"
#include "sfsresize.h"      /* SFS_IsSupportedType */
#include "pfsresize.h"      /* PFS_IsSupportedType */
#include "ffsresize.h"      /* FFS_IsSupportedType */
#include "sfs_util.h"
#include "partclone.h"
#include "locale_support.h"

#define PC_CHUNK_BLOCKS 256          /* device blocks per copy batch */

/* SFS root-block field offsets (same as partmove.c). */
#define SFS_ROOT_ID_V   0x53465300UL
#define SFS_O_ID        0
#define SFS_O_FIRSTBYTEH 32
#define SFS_O_FIRSTBYTE  36
#define SFS_O_LASTBYTEH  40
#define SFS_O_LASTBYTE   44
#define SFS_O_TOTALBLOCKS 48
#define SFS_O_BLOCKSIZE  52

/* ------------------------------------------------------------------ */
/* Big-endian header pack/unpack (fixed 512-byte block)               */
/* ------------------------------------------------------------------ */
static void pc_setl(UBYTE *b, ULONG o, ULONG v)
{
    b[o] = (UBYTE)(v >> 24); b[o+1] = (UBYTE)(v >> 16);
    b[o+2] = (UBYTE)(v >> 8); b[o+3] = (UBYTE)v;
}
static ULONG pc_getl(const UBYTE *b, ULONG o)
{
    return ((ULONG)b[o] << 24) | ((ULONG)b[o+1] << 16)
         | ((ULONG)b[o+2] << 8) | b[o+3];
}

/* Field layout inside the 512-byte header block (longword offsets * 4). */
enum {
    H_MAGIC = 0,  H_VERSION = 4,  H_DOSTYPE = 8,   H_BLKSIZE = 12,
    H_SPB = 16,   H_RESERVED = 20, H_HEADS = 24,   H_SECTORS = 28,
    H_LOWCYL = 32, H_HIGHCYL = 36, H_BLKCOUNT = 40, H_BOOTPRI = 44,
    H_FLAGS = 48, H_DEVFLAGS = 52, H_MASK = 56,    H_MAXXFER = 60,
    H_NUMBUF = 64, H_BUFMEM = 68, H_BOOTBLK = 72,  H_INTERLEAVE = 76,
    H_CONTROL = 80, H_BAUD = 84,
    H_NAME = 88,                  /* 32 bytes */
    H_CHECKSUM = 508              /* last longword */
};

static ULONG pc_header_sum(const UBYTE *b)
{
    ULONG s = 0, i;
    for (i = 0; i < 508; i += 4) s += pc_getl(b, i);
    return s;
}

static void pc_pack_header(UBYTE *b, const struct PartInfo *pi,
                           ULONG block_count)
{
    ULONG i;
    for (i = 0; i < 512; i++) b[i] = 0;
    pc_setl(b, H_MAGIC,     PARTDUMP_MAGIC);
    pc_setl(b, H_VERSION,   PARTDUMP_VERSION);
    pc_setl(b, H_DOSTYPE,   pi->dos_type);
    pc_setl(b, H_BLKSIZE,   pi->block_size);
    pc_setl(b, H_SPB,       pi->sectors_per_block);
    pc_setl(b, H_RESERVED,  pi->reserved_blks);
    pc_setl(b, H_HEADS,     pi->heads);
    pc_setl(b, H_SECTORS,   pi->sectors);
    pc_setl(b, H_LOWCYL,    pi->low_cyl);
    pc_setl(b, H_HIGHCYL,   pi->high_cyl);
    pc_setl(b, H_BLKCOUNT,  block_count);
    pc_setl(b, H_BOOTPRI,   (ULONG)pi->boot_pri);
    pc_setl(b, H_FLAGS,     pi->flags);
    pc_setl(b, H_DEVFLAGS,  pi->dev_flags);
    pc_setl(b, H_MASK,      pi->mask);
    pc_setl(b, H_MAXXFER,   pi->max_transfer);
    pc_setl(b, H_NUMBUF,    pi->num_buffer);
    pc_setl(b, H_BUFMEM,    pi->buf_mem_type);
    pc_setl(b, H_BOOTBLK,   pi->boot_blocks);
    pc_setl(b, H_INTERLEAVE, pi->interleave);
    pc_setl(b, H_CONTROL,   pi->control);
    pc_setl(b, H_BAUD,      pi->baud);
    for (i = 0; i < 31 && pi->drive_name[i]; i++) b[H_NAME + i] = pi->drive_name[i];
    pc_setl(b, H_CHECKSUM,  pc_header_sum(b));
}

static void pc_unpack_header(const UBYTE *b, struct PartDumpHeader *h)
{
    ULONG i;
    h->magic            = pc_getl(b, H_MAGIC);
    h->version          = pc_getl(b, H_VERSION);
    h->dos_type         = pc_getl(b, H_DOSTYPE);
    h->block_size       = pc_getl(b, H_BLKSIZE);
    h->sectors_per_block = pc_getl(b, H_SPB);
    h->reserved_blks    = pc_getl(b, H_RESERVED);
    h->heads            = pc_getl(b, H_HEADS);
    h->sectors          = pc_getl(b, H_SECTORS);
    h->src_low_cyl      = pc_getl(b, H_LOWCYL);
    h->src_high_cyl     = pc_getl(b, H_HIGHCYL);
    h->block_count      = pc_getl(b, H_BLKCOUNT);
    h->boot_pri         = pc_getl(b, H_BOOTPRI);
    h->flags            = pc_getl(b, H_FLAGS);
    h->dev_flags        = pc_getl(b, H_DEVFLAGS);
    h->mask             = pc_getl(b, H_MASK);
    h->max_transfer     = pc_getl(b, H_MAXXFER);
    h->num_buffer       = pc_getl(b, H_NUMBUF);
    h->buf_mem_type     = pc_getl(b, H_BUFMEM);
    h->boot_blocks      = pc_getl(b, H_BOOTBLK);
    h->interleave       = pc_getl(b, H_INTERLEAVE);
    h->control          = pc_getl(b, H_CONTROL);
    h->baud             = pc_getl(b, H_BAUD);
    for (i = 0; i < 31; i++) h->drive_name[i] = (char)b[H_NAME + i];
    h->drive_name[31] = '\0';
}

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                    */
/* ------------------------------------------------------------------ */
static ULONG pc_part_blocks(const struct PartInfo *pi, ULONG dh, ULONG ds)
{
    ULONG heads = pi->heads   > 0 ? pi->heads   : dh;
    ULONG secs  = pi->sectors > 0 ? pi->sectors : ds;
    ULONG spb   = (pi->block_size >= 1024) ? (pi->block_size / 512) : 1;
    return (pi->high_cyl - pi->low_cyl + 1) * heads * secs * spb;
}
static ULONG pc_part_base(const struct PartInfo *pi, ULONG dh, ULONG ds)
{
    ULONG heads = pi->heads   > 0 ? pi->heads   : dh;
    ULONG secs  = pi->sectors > 0 ? pi->sectors : ds;
    ULONG spb   = (pi->block_size >= 1024) ? (pi->block_size / 512) : 1;
    return pi->low_cyl * heads * secs * spb;
}

/* ------------------------------------------------------------------ */
/* Footprint validation (physical 512-byte block space)               */
/* ------------------------------------------------------------------ */
BOOL PartClone_ValidateFootprint(const struct RDBInfo *drdb,
                                 ULONG low, ULONG high, ULONG h, ULONG s,
                                 int skip_idx, char *err_buf, ULONG ebsz)
{
    UWORD i;
    (void)h; (void)s;

    /* Mirror RDB_Write's own accept/reject rules EXACTLY (cylinder space,
       against the RDB's usable lo_cyl/hi_cyl), so a placement this function
       approves can never be bounced by RDB_Write after the data is already
       copied - which is what happened when this checked physical blocks
       against total cylinders instead of the RDB's reserved hi_cyl. */
    if (high < low ||
        (drdb->lo_cyl > 0 && low  < drdb->lo_cyl) ||
        (drdb->hi_cyl > 0 && high > drdb->hi_cyl)) {
        snprintf(err_buf, ebsz, GS(MSG_PC_OVERLAP_FMT),
                 "disk end", (unsigned long)low, (unsigned long)high);
        return FALSE;
    }
    for (i = 0; i < drdb->num_parts; i++) {
        const struct PartInfo *p = &drdb->parts[i];
        if ((int)i == skip_idx) continue;
        if (low <= p->high_cyl && p->low_cyl <= high) {
            snprintf(err_buf, ebsz, GS(MSG_PC_OVERLAP_FMT),
                     p->drive_name, (unsigned long)p->low_cyl,
                     (unsigned long)p->high_cyl);
            return FALSE;
        }
    }
    return TRUE;
}

BOOL PartClone_FindGap(const struct RDBInfo *drdb, ULONG cyl_span,
                       ULONG h, ULONG s, ULONG *low_out)
{
    /* Scan candidate low cylinders from the RDB's low usable cyl upward,
       jumping past each occupied partition; O(parts^2) but parts<=64. */
    ULONG cand = drdb->lo_cyl;
    char  scratch[80];
    while (cand + cyl_span - 1 <= drdb->hi_cyl) {
        if (PartClone_ValidateFootprint(drdb, cand, cand + cyl_span - 1,
                                        h, s, -1, scratch, sizeof(scratch))) {
            *low_out = cand;
            return TRUE;
        }
        /* advance past the nearest partition that starts at/after cand */
        {
            ULONG next = drdb->hi_cyl + 1;
            UWORD i;
            for (i = 0; i < drdb->num_parts; i++) {
                ULONG phi = drdb->parts[i].high_cyl;
                if (phi >= cand && phi + 1 < next) next = phi + 1;
            }
            if (next <= cand) break;
            cand = next;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* SFS position fixup: patch both root blocks' firstbyte/lastbyte from  */
/* the source's absolute byte base to the destination's.  new_base/     */
/* old_base are ABSOLUTE 512-byte block numbers.  Non-fatal-safe: the   */
/* caller decides how to treat a FALSE return.                          */
/* ------------------------------------------------------------------ */
static BOOL pc_sfs_fixup(struct BlockDev *bd, ULONG new_base_blk,
                         ULONG old_base_blk)
{
    UBYTE scratch[512];
    ULONG bsz, sphys, total, r;
    ULONG roots[2];
    UQUAD delta_add, delta_sub;

    if (!BlockDev_ReadBlock(bd, new_base_blk, scratch)) return FALSE;
    if (sfs_getl(scratch, SFS_O_ID) != SFS_ROOT_ID_V) return FALSE;
    bsz = sfs_getl(scratch, SFS_O_BLOCKSIZE);
    if (bsz < 512 || (bsz & (bsz - 1)) || (bsz % 512)) return FALSE;
    sphys = bsz / 512;
    total = sfs_getl(scratch, SFS_O_TOTALBLOCKS);
    if (total < 2) return FALSE;

    /* byte delta between old and new absolute positions */
    if (new_base_blk >= old_base_blk) {
        delta_add = (UQUAD)(new_base_blk - old_base_blk) * 512;
        delta_sub = 0;
    } else {
        delta_add = 0;
        delta_sub = (UQUAD)(old_base_blk - new_base_blk) * 512;
    }

    roots[0] = 0;
    roots[1] = total - 1;
    for (r = 0; r < 2; r++) {
        UBYTE *buf = (UBYTE *)AllocVec(bsz, MEMF_PUBLIC);
        ULONG  i;
        UQUAD  fb, lb;
        BOOL   ok = TRUE;
        if (!buf) return FALSE;
        for (i = 0; i < sphys; i++)
            if (!BlockDev_ReadBlock(bd, new_base_blk + roots[r] * sphys + i,
                                    buf + i * 512)) { ok = FALSE; break; }
        if (ok && sfs_getl(buf, SFS_O_ID) == SFS_ROOT_ID_V) {
            fb = ((UQUAD)sfs_getl(buf, SFS_O_FIRSTBYTEH) << 32)
               |  (UQUAD)sfs_getl(buf, SFS_O_FIRSTBYTE);
            lb = ((UQUAD)sfs_getl(buf, SFS_O_LASTBYTEH) << 32)
               |  (UQUAD)sfs_getl(buf, SFS_O_LASTBYTE);
            fb = fb + delta_add - delta_sub;
            lb = lb + delta_add - delta_sub;
            sfs_setl(buf, SFS_O_FIRSTBYTEH, (ULONG)(fb >> 32));
            sfs_setl(buf, SFS_O_FIRSTBYTE,  (ULONG)(fb & 0xFFFFFFFFUL));
            sfs_setl(buf, SFS_O_LASTBYTEH,  (ULONG)(lb >> 32));
            sfs_setl(buf, SFS_O_LASTBYTE,   (ULONG)(lb & 0xFFFFFFFFUL));
            sfs_set_checksum(buf, bsz);
            for (i = 0; i < sphys; i++)
                if (!BlockDev_WriteBlock(bd, new_base_blk + roots[r] * sphys + i,
                                         buf + i * 512)) { ok = FALSE; break; }
        }
        FreeVec(buf);
        if (!ok) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Dump                                                                */
/* ------------------------------------------------------------------ */
BOOL PartClone_DumpToFile(struct BlockDev *bd, const struct PartInfo *pi,
                          const char *path,
                          MoveProgressFn progress_fn, void *progress_ud,
                          char *err_buf, ULONG ebsz)
{
#define PROG(d,t,ph) do { if (progress_fn) progress_fn(progress_ud,(d),(t),(ph)); } while(0)
    BPTR   fh;
    UBYTE *hdr = NULL, *buf = NULL;
    ULONG  base, count, done = 0;
    BOOL   ok = FALSE;

    base  = pc_part_base(pi, pi->heads, pi->sectors);
    count = pc_part_blocks(pi, pi->heads, pi->sectors);
    if (count == 0) { snprintf(err_buf, ebsz, GS(MSG_PC_NOT_FOUND_FMT), pi->drive_name); return FALSE; }

    hdr = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    buf = (UBYTE *)AllocVec((ULONG)PC_CHUNK_BLOCKS * 512, MEMF_PUBLIC);
    if (!hdr || !buf) { snprintf(err_buf, ebsz, GS(MSG_PC_OOM)); goto out; }

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) { snprintf(err_buf, ebsz, GS(MSG_PC_CANNOT_CREATE_FMT), path); goto out; }

    pc_pack_header(hdr, pi, count);
    if (Write(fh, hdr, 512) != 512) {
        snprintf(err_buf, ebsz, GS(MSG_PC_WRITE_ERR)); Close(fh); goto out;
    }

    PROG(0, count, GS(MSG_PC_COPYING));
    while (done < count) {
        ULONG batch = count - done;
        ULONG i;
        if (batch > PC_CHUNK_BLOCKS) batch = PC_CHUNK_BLOCKS;
        for (i = 0; i < batch; i++) {
            if (!BlockDev_ReadBlock(bd, base + done + i, buf + i * 512)) {
                /* zero-fill unreadable blocks, like the image copiers */
                memset(buf + i * 512, 0, 512);
            }
        }
        if (Write(fh, buf, (LONG)(batch * 512)) != (LONG)(batch * 512)) {
            snprintf(err_buf, ebsz, GS(MSG_PC_WRITE_ERR)); Close(fh); goto out;
        }
        done += batch;
        PROG(done, count, GS(MSG_PC_COPYING));
    }
    Close(fh);
    ok = TRUE;
out:
    if (hdr) FreeVec(hdr);
    if (buf) FreeVec(buf);
    return ok;
#undef PROG
}

/* ------------------------------------------------------------------ */
/* Read header only                                                    */
/* ------------------------------------------------------------------ */
BOOL PartClone_ReadHeader(const char *path, struct PartDumpHeader *hdr,
                          char *err_buf, ULONG ebsz)
{
    BPTR  fh;
    UBYTE *b = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    BOOL  ok = FALSE;
    if (!b) { snprintf(err_buf, ebsz, GS(MSG_PC_OOM)); return FALSE; }
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { snprintf(err_buf, ebsz, GS(MSG_PC_CANNOT_OPEN_FMT), path); FreeVec(b); return FALSE; }
    if (Read(fh, b, 512) != 512) { snprintf(err_buf, ebsz, GS(MSG_PC_SHORT_FILE)); goto out; }
    if (pc_getl(b, H_MAGIC) != PARTDUMP_MAGIC ||
        pc_getl(b, H_CHECKSUM) != pc_header_sum(b)) {
        snprintf(err_buf, ebsz, GS(MSG_PC_BAD_MAGIC)); goto out;
    }
    if (pc_getl(b, H_VERSION) != PARTDUMP_VERSION) {
        snprintf(err_buf, ebsz, GS(MSG_PC_BAD_VERSION_FMT),
                 (unsigned long)pc_getl(b, H_VERSION)); goto out;
    }
    pc_unpack_header(b, hdr);
    ok = TRUE;
out:
    Close(fh); FreeVec(b);
    return ok;
}

/* Copy the DosEnvec fields (everything but location/name) from a dump
   header into a PartInfo. */
static void pc_adopt_header(struct PartInfo *dst, const struct PartDumpHeader *h)
{
    dst->dos_type          = h->dos_type;
    dst->block_size        = h->block_size;
    dst->sectors_per_block = h->sectors_per_block;
    dst->reserved_blks     = h->reserved_blks;
    dst->heads             = h->heads;
    dst->sectors           = h->sectors;
    dst->flags             = h->flags;
    dst->dev_flags         = h->dev_flags;
    dst->mask              = h->mask;
    dst->max_transfer      = h->max_transfer;
    dst->num_buffer        = h->num_buffer;
    dst->buf_mem_type      = h->buf_mem_type;
    dst->boot_blocks       = h->boot_blocks;
    dst->interleave        = h->interleave;
    dst->control           = h->control;
    dst->baud              = h->baud;
    dst->boot_pri          = (LONG)h->boot_pri;
}

/* ------------------------------------------------------------------ */
/* Restore file -> existing partition                                  */
/* ------------------------------------------------------------------ */
BOOL PartClone_RestoreToPart(struct BlockDev *bd, struct RDBInfo *rdb,
                             struct PartInfo *dst, const char *path,
                             MoveProgressFn progress_fn, void *progress_ud,
                             char *err_buf, ULONG ebsz)
{
#define PROG(d,t,ph) do { if (progress_fn) progress_fn(progress_ud,(d),(t),(ph)); } while(0)
    struct PartDumpHeader h;
    BPTR   fh = 0;
    UBYTE *buf = NULL;
    ULONG  dst_blocks, dst_base, done = 0;
    ULONG  src_base_at_dump;
    BOOL   ok = FALSE;

    if (!PartClone_ReadHeader(path, &h, err_buf, ebsz)) return FALSE;

    /* Give the destination the dump's geometry AND cylinder span, keeping
       its start - so the restored filesystem's size matches what the dump
       was made from (FFS derives its root position from the partition size,
       so a size mismatch would break it).  Validate the new footprint fits
       without overlapping another partition, then adopt it. */
    {
        ULONG spb  = (h.block_size >= 1024) ? (h.block_size / 512) : 1;
        ULONG hh   = h.heads   > 0 ? h.heads   : rdb->heads;
        ULONG ss   = h.sectors > 0 ? h.sectors : rdb->sectors;
        ULONG span = (h.src_high_cyl >= h.src_low_cyl)
                     ? (h.src_high_cyl - h.src_low_cyl + 1) : 1;
        ULONG new_high = dst->low_cyl + span - 1;
        int   didx = (int)(dst - &rdb->parts[0]);
        if (!PartClone_ValidateFootprint(rdb, dst->low_cyl, new_high,
                                         hh, ss, didx, err_buf, ebsz))
            return FALSE;
        dst->heads    = hh;
        dst->sectors  = ss;
        dst->high_cyl = new_high;
        dst_blocks = span * hh * ss * spb;
        dst_base   = dst->low_cyl * hh * ss * spb;
        /* where the SFS roots in the dump think they live: source base */
        src_base_at_dump = h.src_low_cyl * hh * ss * spb;
    }
    if (dst_blocks < h.block_count) {
        snprintf(err_buf, ebsz, GS(MSG_PC_DST_TOO_SMALL_FMT),
                 dst->drive_name, (unsigned long)dst_blocks,
                 (unsigned long)h.block_count);
        return FALSE;
    }

    buf = (UBYTE *)AllocVec((ULONG)PC_CHUNK_BLOCKS * 512, MEMF_PUBLIC);
    if (!buf) { snprintf(err_buf, ebsz, GS(MSG_PC_OOM)); return FALSE; }
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { snprintf(err_buf, ebsz, GS(MSG_PC_CANNOT_OPEN_FMT), path); FreeVec(buf); return FALSE; }
    /* skip the 512-byte header */
    if (Read(fh, buf, 512) != 512) { snprintf(err_buf, ebsz, GS(MSG_PC_SHORT_FILE)); goto out; }

    PROG(0, h.block_count, GS(MSG_PC_COPYING));
    while (done < h.block_count) {
        ULONG batch = h.block_count - done;
        LONG  want, got;
        ULONG i;
        if (batch > PC_CHUNK_BLOCKS) batch = PC_CHUNK_BLOCKS;
        want = (LONG)(batch * 512);
        got = Read(fh, buf, want);
        if (got != want) { snprintf(err_buf, ebsz, GS(MSG_PC_SHORT_FILE)); goto out; }
        for (i = 0; i < batch; i++) {
            if (!BlockDev_WriteBlock(bd, dst_base + done + i, buf + i * 512)) {
                snprintf(err_buf, ebsz, GS(MSG_PC_READ_ERR_FMT),
                         (unsigned long)(dst_base + done + i)); goto out;
            }
        }
        done += batch;
        PROG(done, h.block_count, GS(MSG_PC_COPYING));
    }
    Close(fh); fh = 0;

    /* Adopt the dumped geometry, then fix SFS absolute offsets. */
    pc_adopt_header(dst, &h);
    if (SFS_IsSupportedType(h.dos_type)) {
        PROG(h.block_count, h.block_count, GS(MSG_PC_UPDATING_SFS));
        pc_sfs_fixup(bd, dst_base, src_base_at_dump);
    }
    ok = TRUE;
out:
    if (fh) Close(fh);
    if (buf) FreeVec(buf);
    return ok;
#undef PROG
}

/* PFS3 rootblock: options field at byte 4, MODE_SIZEFIELD = 0x10.  The
   rootblock sits at logical block reserved_blks from the partition start.
   PFS uses datestamp validation, not a block checksum, so clearing the bit
   and writing the sector back is safe (same as pfsresize.c).  Non-fatal. */
#define PC_PFS_OPTIONS_OFF  4
#define PC_PFS_MODE_SIZEFIELD 16
static void pc_pfs_clear_sizefield(struct BlockDev *bd, ULONG part_base_blk,
                                   ULONG reserved_blks, ULONG phys_per_lb)
{
    UBYTE sec[512];
    ULONG rb = part_base_blk + (reserved_blks ? reserved_blks : 2) * phys_per_lb;
    ULONG opt;
    if (!BlockDev_ReadBlock(bd, rb, sec)) return;
    /* only touch a genuine PFS rootblock (disktype 'PFS\1'/'PFS\2') */
    if (pc_getl(sec, 0) != 0x50465301UL && pc_getl(sec, 0) != 0x50465302UL)
        return;
    opt = pc_getl(sec, PC_PFS_OPTIONS_OFF);
    if (opt & PC_PFS_MODE_SIZEFIELD) {
        pc_setl(sec, PC_PFS_OPTIONS_OFF, opt & ~(ULONG)PC_PFS_MODE_SIZEFIELD);
        (void)BlockDev_WriteBlock(bd, rb, sec);
    }
}

/* ------------------------------------------------------------------ */
/* Filesystem-driver copy between RDBs                                  */
/* ------------------------------------------------------------------ */
int PartClone_DestNeedsFS(const struct RDBInfo *srdb,
                          const struct RDBInfo *drdb, ULONG dostype)
{
    UWORD i;
    /* ROM handles the DOS\0..DOS\7 family without an on-disk driver. */
    if ((dostype & 0xFFFFFF00UL) == 0x444F5300UL) return 0;
    for (i = 0; i < drdb->num_fs; i++)
        if (drdb->filesystems[i].dos_type == dostype) return 0;
    for (i = 0; i < srdb->num_fs; i++)
        if (srdb->filesystems[i].dos_type == dostype) return 1;
    return -1;
}

BOOL PartClone_CopyFS(const struct RDBInfo *srdb, struct RDBInfo *drdb,
                      ULONG dostype, char *err_buf, ULONG ebsz)
{
    const struct FSInfo *sfi = NULL;
    struct FSInfo *dfi;
    UWORD i;

    for (i = 0; i < srdb->num_fs; i++)
        if (srdb->filesystems[i].dos_type == dostype) { sfi = &srdb->filesystems[i]; break; }
    if (!sfi) { snprintf(err_buf, ebsz, GS(MSG_PC_FS_SRC_NONE)); return FALSE; }
    if (drdb->num_fs >= MAX_FILESYSTEMS) {
        snprintf(err_buf, ebsz, GS(MSG_PC_FS_NOROOM)); return FALSE;
    }
    dfi = &drdb->filesystems[drdb->num_fs];
    *dfi = *sfi;                       /* copy scalar fields + fs_name */
    dfi->code = NULL;
    if (sfi->code && sfi->code_size) {
        dfi->code = (UBYTE *)AllocVec(sfi->code_size, MEMF_PUBLIC);
        if (!dfi->code) { snprintf(err_buf, ebsz, GS(MSG_PC_OOM)); return FALSE; }
        { ULONG b; for (b = 0; b < sfi->code_size; b++) dfi->code[b] = sfi->code[b]; }
        dfi->code_size = sfi->code_size;
    }
    /* RDB_Write assigns the on-disk block numbers / LSEG chain. */
    dfi->block_num    = 0;
    dfi->next_fshd    = 0;
    dfi->seg_list_blk = RDB_END_MARK;
    drdb->num_fs++;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Relabel the just-cloned volume with a "CL-" prefix so it does not    */
/* clash with the source's volume name (AmigaDOS can't mount two        */
/* volumes with the same label at once).  Best-effort; the FS-specific  */
/* label lives in the root block.                                       */
/* ------------------------------------------------------------------ */

/* Build "CL-" + old (capped to maxlen) into out[]; returns new length. */
static ULONG pc_cl_name(UBYTE *out, const UBYTE *old, ULONG oldlen, ULONG maxlen)
{
    ULONG n = 0, i;
    const char *pfx = "CL-";
    for (i = 0; i < 3 && n < maxlen; i++) out[n++] = (UBYTE)pfx[i];
    for (i = 0; i < oldlen && n < maxlen; i++) out[n++] = old[i];
    return n;
}

/* FFS/OFS: BCPL volume name at longword (nlongs-20) in the root block;
   root block number is in the boot block's bb[2].  Recompute the root
   checksum (sum of all longwords == 0, checksum at longword 5). */
static void pc_relabel_ffs(struct BlockDev *bd, ULONG dst_base,
                           ULONG spb, ULONG nlongs)
{
    UBYTE boot[512];
    UBYTE *rb;
    ULONG root, eff = nlongs * 4, noff, i;
    ULONG sum = 0;
    UBYTE newname[32];
    ULONG nl;

    if (!BlockDev_ReadBlock(bd, dst_base, boot)) return;
    root = pc_getl(boot, 8);                 /* bb[2] */
    if (root == 0) return;

    rb = (UBYTE *)AllocVec(eff, MEMF_PUBLIC);
    if (!rb) return;
    for (i = 0; i < spb; i++)
        if (!BlockDev_ReadBlock(bd, dst_base + root * spb + i, rb + i * 512))
            { FreeVec(rb); return; }
    if (pc_getl(rb, 0) != 2 || pc_getl(rb, (nlongs - 1) * 4) != 1) {
        FreeVec(rb); return;                 /* not a T_SHORT/ST_ROOT block */
    }
    noff = (nlongs - 20) * 4;
    {
        UBYTE oldlen = rb[noff];
        if (oldlen > 30) oldlen = 30;
        nl = pc_cl_name(newname, rb + noff + 1, oldlen, 30);
    }
    rb[noff] = (UBYTE)nl;
    for (i = 0; i < nl; i++) rb[noff + 1 + i] = newname[i];
    pc_setl(rb, 20, 0);                      /* clear checksum (longword 5) */
    for (i = 0; i < nlongs; i++) sum += pc_getl(rb, i * 4);
    pc_setl(rb, 20, (ULONG)(-(LONG)sum));
    for (i = 0; i < spb; i++)
        (void)BlockDev_WriteBlock(bd, dst_base + root * spb + i, rb + i * 512);
    FreeVec(rb);
}

/* PFS3: BCPL diskname at byte 20 of the rootblock (no block checksum). */
static void pc_relabel_pfs(struct BlockDev *bd, ULONG part_base,
                           ULONG reserved, ULONG phys)
{
    UBYTE sec[512];
    ULONG rb = part_base + (reserved ? reserved : 2) * phys;
    UBYTE newname[32];
    ULONG nl, i;
    if (!BlockDev_ReadBlock(bd, rb, sec)) return;
    if (pc_getl(sec, 0) != 0x50465301UL && pc_getl(sec, 0) != 0x50465302UL)
        return;
    {
        UBYTE oldlen = sec[20];
        if (oldlen > 28) oldlen = 28;        /* 31 max, minus "CL-" */
        nl = pc_cl_name(newname, sec + 21, oldlen, 31);
    }
    sec[20] = (UBYTE)nl;
    for (i = 0; i < nl; i++) sec[21 + i] = newname[i];
    (void)BlockDev_WriteBlock(bd, rb, sec);
}

/* ------------------------------------------------------------------ */
/* Direct partition -> partition                                       */
/* ------------------------------------------------------------------ */
BOOL PartClone_PartToPart(struct BlockDev *sbd, const struct PartInfo *src,
                          struct BlockDev *dbd, struct RDBInfo *drdb,
                          struct PartInfo *dst,
                          MoveProgressFn progress_fn, void *progress_ud,
                          char *err_buf, ULONG ebsz)
{
#define PROG(d,t,ph) do { if (progress_fn) progress_fn(progress_ud,(d),(t),(ph)); } while(0)
    UBYTE *buf = NULL;
    ULONG  src_base, src_count, dst_base, dst_blocks, done = 0;
    ULONG  sh, ss, dh, ds;
    BOOL   ok = FALSE;

    /* Keep the DESTINATION partition exactly as it is - its own geometry,
       location and size - and drop the source's blocks into it.  The
       filesystem content is partition-relative and geometry-independent, so
       this works across disks of different geometry: only the FS's own size
       awareness matters (see the FFS check below).  Block sizes must match
       so the FS blocks line up. */
    if (src->block_size && dst->block_size && src->block_size != dst->block_size) {
        snprintf(err_buf, ebsz, GS(MSG_PC_BLKSIZE_MISMATCH),
                 (unsigned long)src->block_size, (unsigned long)dst->block_size,
                 (unsigned long)src->block_size);
        return FALSE;
    }

    sh = src->heads   > 0 ? src->heads   : drdb->heads;
    ss = src->sectors > 0 ? src->sectors : drdb->sectors;
    dh = dst->heads   > 0 ? dst->heads   : drdb->heads;
    ds = dst->sectors > 0 ? dst->sectors : drdb->sectors;

    src_base  = pc_part_base(src, sh, ss);
    src_count = pc_part_blocks(src, sh, ss);
    dst_base   = pc_part_base(dst, dh, ds);   /* dest's OWN physical footprint */
    dst_blocks = pc_part_blocks(dst, dh, ds);

    if (src_count == 0) { snprintf(err_buf, ebsz, GS(MSG_PC_NOT_FOUND_FMT), src->drive_name); return FALSE; }

    /* FFS/OFS bakes its total size into its layout (root position + bitmap
       come from the geometry).  Same size = drop-in copy.  LARGER = copy then
       grow the FFS to fill (the grow is fed the EXACT source block count via
       old_blocks_ovr, since it is not a whole number of dest cylinders).
       SMALLER can't be done by a plain copy. */
    if (dst_blocks < src_count) {
        snprintf(err_buf, ebsz, GS(MSG_PC_DST_TOO_SMALL_FMT),
                 dst->drive_name, (unsigned long)dst_blocks,
                 (unsigned long)src_count);
        return FALSE;
    }

    buf = (UBYTE *)AllocVec((ULONG)PC_CHUNK_BLOCKS * 512, MEMF_PUBLIC);
    if (!buf) { snprintf(err_buf, ebsz, GS(MSG_PC_OOM)); return FALSE; }

    PROG(0, src_count, GS(MSG_PC_COPYING));
    while (done < src_count) {
        ULONG batch = src_count - done;
        ULONG i;
        if (batch > PC_CHUNK_BLOCKS) batch = PC_CHUNK_BLOCKS;
        for (i = 0; i < batch; i++) {
            if (!BlockDev_ReadBlock(sbd, src_base + done + i, buf + i * 512))
                memset(buf + i * 512, 0, 512);
        }
        for (i = 0; i < batch; i++) {
            if (!BlockDev_WriteBlock(dbd, dst_base + done + i, buf + i * 512)) {
                snprintf(err_buf, ebsz, GS(MSG_PC_READ_ERR_FMT),
                         (unsigned long)(dst_base + done + i)); goto out;
            }
        }
        done += batch;
        PROG(done, src_count, GS(MSG_PC_COPYING));
    }

    /* Adopt only the FILESYSTEM-defining fields; keep the destination's own
       name, location, geometry, block size and mount flags. */
    dst->dos_type          = src->dos_type;
    dst->sectors_per_block = src->sectors_per_block;
    dst->reserved_blks     = src->reserved_blks;

    if (FFS_IsSupportedType(src->dos_type) && dst_blocks > src_count) {
        /* Larger destination: grow the just-copied FFS to fill it.  The FFS
           is coherent at its source size; the grow reads its root (from the
           boot-block pointer) and bitmap chain, relocates the root to the new
           centre, extends the bitmap and sets bm_flag=0 so FFS revalidates on
           mount.  old_blocks_ovr = the EXACT source FS block count. */
        ULONG spb = (dst->block_size >= 1024) ? (dst->block_size / 512) : 1;
        /* FFS_GrowPartition writes a ~210-char diagnostic into its err_buf on
           BOTH success and failure, so give it its own 256-byte buffer - a
           smaller one overflows and smashes the stack. */
        static char growbuf[256];
        growbuf[0] = '\0';
        PROG(src_count, src_count, GS(MSG_PC_UPDATING_SFS));
        if (!FFS_GrowPartition(dbd, drdb, dst, dst->low_cyl, src_count / spb,
                               growbuf, progress_fn, progress_ud)) {
            strncpy(err_buf, growbuf, ebsz - 1); err_buf[ebsz - 1] = '\0';
            goto out;   /* ok stays FALSE */
        }
    } else if (SFS_IsSupportedType(src->dos_type)) {
        /* SFS root blocks store ABSOLUTE byte offsets - shift them from the
           source's physical position to the destination's. */
        PROG(src_count, src_count, GS(MSG_PC_UPDATING_SFS));
        pc_sfs_fixup(dbd, dst_base, src_base);
        if (dst_blocks > src_count) {
            /* Larger destination: SFS validates its stored total, firstbyte
               and lastbyte against the DosEnvec at mount, so a source-sized
               SFS in a bigger partition mounts "Uninitialized".  Grow the
               just-relocated SFS to fill the partition - passing the EXACT
               destination SFS-block count so the total matches the DosEnvec
               (the cylinder-derived total can be off by a block or two). */
            ULONG spb  = (dst->block_size >= 1024) ? (dst->block_size / 512) : 1;
            ULONG dbpc = (dst->heads   > 0 ? dst->heads   : dh) *
                         (dst->sectors > 0 ? dst->sectors : ds) * spb;
            ULONG old_hi = dst->low_cyl + (dbpc ? (src_count / dbpc) : 0);
            static char growbuf[256];
            if (old_hi > dst->low_cyl) old_hi -= 1; else old_hi = dst->low_cyl;
            growbuf[0] = '\0';
            if (!SFS_GrowPartition(dbd, drdb, dst, old_hi, dst_blocks / spb,
                                   growbuf, progress_fn, progress_ud)) {
                strncpy(err_buf, growbuf, ebsz - 1); err_buf[ebsz - 1] = '\0';
                goto out;   /* ok stays FALSE */
            }
        }
    } else if (PFS_IsSupportedType(src->dos_type)) {
        /* PFS3 fails to mount if MODE_SIZEFIELD is set and its stored
           disksize != the partition's dg_TotalSectors.  In a larger
           destination they differ, so clear the flag (the FS then uses its
           own stored disksize - the extra space is free for a later GROW). */
        PROG(src_count, src_count, GS(MSG_PC_UPDATING_SFS));
        pc_pfs_clear_sizefield(dbd, dst_base, dst->reserved_blks,
                               (dst->block_size >= 1024) ? (dst->block_size / 512) : 1);
    }

    /* Rename the cloned volume "CL-<old>" so it does not clash with the
       source's label (best-effort; FFS/OFS and PFS3 supported). */
    {
        ULONG spb    = (dst->block_size >= 1024) ? (dst->block_size / 512) : 1;
        ULONG nlongs = ((dst->block_size >= 512) ? dst->block_size : 512) / 4;
        if (FFS_IsSupportedType(dst->dos_type))
            pc_relabel_ffs(dbd, dst_base, spb, nlongs);
        else if (PFS_IsSupportedType(dst->dos_type))
            pc_relabel_pfs(dbd, dst_base, dst->reserved_blks, spb);
    }
    ok = TRUE;
out:
    if (buf) FreeVec(buf);
    return ok;
#undef PROG
}
