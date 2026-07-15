/*
 * progresswin.h - Small cancellable progress window shared by long-running
 * block-level operations (disk image dump/restore, whole-disk copy, ...).
 *
 * One status line gets repainted in place on every callback, plus a Cancel
 * button. No close-gadget-driven exit logic beyond setting `cancelled` -
 * the caller owns the lifecycle (open before the operation, close after)
 * and is expected to stop as soon as ProgressWin_Callback returns FALSE.
 */

#ifndef PROGRESSWIN_H
#define PROGRESSWIN_H

#include <exec/types.h>
#include <intuition/intuition.h>

struct ProgressWin {
    struct Window  *win;
    APTR            vi;
    struct Gadget  *glist;
    struct Gadget  *cancel_gad;
    UWORD           x;          /* text x */
    UWORD           y;          /* text baseline y */
    char            title[80];  /* Intuition keeps a pointer */
    ULONG           last_pct;   /* last percentage drawn (skip duplicates) */
    BOOL            cancelled;  /* set by cancel button / close gadget */
};

void ProgressWin_Open (struct ProgressWin *p, const char *title);
void ProgressWin_Close(struct ProgressWin *p);

/* ImageCopyCb-compatible callback (see imagecopy.h): redraws the status
 * line if the percentage advanced (or unconditionally when total is
 * unknown), draining pending input first so Cancel/close/ESC is responsive.
 * Returns FALSE once the user has cancelled, telling the caller to abort. */
BOOL ProgressWin_Callback(void *ud, ULONG cur, ULONG total);

#endif /* PROGRESSWIN_H */
