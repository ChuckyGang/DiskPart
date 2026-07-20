/*
 * shrinkinfo.c - Minimum-shrinkable-size report (SHRINKINFO command).
 *
 * Shared dispatch + formatting for the read-only per-FS scanners in
 * ffsresize.c / pfsresize.c / sfsresize.c.  Output goes through a
 * caller-supplied line callback so the same code serves the CLI and
 * the script engine.  Nothing here writes to disk.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "clib.h"
#include "rdb.h"
#include "ffsresize.h"
#include "pfsresize.h"
#include "sfsresize.h"
#include "shrinkinfo.h"
#include "locale_support.h"

enum { SI_FS_FFS = 1, SI_FS_SFS, SI_FS_PFS };

static char s_line[200];

LONG ShrinkInfo_Run(struct BlockDev *bd, const struct RDBInfo *rdb,
                    const struct PartInfo *pi,
                    ShrinkInfoEmit emit, void *ud)
{
    struct ShrinkReport rep;
    char  err[256];
    char  sz1[20], sz2[20];
    const char *fsname;
    int   fskind;
    BOOL  ok;

    memset(&rep, 0, sizeof(rep));
    err[0] = '\0';

    if      (FFS_IsSupportedType(pi->dos_type)) { fskind = SI_FS_FFS; fsname = "FFS/OFS"; }
    else if (SFS_IsSupportedType(pi->dos_type)) { fskind = SI_FS_SFS; fsname = "SFS"; }
    else if (PFS_IsSupportedType(pi->dos_type)) { fskind = SI_FS_PFS; fsname = "PFS3"; }
    else {
        DP_SNPRINTF(s_line, GS(MSG_SI_UNSUPPORTED_FMT),
                    pi->drive_name, (unsigned long)pi->dos_type);
        emit(ud, s_line);
        return RETURN_WARN;
    }

    DP_SNPRINTF(s_line, GS(MSG_SI_SCANNING_FMT), pi->drive_name, fsname);
    emit(ud, s_line);

    switch (fskind) {
        case SI_FS_SFS: ok = SFS_ShrinkInfo(bd, rdb, pi, &rep, err); break;
        case SI_FS_PFS: ok = PFS_ShrinkInfo(bd, rdb, pi, &rep, err); break;
        default:        ok = FFS_ShrinkInfo(bd, rdb, pi, &rep, err); break;
    }
    if (!ok) {
        DP_SNPRINTF(s_line, GS(MSG_SI_FAIL_FMT), pi->drive_name, err);
        emit(ud, s_line);
        return RETURN_ERROR;
    }

    /* Blocks-per-cylinder from the FS's own total (same trick as the PFS
       grow: immune to DosEnvec-vs-format geometry mismatches). */
    ULONG ncyl = pi->high_cyl - pi->low_cyl + 1;
    ULONG bpc  = (ncyl > 0) ? rep.total_blocks / ncyl : 0;
    if (bpc == 0) {
        DP_SNPRINTF(s_line, GS(MSG_SI_FAIL_FMT), pi->drive_name,
                    "blocks/cylinder = 0");
        emit(ud, s_line);
        return RETURN_ERROR;
    }
    ULONG min_cyls = (rep.min_blocks + bpc - 1) / bpc;
    if (min_cyls == 0)   min_cyls = 1;
    if (min_cyls > ncyl) min_cyls = ncyl;
    ULONG min_high = pi->low_cyl + min_cyls - 1;
    ULONG reclaim  = ncyl - min_cyls;

    DP_SNPRINTF(s_line, GS(MSG_SI_HEADER_FMT), pi->drive_name, fsname);
    emit(ud, s_line);

    FormatSize((UQUAD)rep.total_blocks * rep.fs_block_bytes, sz1);
    DP_SNPRINTF(s_line, GS(MSG_SI_CURRENT_FMT),
                (unsigned long)ncyl,
                (unsigned long)pi->low_cyl, (unsigned long)pi->high_cyl,
                sz1);
    emit(ud, s_line);

    {
        ULONG pct = (rep.total_blocks > 0)
                    ? (ULONG)(((UQUAD)rep.used_blocks * 100) / rep.total_blocks)
                    : 0;
        DP_SNPRINTF(s_line, GS(MSG_SI_USED_FMT),
                    (unsigned long)rep.used_blocks,
                    (unsigned long)rep.total_blocks,
                    (unsigned long)pct);
        emit(ud, s_line);
    }

    DP_SNPRINTF(s_line, GS(MSG_SI_HIGHEST_FMT),
                (unsigned long)rep.highest_used,
                (unsigned long)rep.total_blocks);
    emit(ud, s_line);

    FormatSize((UQUAD)min_cyls * bpc * rep.fs_block_bytes, sz2);
    DP_SNPRINTF(s_line, GS(MSG_SI_MIN_FMT),
                (unsigned long)min_cyls, sz2, (unsigned long)min_high);
    emit(ud, s_line);

    if (reclaim == 0) {
        emit(ud, GS(MSG_SI_RECLAIM_NONE));
    } else {
        FormatSize((UQUAD)reclaim * bpc * rep.fs_block_bytes, sz1);
        DP_SNPRINTF(s_line, GS(MSG_SI_RECLAIM_FMT),
                    (unsigned long)reclaim, sz1);
        emit(ud, s_line);
    }

    if (rep.meta_note_block != 0) {
        DP_SNPRINTF(s_line, GS(MSG_SI_META_NOTE_FMT),
                    (unsigned long)rep.meta_note_block);
        emit(ud, s_line);
    }
    if (rep.deleted_blocks != 0) {
        DP_SNPRINTF(s_line, GS(MSG_SI_SFS_DELETED_FMT),
                    (unsigned long)rep.deleted_blocks);
        emit(ud, s_line);
    }
    if (!rep.fresh)
        emit(ud, GS(MSG_SI_FLUSH_CAVEAT));

    return RETURN_OK;
}
