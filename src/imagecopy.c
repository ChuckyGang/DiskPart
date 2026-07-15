/*
 * imagecopy.c - Whole-disk image dump and restore (see imagecopy.h).
 *
 * Strategy: per-block reads/writes via the existing BlockDev_ReadBlock /
 * BlockDev_WriteBlock API are batched against a 64 KB stage buffer so the
 * file side does one Read/Write per 128 blocks instead of per block.
 * The device side stays per-block (the BlockDev API doesn't expose
 * multi-block transfers), but on most drivers the per-block overhead
 * is small relative to the actual seek/transfer time, and 64 KB is a
 * conservative size that works on every driver tested.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "imagecopy.h"
#include "locale_support.h"

/* 128 * 512 = 64 KB. Big enough to amortise filesystem write cost,
 * small enough to keep AllocVec happy on tight memory. */
#define IMG_BATCH_BLOCKS  128

static void set_err(char *errbuf, ULONG ebsz, const char *msg)
{
    ULONG len = 0;
    if (!errbuf || ebsz == 0) return;
    while (msg[len] && len < ebsz - 1) { errbuf[len] = msg[len]; len++; }
    errbuf[len] = '\0';
}

BOOL ImageCopy_DiskToFile(struct BlockDev *bd, const char *path,
                          ULONG count, ImageCopyCb cb, void *ud,
                          char *errbuf, ULONG ebsz)
{
    BPTR   fh;
    UBYTE *buf;
    ULONG  total_blocks;
    ULONG  block;

    if (!bd || !path || !*path) {
        set_err(errbuf, ebsz, GS(MSG_IC_BAD_PARAMETERS));
        return FALSE;
    }

    total_blocks = (ULONG)(bd->total_bytes / bd->block_size);
    if (total_blocks == 0) {
        set_err(errbuf, ebsz, GS(MSG_IC_ZERO_BLOCKS));
        return FALSE;
    }

    if (count == 0)             count = total_blocks;
    if (count > total_blocks)   count = total_blocks;

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        set_err(errbuf, ebsz, GS(MSG_IC_CANNOT_CREATE_FILE));
        return FALSE;
    }

    buf = (UBYTE *)AllocVec(IMG_BATCH_BLOCKS * bd->block_size, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        set_err(errbuf, ebsz, GS(MSG_IC_OUT_OF_MEMORY));
        return FALSE;
    }

    for (block = 0; block < count; ) {
        ULONG batch = count - block;
        ULONG i;
        if (batch > IMG_BATCH_BLOCKS) batch = IMG_BATCH_BLOCKS;

        /* Per-block reads - zero unreadable blocks rather than abort,
         * matching rdb_backup_extended() behaviour. */
        for (i = 0; i < batch; i++) {
            UBYTE *dst = buf + i * bd->block_size;
            if (!BlockDev_ReadBlock(bd, block + i, dst))
                memset(dst, 0, bd->block_size);
        }

        if (Write(fh, buf, (LONG)(batch * bd->block_size)) !=
            (LONG)(batch * bd->block_size)) {
            FreeVec(buf);
            Close(fh);
            set_err(errbuf, ebsz, GS(MSG_IC_WRITE_ERROR));
            return FALSE;
        }

        block += batch;
        if (cb && !cb(ud, block, count)) {
            FreeVec(buf);
            Close(fh);
            set_err(errbuf, ebsz, GS(MSG_IC_CANCELLED));
            return FALSE;
        }
    }

    FreeVec(buf);
    Close(fh);
    return TRUE;
}

