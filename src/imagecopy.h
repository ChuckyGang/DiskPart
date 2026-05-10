/*
 * imagecopy.h — Whole-disk image dump and restore.
 *
 * Wraps BlockDev_ReadBlock / BlockDev_WriteBlock with batched dos.library
 * file I/O so a disk can be copied to a regular file (image dump) and a
 * file can be written back to a disk (image restore).
 *
 * Both directions operate purely sequentially — no Seek is performed on
 * the file once it is opened — so the AmigaOS Seek() 2 GB ceiling does
 * not apply to the copy itself. The destination filesystem still has to
 * be able to hold the resulting file (FFS pre-OS3.5 caps at 2 GB; SFS,
 * PFS3, FFS-NSD support much larger files).
 */

#ifndef IMAGECOPY_H
#define IMAGECOPY_H

#include <exec/types.h>
#include "rdb.h"

/* Informational threshold: above this size (2 GB) callers may wish to
 * warn the user that the destination filesystem must support large
 * files. Not enforced inside imagecopy. */
#define IMAGE_LARGE_THRESHOLD   ((UQUAD)0x80000000UL)

/* Progress / cancel callback. Invoked once per stage-buffer batch with
 * the cumulative block count copied so far and a hint of the expected
 * total (`total` may be 0 if not known up front, e.g. an input image
 * file larger than ExamineFH could report). Callback may be NULL.
 *
 * Return TRUE to continue the copy, FALSE to abort it cleanly — the copy
 * function will then close the file, free the buffer, set errbuf to
 * "Cancelled by user." and return FALSE. The callback runs synchronously
 * in the calling task. */
typedef BOOL (*ImageCopyCb)(void *ud, ULONG cur, ULONG total);

/* Copy `count` blocks from bd starting at block 0 into a fresh image file
 * at `path`. count==0 means "all of bd". The output file is written
 * sequentially (no Seek), so size is bounded only by the destination
 * filesystem's max-file-size. Returns TRUE on success. On failure, errbuf
 * (if non-NULL) gets a short message and a partial file may exist. */
BOOL ImageCopy_DiskToFile(struct BlockDev *bd, const char *path,
                          ULONG count, ImageCopyCb cb, void *ud,
                          char *errbuf, ULONG ebsz);

/* Copy an image file at `path` onto bd starting at block 0.
 * The file is read sequentially until EOF (no Seek), so input size is
 * not limited by Seek's 2 GB ceiling. Each Read result must be a multiple
 * of bd->block_size, and the cumulative block count must fit within bd
 * capacity. Returns TRUE on success. */
BOOL ImageCopy_FileToDisk(struct BlockDev *bd, const char *path,
                          ImageCopyCb cb, void *ud,
                          char *errbuf, ULONG ebsz);

#endif /* IMAGECOPY_H */
