/*
 * shrinkinfo.h - Minimum-shrinkable-size report (SHRINKINFO command).
 *
 * READ-ONLY: walks each filesystem's own allocation bitmap to find the
 * highest in-use block, i.e. how far the partition could shrink.  This is
 * the read-only half of a future SHRINK - the same scan becomes its
 * mandatory tail-free safety check.  Nothing here writes to disk.
 */

#ifndef SHRINKINFO_H
#define SHRINKINFO_H

#include <exec/types.h>
#include "rdb.h"

/*
 * Filled by the per-FS scanners below.  All block numbers/counts are
 * partition-relative, in the filesystem's own block unit of
 * fs_block_bytes bytes (FFS: eff. FS block, PFS3: DosEnvec logical
 * block, SFS: SFS block).
 */
struct ShrinkReport {
    ULONG total_blocks;    /* the FS's own idea of the partition size    */
    ULONG used_blocks;     /* allocated blocks incl. reserved/boot area  */
    ULONG highest_used;    /* highest allocated block number             */
    ULONG min_blocks;      /* shrink floor (movable metadata excluded)   */
    ULONG fs_block_bytes;  /* bytes per block for the numbers above      */
    ULONG meta_note_block; /* !=0: movable FS metadata found above the
                              floor that a real SHRINK would relocate    */
    ULONG deleted_blocks;  /* SFS only: blocks held by recycle-bin files */
    UBYTE fresh;           /* TRUE: scanner Inhibit()ed the volume, the
                              numbers are post-flush; FALSE: last-flush
                              caveat applies when the volume is mounted  */
};

/*
 * Per-FS scanners (implemented next to their grow counterparts in
 * ffsresize.c / pfsresize.c / sfsresize.c so they share the on-disk
 * layout knowledge).  err_buf must hold at least 256 bytes.
 * Return TRUE on success with *rep filled.
 */
BOOL FFS_ShrinkInfo(struct BlockDev *bd, const struct RDBInfo *rdb,
                    const struct PartInfo *pi, struct ShrinkReport *rep,
                    char *err_buf);
BOOL PFS_ShrinkInfo(struct BlockDev *bd, const struct RDBInfo *rdb,
                    const struct PartInfo *pi, struct ShrinkReport *rep,
                    char *err_buf);
BOOL SFS_ShrinkInfo(struct BlockDev *bd, const struct RDBInfo *rdb,
                    const struct PartInfo *pi, struct ShrinkReport *rep,
                    char *err_buf);

/* Line-output callback: receives fully formatted lines (with \n). */
typedef void (*ShrinkInfoEmit)(void *ud, const char *line);

/*
 * Dispatch on pi->dos_type, run the scanner, and emit the formatted
 * report.  Returns RETURN_OK, RETURN_WARN (unsupported filesystem) or
 * RETURN_ERROR (scan failed; reason already emitted).
 */
LONG ShrinkInfo_Run(struct BlockDev *bd, const struct RDBInfo *rdb,
                    const struct PartInfo *pi,
                    ShrinkInfoEmit emit, void *ud);

#endif /* SHRINKINFO_H */
