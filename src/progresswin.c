/*
 * progresswin.c - Small cancellable progress window. See progresswin.h.
 *
 * Extracted from partview_image.c so other block-level copy operations
 * (whole-disk-to-disk copy) can reuse the same dialog.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/gadtools.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "locale_support.h"
#include "progresswin.h"

#define PROG_GID_CANCEL  100

void ProgressWin_Open(struct ProgressWin *p, const char *title)
{
    struct Screen *scr;
    UWORD fh, bor_l, bor_t, bor_r, bor_b, pad, btn_h, btn_w;
    UWORD win_w, win_h, status_y, btn_y;

    memset(p, 0, sizeof(*p));
    strncpy(p->title, title, sizeof(p->title) - 1);
    p->last_pct = 0xFFFFFFFFUL;

    scr = LockPubScreen(NULL);
    if (!scr) return;

    p->vi = GetVisualInfoA(scr, NULL);
    if (!p->vi) { UnlockPubScreen(NULL, scr); return; }

    fh    = scr->Font->ta_YSize;
    bor_l = (UWORD)scr->WBorLeft;
    bor_t = (UWORD)scr->WBorTop + fh + 1;
    bor_r = (UWORD)scr->WBorRight;
    bor_b = (UWORD)scr->WBorBottom;
    pad   = 8;
    btn_h = fh + 6;
    btn_w = 80;
    win_w = 380;
    status_y = bor_t + pad;                 /* status text top */
    btn_y    = status_y + (fh + 4) + pad;   /* cancel button top */
    win_h    = btn_y + btn_h + pad + bor_b;

    {
        struct Gadget *gctx;
        if (CreateContext(&p->glist) == NULL) {
            FreeVisualInfo(p->vi); p->vi = NULL;
            UnlockPubScreen(NULL, scr);
            return;
        }
        gctx = p->glist;
        {
            struct NewGadget ng;
            struct TagItem   bt[] = { { TAG_DONE, 0 } };
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = p->vi;
            ng.ng_TextAttr   = scr->Font;
            ng.ng_LeftEdge   = (win_w - btn_w) / 2;
            ng.ng_TopEdge    = btn_y;
            ng.ng_Width      = btn_w;
            ng.ng_Height     = btn_h;
            ng.ng_GadgetText = GS(MSG_CANCEL);
            ng.ng_GadgetID   = PROG_GID_CANCEL;
            p->cancel_gad = CreateGadgetA(BUTTON_KIND, gctx, &ng, bt);
            if (!p->cancel_gad) {
                FreeGadgets(p->glist); p->glist = NULL;
                FreeVisualInfo(p->vi); p->vi = NULL;
                UnlockPubScreen(NULL, scr);
                return;
            }
        }
    }

    {
        struct TagItem win_tags[] = {
            { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
            { WA_Width,     win_w  },
            { WA_Height,    win_h  },
            { WA_Title,     (ULONG)p->title },
            { WA_Gadgets,   (ULONG)p->glist },
            { WA_PubScreen, (ULONG)scr },
            { WA_IDCMP,     IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                            IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW },
            { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                            WFLG_CLOSEGADGET | WFLG_SMART_REFRESH |
                            WFLG_ACTIVATE },
            { TAG_DONE,     0 }
        };
        p->win = OpenWindowTagList(NULL, win_tags);
    }

    UnlockPubScreen(NULL, scr);
    if (!p->win) {
        if (p->glist) { FreeGadgets(p->glist); p->glist = NULL; }
        if (p->vi)    { FreeVisualInfo(p->vi); p->vi = NULL; }
        return;
    }
    GT_RefreshWindow(p->win, NULL);

    p->x = bor_l + pad;
    p->y = status_y +
           (p->win->RPort->Font ? p->win->RPort->Font->tf_Baseline : (UWORD)(fh - 1));
}

void ProgressWin_Close(struct ProgressWin *p)
{
    if (!p) return;
    if (p->win) {
        RemoveGList(p->win, p->glist, -1);
        CloseWindow(p->win);
        p->win = NULL;
    }
    if (p->glist) { FreeGadgets(p->glist); p->glist = NULL; }
    if (p->vi)    { FreeVisualInfo(p->vi); p->vi = NULL; }
}

/* Drain any pending IntuiMessages, watching for cancel triggers
 * (Cancel button, close gadget, or ESC key). Sets p->cancelled if any
 * trigger fires. Non-blocking - returns immediately if no messages. */
static void prog_check_input(struct ProgressWin *p)
{
    struct IntuiMessage *imsg;
    if (!p->win) return;
    while ((imsg = GT_GetIMsg(p->win->UserPort)) != NULL) {
        ULONG          iclass = imsg->Class;
        UWORD          code   = imsg->Code;
        struct Gadget *gad    = (struct Gadget *)imsg->IAddress;
        GT_ReplyIMsg(imsg);
        switch (iclass) {
        case IDCMP_GADGETUP:
            if (gad && gad->GadgetID == PROG_GID_CANCEL)
                p->cancelled = TRUE;
            break;
        case IDCMP_CLOSEWINDOW:
            p->cancelled = TRUE;
            break;
        case IDCMP_VANILLAKEY:
            if (code == 27 /* ESC */) p->cancelled = TRUE;
            break;
        case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(p->win);
            GT_EndRefresh(p->win, TRUE);
            break;
        }
    }
}

BOOL ProgressWin_Callback(void *ud, ULONG cur, ULONG total)
{
    struct ProgressWin *p = (struct ProgressWin *)ud;
    char  line[96];
    UWORD len, pad;

    if (!p->win) return FALSE;

    /* Drain pending Intuition messages first so cancel is responsive
     * even when redrawing is throttled by the percentage check. */
    prog_check_input(p);
    if (p->cancelled) return FALSE;

    if (total > 0) {
        ULONG pct = (cur * 100UL) / total;
        if (cur != total && pct == p->last_pct) return TRUE;
        p->last_pct = pct;
        DP_SNPRINTF(line, GS(MSG_IMG_PROGRESS_PCT_FMT),
                (unsigned long)cur, (unsigned long)total,
                (unsigned long)pct);
    } else {
        DP_SNPRINTF(line, GS(MSG_IMG_PROGRESS_COPIED_FMT), (unsigned long)cur);
    }

    /* Pad to 60 chars so any previous longer text is fully erased. */
    len = (UWORD)strlen(line);
    for (pad = len; pad < 60 && pad < sizeof(line) - 1; pad++) line[pad] = ' ';
    line[(pad < sizeof(line)) ? pad : sizeof(line) - 1] = '\0';

    SetAPen(p->win->RPort, 1);
    Move(p->win->RPort, p->x, p->y);
    Text(p->win->RPort, line, strlen(line));
    return TRUE;
}