BOOL ImageCopy_FileToDisk(struct BlockDev *bd, const char *path,
                          ImageCopyCb cb, void *ud,
                          char *errbuf, ULONG ebsz)
{
    BPTR   fh;
    UBYTE *buf;
    ULONG  total_blocks;
    ULONG  block;
    ULONG  hint_total;          /* progress hint, 0 = unknown */

    if (!bd || !path || !*path) {
        set_err(errbuf, ebsz, GS(MSG_IC_BAD_PARAMETERS));
        return FALSE;
    }

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        set_err(errbuf, ebsz, GS(MSG_IC_CANNOT_OPEN_FILE));
        return FALSE;
    }

    /* Try ExamineFH for a progress hint. fib_Size is LONG so files >2 GB
     * may report a negative or wrapped value - treat anything not strictly
     * positive and block-aligned as "unknown", and fall back to streaming
     * progress without a percentage. */
    hint_total = 0;
    {
        struct FileInfoBlock *fib =
            (struct FileInfoBlock *)AllocVec(sizeof(*fib),
                                              MEMF_PUBLIC | MEMF_CLEAR);
        if (fib) {
            if (ExamineFH(fh, fib) && fib->fib_Size > 0 &&
                ((ULONG)fib->fib_Size % bd->block_size) == 0)
                hint_total = (ULONG)fib->fib_Size / bd->block_size;
            FreeVec(fib);
        }
    }

    total_blocks = (ULONG)(bd->total_bytes / bd->block_size);

    buf = (UBYTE *)AllocVec(IMG_BATCH_BLOCKS * bd->block_size, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        set_err(errbuf, ebsz, GS(MSG_IC_OUT_OF_MEMORY));
        return FALSE;
    }

    /* Stream sequentially until Read returns 0 (EOF). No Seek calls,
     * so the input file is not constrained by the AmigaOS Seek 2 GB limit. */
    block = 0;
    for (;;) {
        ULONG i;
        ULONG got_blocks;
        LONG  got = Read(fh, buf, (LONG)(IMG_BATCH_BLOCKS * bd->block_size));
        if (got < 0) {
            FreeVec(buf); Close(fh);
            set_err(errbuf, ebsz, GS(MSG_IC_READ_ERROR));
            return FALSE;
        }
        if (got == 0) break;                /* EOF */
        if ((ULONG)got % bd->block_size != 0) {
            FreeVec(buf); Close(fh);
            set_err(errbuf, ebsz, GS(MSG_IC_NOT_BLOCK_ALIGNED));
            return FALSE;
        }
        got_blocks = (ULONG)got / bd->block_size;
        if (block + got_blocks > total_blocks) {
            FreeVec(buf); Close(fh);
            set_err(errbuf, ebsz, GS(MSG_IC_IMAGE_TOO_LARGE));
            return FALSE;
        }
        for (i = 0; i < got_blocks; i++) {
            if (!BlockDev_WriteBlock(bd, block + i,
                                     buf + i * bd->block_size)) {
                FreeVec(buf); Close(fh);
                set_err(errbuf, ebsz, GS(MSG_IC_WRITE_ERROR));
                return FALSE;
            }
        }
        block += got_blocks;
        if (cb && !cb(ud, block, hint_total)) {
            FreeVec(buf);
            Close(fh);
            set_err(errbuf, ebsz, GS(MSG_IC_CANCELLED));
            return FALSE;
        }
    }

    if (block == 0) {
        FreeVec(buf); Close(fh);
        set_err(errbuf, ebsz, GS(MSG_IC_IMAGE_EMPTY));
        return FALSE;
    }

    FreeVec(buf);
    Close(fh);
    return TRUE;
}

BOOL ImageCopy_DiskToDisk(struct BlockDev *src, struct BlockDev *dst,
                          ImageCopyCb cb, void *ud,
                          char *errbuf, ULONG ebsz)
{
    UBYTE *buf;
    ULONG  total_blocks;
    ULONG  dst_blocks;
    ULONG  block;

    if (!src || !dst) {
        set_err(errbuf, ebsz, GS(MSG_IC_BAD_PARAMETERS));
        return FALSE;
    }

    if (src->block_size != dst->block_size) {
        set_err(errbuf, ebsz, GS(MSG_IC_BLOCK_SIZE_MISMATCH));
        return FALSE;
    }

    total_blocks = (ULONG)(src->total_bytes / src->block_size);
    if (total_blocks == 0) {
        set_err(errbuf, ebsz, GS(MSG_IC_ZERO_BLOCKS));
        return FALSE;
    }

    dst_blocks = (ULONG)(dst->total_bytes / dst->block_size);
    if (dst_blocks < total_blocks) {
        set_err(errbuf, ebsz, GS(MSG_IC_DEST_TOO_SMALL));
        return FALSE;
    }

    buf = (UBYTE *)AllocVec(IMG_BATCH_BLOCKS * src->block_size, MEMF_PUBLIC);
    if (!buf) {
        set_err(errbuf, ebsz, GS(MSG_IC_OUT_OF_MEMORY));
        return FALSE;
    }

    for (block = 0; block < total_blocks; ) {
        ULONG batch = total_blocks - block;
        ULONG i;
        if (batch > IMG_BATCH_BLOCKS) batch = IMG_BATCH_BLOCKS;

        /* Per-block reads - zero unreadable blocks rather than abort,
         * matching ImageCopy_DiskToFile() behaviour. */
        for (i = 0; i < batch; i++) {
            UBYTE *p = buf + i * src->block_size;
            if (!BlockDev_ReadBlock(src, block + i, p))
                memset(p, 0, src->block_size);
        }

        for (i = 0; i < batch; i++) {
            if (!BlockDev_WriteBlock(dst, block + i,
                                     buf + i * src->block_size)) {
                FreeVec(buf);
                set_err(errbuf, ebsz, GS(MSG_IC_WRITE_ERROR));
                return FALSE;
            }
        }

        block += batch;
        if (cb && !cb(ud, block, total_blocks)) {
            FreeVec(buf);
            set_err(errbuf, ebsz, GS(MSG_IC_CANCELLED));
            return FALSE;
        }
    }

    FreeVec(buf);
    return TRUE;
}
