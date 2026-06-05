/*
 * partview_image.c - Whole-disk image dump and restore (GUI side).
 *
 * Two operations available from the Advanced menu:
 *   image_dump_disk    : current disk -> image file
 *   image_restore_disk : image file -> current disk (DESTRUCTIVE)
 *
 * Both open a small progress window that updates in place during the
 * copy.  The actual block-level work lives in imagecopy.c.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "imagecopy.h"
#include "locale_support.h"
#include "partview_internal.h"

#define IMG_GID_CANCEL  100

extern struct Library *AslBase;

/* ------------------------------------------------------------------ */
/* Small progress window: one status line that gets repainted in      */
/* place every callback. No close gadget, no buttons - caller owns    */
/* the lifecycle (open before the copy, close after).                 */
/* ------------------------------------------------------------------ */

struct ImgProgress {
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

static void prog_open(struct ImgProgress *p, const char *title)
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
            ng.ng_GadgetID   = IMG_GID_CANCEL;
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

static void prog_close(struct ImgProgress *p)
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
static void prog_check_input(struct ImgProgress *p)
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
            if (gad && gad->GadgetID == IMG_GID_CANCEL)
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

/* ImageCopy progress callback: redraw the status line if the percentage
 * advanced (or unconditionally when total is unknown). Called from
 * inside ImageCopy_*; runs synchronously in the same task.
 *
 * Returns FALSE if the user clicked Cancel / closed the window / pressed
 * ESC, which tells ImageCopy_* to abort cleanly. */
static BOOL prog_cb(void *ud, ULONG cur, ULONG total)
{
    struct ImgProgress *p = (struct ImgProgress *)ud;
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
        sprintf(line, GS(MSG_IMG_PROGRESS_PCT_FMT),
                (unsigned long)cur, (unsigned long)total,
                (unsigned long)pct);
    } else {
        sprintf(line, GS(MSG_IMG_PROGRESS_COPIED_FMT), (unsigned long)cur);
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

/* ------------------------------------------------------------------ */
/* Dump current disk -> image file                                     */
/* ------------------------------------------------------------------ */

void image_dump_disk(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct es;
    static char       save_path[256];
    UQUAD             cap_bytes;
    UQUAD             disk_bytes;
    char              size_str[24], cap_str[24];

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_DEV_NOT_ACCESSIBLE);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    disk_bytes = bd->total_bytes;
    cap_bytes  = IMAGE_LARGE_THRESHOLD;
    (void)cap_str;

    /* Above 2 GB the destination filesystem must support large files
     * (SFS, PFS3, FFS-NSD, FFS post-OS3.5).  Warn but allow. */
    if (disk_bytes > cap_bytes) {
        char body[400];
        FormatSize(disk_bytes, size_str);
        sprintf(body, GS(MSG_IMG_DUMP_LARGE_WARN_FMT), size_str);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_WARN_TITLE);
        es.es_TextFormat=(UBYTE*)body;
        es.es_GadgetFormat=(UBYTE*)GS(MSG_IMG_CONTINUE_CANCEL);
        if (EasyRequest(win, &es, NULL) != 1) return;
    }

