/*
 * ffsresize.h — Experimental FFS/OFS filesystem grow after a partition
 *               cylinder range extension.
 *
 * EXPERIMENTAL: writes filesystem metadata blocks directly to disk.
 *               Keep this file separate so it can be removed cleanly.
 */

#ifndef FFSRESIZE_H
#define FFSRESIZE_H

#include <exec/types.h>
#include "rdb.h"

/*
 * Returns TRUE if dostype is an AmigaDOS OFS/FFS variant we can handle
 * (DOS\0 through DOS\7).
 */
BOOL FFS_IsSupportedType(ULONG dostype);

/*
 * Optional progress callback.  Called at the start of each phase with a
 * short human-readable description of what is about to happen.
 * ud  — opaque user-data pointer passed unchanged from FFS_GrowPartition.
 * msg — NUL-terminated ASCII string, valid only for the duration of the call.
 * The callback must return quickly; it must not block or do disk I/O.
 */
typedef void (*FFS_ProgressFn)(void *ud, const char *msg);

/*
 * Grow the filesystem on partition *pi to cover the extended cylinder range.
 * pi->high_cyl must already be set to the NEW (larger) value.
 * old_high_cyl is the value it had before the edit.
 *
 * Writes bitmap blocks directly to disk — the RDB write (high_cyl update)
 * should happen AFTER this call succeeds.
 *
 * err_buf      : caller-supplied buffer for error text, at least 128 bytes.
 * progress_fn  : optional progress callback (may be NULL).
 * progress_ud  : opaque value passed to progress_fn.
 * Returns TRUE on success.
 */
BOOL FFS_GrowPartition(struct BlockDev *bd, const struct RDBInfo *rdb,
                       const struct PartInfo *pi, ULONG old_high_cyl,
                       char *err_buf,
                       FFS_ProgressFn progress_fn, void *progress_ud);

#endif /* FFSRESIZE_H */
