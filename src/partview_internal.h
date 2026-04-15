/*
 * partview_internal.h — Cross-file declarations for partview module.
 *
 * partview.c was split into:
 *   partview.c          — map drawing, list rendering, build_gadgets, event loop
 *   partview_dialogs.c  — partition dialog, advanced dialog, geometry, about,
 *                         parse helpers, FriendlyDosType helper
 *   partview_fs.c       — filesystem manager dialog
 *   partview_rdb.c      — RDB backup/restore/view and diagnostic tools
 *   partview_move.c     — partition move, filesystem grow operations
 *
 * Functions promoted from static are declared here and included by all
 * partview_*.c files.  Do NOT include this header outside the partview group.
 */

#ifndef PARTVIEW_INTERNAL_H
#define PARTVIEW_INTERNAL_H

#include <exec/types.h>
#include <intuition/intuition.h>
#include "rdb.h"
#include "devices.h"

/* ------------------------------------------------------------------ */
/* partview.c                                                           */
/* ------------------------------------------------------------------ */

/* Human-readable filesystem name for a DosType. buf >= 16 bytes. */
void FriendlyDosType(ULONG dostype, char *buf);

/* ------------------------------------------------------------------ */
/* partview_dialogs.c                                                   */
/* ------------------------------------------------------------------ */

ULONG parse_num (const char *s);
LONG  parse_long(const char *s);
ULONG parse_dostype(const char *s);

void  partition_advanced_dialog(struct PartInfo *pi);
BOOL  partition_dialog(struct PartInfo *pi, const char *title,
                       const struct RDBInfo *rdb);

void  show_about(struct Window *win);
BOOL  geometry_dialog(ULONG def_cyls, ULONG def_heads, ULONG def_secs,
                      ULONG *out_cyls, ULONG *out_heads, ULONG *out_secs);

/* ------------------------------------------------------------------ */
/* partview_fs.c                                                        */
/* ------------------------------------------------------------------ */

/* Returns TRUE if any changes were made to the RDB filesystem list. */
BOOL  filesystem_manager_dialog(struct RDBInfo *rdb);

/* ------------------------------------------------------------------ */
/* partview_rdb.c                                                       */
/* ------------------------------------------------------------------ */

void  rdb_backup_block   (struct Window *win, struct BlockDev *bd,
                          struct RDBInfo *rdb);
void  rdb_restore_block  (struct Window *win, struct BlockDev *bd);
void  rdb_backup_extended(struct Window *win, struct BlockDev *bd,
                          struct RDBInfo *rdb);
void  rdb_restore_extended(struct Window *win, struct BlockDev *bd);
void  rdb_view_block     (struct Window *win, struct BlockDev *bd,
                          struct RDBInfo *rdb);
void  rdb_raw_scan       (struct Window *win, struct BlockDev *bd);
void  raw_disk_read      (struct Window *win, struct BlockDev *bd);
void  raw_hex_dump       (struct Window *win, struct BlockDev *bd);

/* ------------------------------------------------------------------ */
/* partview_move.c                                                      */
/* ------------------------------------------------------------------ */

void  check_ffs_root(struct Window *win, struct BlockDev *bd,
                     const struct RDBInfo *rdb, WORD sel);
BOOL  offer_move_partition(struct Window *win, struct BlockDev *bd,
                           struct RDBInfo *rdb, struct PartInfo *pi,
                           ULONG default_lo);
void  offer_ffs_grow(struct Window *win, struct BlockDev *bd,
                     const struct RDBInfo *rdb, struct PartInfo *pi,
                     ULONG old_hi);
void  offer_pfs_grow(struct Window *win, struct BlockDev *bd,
                     const struct RDBInfo *rdb, struct PartInfo *pi,
                     ULONG old_hi);
void  offer_sfs_grow(struct Window *win, struct BlockDev *bd,
                     struct RDBInfo *rdb, struct PartInfo *pi,
                     ULONG old_hi);

#endif /* PARTVIEW_INTERNAL_H */