    if (!AslBase) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_ASL_UNAVAIL);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    {
        struct FileRequester *fr;
        BOOL chosen = FALSE;
        struct TagItem at[] = {
            { ASLFR_TitleText,    (ULONG)GS(MSG_IMG_SAVE_REQ_TITLE) },
            { ASLFR_DoSaveMode,   TRUE },
            { ASLFR_InitialDrawer,(ULONG)"RAM:" },
            { ASLFR_InitialFile,  (ULONG)"disk.hdf" },
            { TAG_DONE, 0 }
        };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at);
        if (fr) {
            if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
                strncpy(save_path, fr->fr_Drawer ? fr->fr_Drawer : "",
                        sizeof(save_path) - 1);
                save_path[sizeof(save_path) - 1] = '\0';
                AddPart((UBYTE *)save_path, (UBYTE *)fr->fr_File,
                        sizeof(save_path));
                chosen = TRUE;
            }
            FreeAslRequest(fr);
        }
        if (!chosen) return;
    }

    {
        static struct ImgProgress prog;
        char  errbuf[80];
        BOOL  ok;
        char  done_msg[300];

        sprintf(prog.title, GS(MSG_IMG_DUMP_PROGRESS_TITLE_FMT), save_path);
        prog_open(&prog, prog.title);

        errbuf[0] = '\0';
        ok = ImageCopy_DiskToFile(bd, save_path, 0,
                                  prog_cb, &prog, errbuf, sizeof(errbuf));
        prog_close(&prog);

        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_DUMP_TITLE);
        if (ok) {
            sprintf(done_msg, GS(MSG_IMG_DUMP_OK_FMT), save_path);
        } else if (prog.cancelled) {
            sprintf(done_msg, GS(MSG_IMG_DUMP_CANCELLED_FMT), save_path);
        } else {
            sprintf(done_msg, GS(MSG_IMG_DUMP_FAILED_FMT),
                    errbuf[0] ? errbuf : GS(MSG_IMG_UNKNOWN_ERROR));
        }
        es.es_TextFormat=(UBYTE*)done_msg;
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Restore image file -> current disk (DESTRUCTIVE)                    */
/* ------------------------------------------------------------------ */

void image_restore_disk(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct es;
    static char       load_path[256];

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_DEV_NOT_ACCESSIBLE);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    es.es_StructSize=sizeof(es); es.es_Flags=0;
    es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_WARN_TITLE);
    es.es_TextFormat=(UBYTE*)GS(MSG_IMG_RESTORE_WARN_BODY);
    es.es_GadgetFormat=(UBYTE*)GS(MSG_IMG_RESTORE_WARN_GADGETS);
    if (EasyRequest(win, &es, NULL) != 1) return;

    if (!AslBase) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_TITLE);
        es.es_TextFormat=(UBYTE*)GS(MSG_IMG_ASL_UNAVAIL);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL); return;
    }

    {
        struct FileRequester *fr;
        BOOL chosen = FALSE;
        struct TagItem at[] = {
            { ASLFR_TitleText,    (ULONG)GS(MSG_IMG_LOAD_REQ_TITLE) },
            { ASLFR_InitialDrawer,(ULONG)"RAM:" },
            { ASLFR_InitialFile,  (ULONG)"" },
            { TAG_DONE, 0 }
        };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at);
        if (fr) {
            if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
                strncpy(load_path, fr->fr_Drawer ? fr->fr_Drawer : "",
                        sizeof(load_path) - 1);
                load_path[sizeof(load_path) - 1] = '\0';
                AddPart((UBYTE *)load_path, (UBYTE *)fr->fr_File,
                        sizeof(load_path));
                chosen = TRUE;
            }
            FreeAslRequest(fr);
        }
        if (!chosen) return;
    }

    {
        static struct ImgProgress prog;
        char  errbuf[80];
        BOOL  ok;
        char  done_msg[300];

        sprintf(prog.title, GS(MSG_IMG_RESTORE_PROGRESS_TITLE_FMT), load_path);
        prog_open(&prog, prog.title);

        errbuf[0] = '\0';
        ok = ImageCopy_FileToDisk(bd, load_path,
                                  prog_cb, &prog, errbuf, sizeof(errbuf));
        prog_close(&prog);

        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)GS(MSG_IMG_RESTORE_TITLE);
        if (ok) {
            es.es_TextFormat=(UBYTE*)GS(MSG_IMG_RESTORE_OK);
        } else if (prog.cancelled) {
            es.es_TextFormat=(UBYTE*)GS(MSG_IMG_RESTORE_CANCELLED);
        } else {
            sprintf(done_msg, GS(MSG_IMG_RESTORE_FAILED_FMT),
                    errbuf[0] ? errbuf : GS(MSG_IMG_UNKNOWN_ERROR));
            es.es_TextFormat=(UBYTE*)done_msg;
        }
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
}
