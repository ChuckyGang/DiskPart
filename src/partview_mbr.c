/*
 * partview_mbr.c - MBR partition add/edit/zero dialogs for DiskPart.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>

#include "clib.h"
#include "locale_support.h"
#include "version.h"
#include "rdb.h"
#include "partmove.h"
#include "partview_internal.h"

extern struct ExecBase      *SysBase;
extern struct IntuitionBase *IntuitionBase;
extern struct Library       *GadToolsBase;
extern struct GfxBase       *GfxBase;

/* ------------------------------------------------------------------ */
/* Type cycle data (same order everywhere)                             */
/* ------------------------------------------------------------------ */

static const char *const s_cycle_labels[] = {
    "FAT32", "FAT32 LBA", "FAT16", "Linux", "Linux Swap", NULL
};
static const UBYTE s_cycle_types[] = {
    MBRT_FAT32, MBRT_FAT32LBA, MBRT_FAT16, MBRT_LINUX, MBRT_LINSWAP
};
#define MBR_CYCLE_COUNT 5

static WORD type_to_idx(UBYTE type)
{
    WORD i;
    for (i = 0; i < MBR_CYCLE_COUNT; i++)
        if (s_cycle_types[i] == type) return i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Gadget IDs - init RDB dialog                                        */
/* ------------------------------------------------------------------ */

#define GID_INIT_CHK   20
#define GID_INIT_BTN1  21   /* Re-init / Yes */
#define GID_INIT_BTN2  22   /* Update Geometry (has_rdb) / Manual... (no_rdb) */
#define GID_INIT_BTN3  23   /* Manual... (has_rdb) / No (no_rdb) */
#define GID_INIT_BTN4  24   /* Cancel (has_rdb only) */

/* ------------------------------------------------------------------ */
/* Gadget IDs - edit dialog                                            */
/* ------------------------------------------------------------------ */

#define GID_MTYPE   1
#define GID_MACTIVE 2
#define GID_MSTART  3
#define GID_MEND    4
#define GID_MOK     5
#define GID_MCANCEL 6

/* ------------------------------------------------------------------ */
/* Overlap check                                                        */
/* ------------------------------------------------------------------ */

static BOOL cyls_overlap(ULONG a_lo, ULONG a_hi, ULONG b_lo, ULONG b_hi)
{
    return (BOOL)(a_lo <= b_hi && b_lo <= a_hi);
}

/* Returns error string, or NULL if the range is valid. */
static const char *validate_mbr_range(ULONG lo, ULONG hi,
                                      const struct RDBInfo *rdb,
                                      const struct MBRInfo *mbr,
                                      UBYTE own_slot)
{
    UWORD i;

    if (lo > hi)
        return GS(MSG_MBR_OVERLAP);

    /* Must not overlap the RDB reserved area (cyl 0 .. lo_cyl-1). */
    if (rdb && rdb->valid && rdb->lo_cyl > 0) {
        if (cyls_overlap(lo, hi, 0, rdb->lo_cyl - 1))
            return GS(MSG_MBR_OVERLAP);
    } else if (lo == 0) {
        return GS(MSG_MBR_OVERLAP);
    }

    /* Must not overlap any RDB partition. */
    if (rdb && rdb->valid) {
        for (i = 0; i < rdb->num_parts; i++) {
            if (cyls_overlap(lo, hi,
                             rdb->parts[i].low_cyl,
                             rdb->parts[i].high_cyl))
                return GS(MSG_MBR_OVERLAP);
        }
    }

    /* Must not overlap other MBR partitions. */
    if (mbr && mbr->valid) {
        ULONG heads   = (rdb && rdb->valid && rdb->heads   > 0) ? rdb->heads   : 1;
        ULONG sectors = (rdb && rdb->valid && rdb->sectors > 0) ? rdb->sectors : 1;
        for (i = 0; i < MBR_MAX_PARTS; i++) {
            ULONG other_lo, other_hi;
            if (i == (UWORD)own_slot) continue;
            if (!mbr->parts[i].present) continue;
            other_lo = MBR_LBAToCyl(mbr->parts[i].lba_start, heads, sectors);
            other_hi = MBR_LBAToCyl(
                mbr->parts[i].lba_start + mbr->parts[i].lba_size - 1,
                heads, sectors);
            if (cyls_overlap(lo, hi, other_lo, other_hi))
                return GS(MSG_MBR_OVERLAP);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* init_rdb_dialog - Init RDB confirmation with "Add MBR" checkbox    */
/*                                                                     */
/* Replaces the EasyRequest for both "has RDB" and "no RDB" cases.    */
/* body  = pre-formatted body text (may contain \n).                  */
/* has_rdb = TRUE  → buttons: Re-init / Update Geometry / Manual... / Cancel */
/*           FALSE → buttons: Yes / Manual... / No                    */
/* Returns same values as the original EasyRequest would:             */
/*   has_rdb: 1=Re-init, 2=Update Geo, 3=Manual, 0=Cancel            */
/*   no_rdb:  1=Yes,     2=Manual,     0=No                           */
/* *out_add_mbr is set only when return value is 1.                   */
/* ------------------------------------------------------------------ */

static void split_nth(const char *s, UWORD n, char *buf, UWORD bufsz)
{
    UWORD i = 0;
    const char *end;
    UWORD len;
    while (i < n && *s) { if (*s++ == '|') i++; }
    end = s;
    while (*end && *end != '|') end++;
    len = (UWORD)(end - s);
    if (len >= bufsz) len = (UWORD)(bufsz - 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
}

static void draw_body_text(struct Window *win, const char *body, WORD tx, WORD ty)
{
    struct RastPort *rp = win->RPort;
    const char *p = body;
    WORD y = ty;
    SetAPen(rp, 1);
    SetBPen(rp, 0);
    SetDrMd(rp, JAM2);
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        if (nl > p) {
            Move(rp, tx, y + (WORD)rp->TxBaseline);
            Text(rp, (STRPTR)p, (ULONG)(nl - p));
        }
        y += (WORD)rp->TxHeight;
        p = (*nl == '\n') ? nl + 1 : nl;
    }
}

LONG init_rdb_dialog(struct Window *parent, const char *body,
                     BOOL has_rdb, BOOL *out_add_mbr)
{
    struct Screen       *scr;
    APTR                 vi;
    struct Gadget       *glist = NULL, *prev;
    struct Gadget       *chk_gad = NULL;
    struct NewGadget     ng;
    struct Window       *win = NULL;
    struct IntuiMessage *imsg;
    ULONG  class;
    APTR   iaddr;
    BOOL   running = TRUE;
    LONG   result = 0;
    UWORD  fh, bor_l, bor_t, bor_r, bor_b, pad;
    UWORD  btn_h, chk_h, btn_w, inner_w, win_w, win_h;
    UWORD  num_lines, text_h;
    WORD   text_ty, chk_y, btn_y, btn_y2;
    const char *cp;
    const char *gfmt;
    char   lbl0[32], lbl1[32], lbl2[32], lbl3[32];

    *out_add_mbr = FALSE;

    /* Count body text lines */
    num_lines = 1;
    for (cp = body; *cp; cp++) if (*cp == '\n') num_lines++;

    scr = parent ? parent->WScreen : NULL;
    if (!scr) return 0;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) return 0;

    fh    = (UWORD)scr->Font->ta_YSize;
    bor_l = (UWORD)scr->WBorLeft;
    bor_t = (UWORD)(scr->WBorTop + fh + 1);
    bor_r = (UWORD)scr->WBorRight;
    bor_b = (UWORD)scr->WBorBottom;
    pad   = 6;
    btn_h = (UWORD)(fh + 6);
    chk_h = (UWORD)(fh + 4);
    text_h = (UWORD)(num_lines * (fh + 2));

    /* button widths: has_rdb uses 2 rows of 2; no_rdb uses 1 row of 3 */
    inner_w = 360;
    btn_w   = has_rdb ? (UWORD)((inner_w - pad) / 2)
                      : (UWORD)((inner_w - 2 * pad) / 3);
    win_w   = (UWORD)(bor_l + pad + inner_w + pad + bor_r);

    /* has_rdb needs extra row for second pair of buttons */
    win_h = (UWORD)(bor_t + pad + text_h + pad + chk_h + pad + btn_h + pad
                    + (has_rdb ? btn_h + pad : 0) + bor_b);

    /* Extract button labels from the existing gadget format strings */
    gfmt = has_rdb ? GS(MSG_PV_INIT_HAS_RDB_GADGETS)
                   : GS(MSG_PV_INIT_NO_RDB_GADGETS);
    split_nth(gfmt, 0, lbl0, sizeof(lbl0));
    split_nth(gfmt, 1, lbl1, sizeof(lbl1));
    split_nth(gfmt, 2, lbl2, sizeof(lbl2));
    if (has_rdb) split_nth(gfmt, 3, lbl3, sizeof(lbl3));

    {
        struct TagItem wt[] = {
            { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
            { WA_Width,     win_w },
            { WA_Height,    win_h },
            { WA_Title,     (ULONG)GS(MSG_PV_INIT_TITLE) },
            { WA_PubScreen, (ULONG)scr },
            { WA_IDCMP,     IDCMP_GADGETUP | IDCMP_CLOSEWINDOW |
                            IDCMP_REFRESHWINDOW },
            { WA_Flags,     WFLG_DRAGBAR | WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH },
            { TAG_DONE, 0 }
        };
        win = OpenWindowTagList(NULL, wt);
    }
    if (!win) { FreeVisualInfo(vi); return 0; }

    prev = CreateContext(&glist);

    text_ty = (WORD)(bor_t + pad);
    chk_y   = (WORD)(bor_t + pad + text_h + pad);
    btn_y   = (WORD)(chk_y + chk_h + pad);
    btn_y2  = (WORD)(btn_y + btn_h + pad);

    /* Checkbox: Add MBR at block 0 */
    ng.ng_LeftEdge   = (WORD)(bor_l + pad);
    ng.ng_TopEdge    = chk_y;
    ng.ng_Width      = (UWORD)(fh + 2);
    ng.ng_Height     = chk_h;
    ng.ng_GadgetText = (UBYTE *)GS(MSG_MBR_INIT_CHK);
    ng.ng_TextAttr   = scr->Font;
    ng.ng_VisualInfo = vi;
    ng.ng_GadgetID   = GID_INIT_CHK;
    ng.ng_Flags      = PLACETEXT_RIGHT;
    {
        struct TagItem ct[] = { { GTCB_Checked, FALSE }, { TAG_DONE, 0 } };
        chk_gad = prev = CreateGadgetA(CHECKBOX_KIND, prev, &ng, ct);
    }

    ng.ng_Height = btn_h;
    ng.ng_Flags  = PLACETEXT_IN;

    if (has_rdb) {
        /* Row 1: Re-init | Update Geometry */
        ng.ng_TopEdge    = btn_y;
        ng.ng_Width      = btn_w;
        ng.ng_LeftEdge   = (WORD)(bor_l + pad);
        ng.ng_GadgetText = (UBYTE *)lbl0;
        ng.ng_GadgetID   = GID_INIT_BTN1;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);

        ng.ng_LeftEdge   = (WORD)(bor_l + pad + btn_w + pad);
        ng.ng_GadgetText = (UBYTE *)lbl1;
        ng.ng_GadgetID   = GID_INIT_BTN2;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);

        /* Row 2: Manual... | Cancel */
        ng.ng_TopEdge    = btn_y2;
        ng.ng_LeftEdge   = (WORD)(bor_l + pad);
        ng.ng_GadgetText = (UBYTE *)lbl2;
        ng.ng_GadgetID   = GID_INIT_BTN3;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);

        ng.ng_LeftEdge   = (WORD)(bor_l + pad + btn_w + pad);
        ng.ng_GadgetText = (UBYTE *)lbl3;
        ng.ng_GadgetID   = GID_INIT_BTN4;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
    } else {
        /* Single row: Yes | Manual... | No */
        ng.ng_TopEdge    = btn_y;
        ng.ng_Width      = btn_w;
        ng.ng_LeftEdge   = (WORD)(bor_l + pad);
        ng.ng_GadgetText = (UBYTE *)lbl0;
        ng.ng_GadgetID   = GID_INIT_BTN1;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);

        ng.ng_LeftEdge   = (WORD)(bor_l + pad + btn_w + pad);
        ng.ng_GadgetText = (UBYTE *)lbl1;
        ng.ng_GadgetID   = GID_INIT_BTN2;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);

        ng.ng_LeftEdge   = (WORD)(bor_l + pad + (btn_w + pad) * 2);
        ng.ng_GadgetText = (UBYTE *)lbl2;
        ng.ng_GadgetID   = GID_INIT_BTN3;
        prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
    }

    if (!prev || !chk_gad) {
        if (glist) FreeGadgets(glist);
        CloseWindow(win);
        FreeVisualInfo(vi);
        return 0;
    }

    AddGList(win, glist, (UWORD)-1, -1, NULL);
    RefreshGList(glist, win, NULL, -1);
    GT_RefreshWindow(win, NULL);
    draw_body_text(win, body, (WORD)(bor_l + pad), text_ty);

    while (running) {
        WaitPort(win->UserPort);
        while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
            class = imsg->Class;
            iaddr = imsg->IAddress;
            GT_ReplyIMsg(imsg);

            if (class == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                draw_body_text(win, body, (WORD)(bor_l + pad), text_ty);
                GT_EndRefresh(win, TRUE);
                continue;
            }
            if (class == IDCMP_CLOSEWINDOW) { running = FALSE; break; }
            if (class == IDCMP_GADGETUP) {
                UWORD gid = ((struct Gadget *)iaddr)->GadgetID;
                if (gid == GID_INIT_BTN1) {
                    *out_add_mbr = (BOOL)((chk_gad->Flags & GFLG_SELECTED) != 0);
                    result = 1; running = FALSE; break;
                }
                if (gid == GID_INIT_BTN2) { result = 2; running = FALSE; break; }
                if (gid == GID_INIT_BTN3) {
                    result = has_rdb ? 3 : 0; running = FALSE; break;
                }
                if (gid == GID_INIT_BTN4) { result = 0; running = FALSE; break; }
            }
        }
    }

    RemoveGList(win, glist, -1);
    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    return result;
}

/* ------------------------------------------------------------------ */
/* mbr_edit_dialog - common implementation for add and edit            */
/* own_slot = MBR_MAX_PARTS means add mode (skip self-check)          */
/* ------------------------------------------------------------------ */

static BOOL mbr_edit_dialog(struct Window *parent, struct BlockDev *bd,
                            struct MBRInfo *mbr, const struct RDBInfo *rdb,
                            UBYTE own_slot, struct MBRPart *result)
{
    struct Screen   *scr;
    APTR             vi;
    struct Gadget   *glist = NULL, *prev;
    struct Gadget   *type_gad   = NULL;
    struct Gadget   *active_gad = NULL;
    struct Gadget   *start_gad  = NULL;
    struct Gadget   *end_gad    = NULL;
    struct NewGadget ng;
    struct Window   *win = NULL;
    struct IntuiMessage *imsg;
    ULONG  class;
    UWORD  code;
    APTR   iaddr;
    BOOL   running = TRUE, ok = FALSE;
    char   start_buf[16], end_buf[20];
    BOOL   is_add = (own_slot >= MBR_MAX_PARTS);

    ULONG heads   = (rdb && rdb->valid && rdb->heads   > 0) ? rdb->heads   : 0;
    ULONG sectors = (rdb && rdb->valid && rdb->sectors > 0) ? rdb->sectors : 0;

    WORD  cur_type_idx;
    ULONG init_lo, init_hi;

    if (is_add) {
        cur_type_idx = 0;
        init_lo = (rdb && rdb->valid) ? rdb->lo_cyl : 1;
        init_hi = (rdb && rdb->valid) ? rdb->hi_cyl : init_lo;
        /* Default start: after last RDB partition */
        if (rdb && rdb->valid && rdb->num_parts > 0) {
            ULONG last_hi = 0;
            UWORD k;
            for (k = 0; k < rdb->num_parts; k++)
                if (rdb->parts[k].high_cyl > last_hi)
                    last_hi = rdb->parts[k].high_cyl;
            if (last_hi + 1 <= rdb->hi_cyl)
                init_lo = last_hi + 1;
        }
        if (init_lo > init_hi) init_lo = init_hi;
    } else {
        struct MBRPart *p = &mbr->parts[own_slot];
        cur_type_idx = (WORD)type_to_idx(p->type);
        init_lo = (heads && sectors) ?
            MBR_LBAToCyl(p->lba_start, heads, sectors) : 0;
        init_hi = (heads && sectors) ?
            MBR_LBAToCyl(p->lba_start + p->lba_size - 1, heads, sectors) : 0;
    }

    sprintf(start_buf, "%lu", (unsigned long)init_lo);
    sprintf(end_buf,   "%lu", (unsigned long)init_hi);

    scr = parent ? parent->WScreen : NULL;
    if (!scr) return FALSE;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) return FALSE;

    {
        UWORD fh    = scr->Font->ta_YSize;
        UWORD bor_l = (UWORD)scr->WBorLeft;
        UWORD bor_t = (UWORD)scr->WBorTop + fh + 1;
        UWORD bor_r = (UWORD)scr->WBorRight;
        UWORD bor_b = (UWORD)scr->WBorBottom;
        UWORD pad   = 6;
        UWORD lbl_w = 110;
        UWORD gad_h = fh + 6;
        UWORD chk_h = fh + 4;
        UWORD str_w = 100;
        UWORD btn_w = 70;
        UWORD inner_w = lbl_w + 4 + str_w;
        UWORD win_w   = bor_l + pad + inner_w + pad + bor_r;
        UWORD row_s   = (UWORD)(fh + 8);
        UWORD btn_row_h = gad_h + pad;
        UWORD content_h = gad_h + row_s + chk_h + row_s + gad_h + row_s
                        + gad_h + row_s + btn_row_h;
        UWORD win_h   = bor_t + pad + content_h + bor_b;
        WORD  gad_x   = (WORD)(bor_l + pad + lbl_w + 4);
        WORD  row_y   = (WORD)(bor_t + pad);

        (void)row_s;

        if (win_w < 300) win_w = 300;

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)(is_add ? GS(MSG_MBR_DLG_ADD_TITLE)
                                               : GS(MSG_MBR_DLG_EDIT_TITLE)) },
                { WA_PubScreen, (ULONG)scr },
                { WA_Gadgets,   NULL },
                { WA_IDCMP,     IDCMP_GADGETUP | IDCMP_GADGETDOWN |
                                IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_CLOSEGADGET |
                                WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
        if (!win) { FreeVisualInfo(vi); return FALSE; }

        prev = CreateContext(&glist);

        /* Row 0: Type cycle */
        ng.ng_LeftEdge   = gad_x;
        ng.ng_TopEdge    = row_y;
        ng.ng_Width      = str_w;
        ng.ng_Height     = gad_h;
        ng.ng_GadgetText = (UBYTE *)GS(MSG_MBR_DLG_TYPE);
        ng.ng_TextAttr   = scr->Font;
        ng.ng_VisualInfo = vi;
        ng.ng_GadgetID   = GID_MTYPE;
        ng.ng_Flags      = PLACETEXT_LEFT;
        {
            struct TagItem ct[] = {
                { GTCY_Labels, (ULONG)s_cycle_labels     },
                { GTCY_Active, (ULONG)cur_type_idx       },
                { TAG_DONE, 0 }
            };
            type_gad = prev = CreateGadgetA(CYCLE_KIND, prev, &ng, ct);
        }
        row_y += (WORD)(gad_h + pad);

        /* Row 1: Active checkbox */
        ng.ng_LeftEdge   = gad_x;
        ng.ng_TopEdge    = row_y;
        ng.ng_Width      = str_w;
        ng.ng_Height     = chk_h;
        ng.ng_GadgetText = (UBYTE *)GS(MSG_MBR_DLG_ACTIVE);
        ng.ng_GadgetID   = GID_MACTIVE;
        ng.ng_Flags      = PLACETEXT_RIGHT;
        {
            BOOL init_active = is_add ? FALSE : mbr->parts[own_slot].active;
            struct TagItem ct[] = {
                { GTCB_Checked, (ULONG)init_active },
                { TAG_DONE, 0 }
            };
            active_gad = prev = CreateGadgetA(CHECKBOX_KIND, prev, &ng, ct);
        }
        row_y += (WORD)(chk_h + pad);

        /* Row 2: Start Cylinder */
        ng.ng_LeftEdge   = gad_x;
        ng.ng_TopEdge    = row_y;
        ng.ng_Width      = str_w;
        ng.ng_Height     = gad_h;
        ng.ng_GadgetText = (UBYTE *)GS(MSG_MBR_DLG_STARTCYL);
        ng.ng_GadgetID   = GID_MSTART;
        ng.ng_Flags      = PLACETEXT_LEFT;
        {
            struct TagItem st[] = {
                { GTST_String,   (ULONG)start_buf },
                { GTST_MaxChars, 12               },
                { TAG_DONE, 0 }
            };
            start_gad = prev = CreateGadgetA(STRING_KIND, prev, &ng, st);
        }
        row_y += (WORD)(gad_h + pad);

        /* Row 3: End Cylinder  (also accepts +<n>K/M/G for size from start) */
        ng.ng_LeftEdge   = gad_x;
        ng.ng_TopEdge    = row_y;
        ng.ng_Width      = str_w;
        ng.ng_Height     = gad_h;
        ng.ng_GadgetText = (UBYTE *)GS(MSG_MBR_DLG_ENDCYL);
        ng.ng_GadgetID   = GID_MEND;
        ng.ng_Flags      = PLACETEXT_LEFT;
        {
            struct TagItem st[] = {
                { GTST_String,   (ULONG)end_buf   },
                { GTST_MaxChars, 16               },
                { TAG_DONE, 0 }
            };
            end_gad = prev = CreateGadgetA(STRING_KIND, prev, &ng, st);
        }
        row_y += (WORD)(gad_h + pad);

        /* Buttons */
        {
            WORD b1x = (WORD)(bor_l + pad);
            WORD b2x = (WORD)(win_w - bor_r - pad - btn_w);

            ng.ng_LeftEdge   = b1x;
            ng.ng_TopEdge    = row_y;
            ng.ng_Width      = btn_w;
            ng.ng_Height     = gad_h;
            ng.ng_GadgetText = (UBYTE *)GS(MSG_OK);
            ng.ng_GadgetID   = GID_MOK;
            ng.ng_Flags      = PLACETEXT_IN;
            prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);

            ng.ng_LeftEdge   = b2x;
            ng.ng_GadgetText = (UBYTE *)GS(MSG_CANCEL);
            ng.ng_GadgetID   = GID_MCANCEL;
            prev = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
        }

        if (!prev || !type_gad || !active_gad || !start_gad || !end_gad) {
            if (glist) FreeGadgets(glist);
            CloseWindow(win);
            FreeVisualInfo(vi);
            return FALSE;
        }

        AddGList(win, glist, (UWORD)-1, -1, NULL);
        RefreshGList(glist, win, NULL, -1);
        GT_RefreshWindow(win, NULL);
    }

    /* Event loop */
    while (running) {
        WaitPort(win->UserPort);
        while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
            class = imsg->Class;
            code  = imsg->Code;
            iaddr = imsg->IAddress;
            GT_ReplyIMsg(imsg);

            if (class == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
                continue;
            }
            if (class == IDCMP_CLOSEWINDOW) {
                running = FALSE; break;
            }
            if (class == IDCMP_GADGETUP || class == IDCMP_GADGETDOWN) {
                UWORD gid = ((struct Gadget *)iaddr)->GadgetID;

                if (gid == GID_MTYPE)
                    cur_type_idx = (WORD)code;

                if (gid == GID_MCANCEL) { running = FALSE; break; }

                if (gid == GID_MOK) {
                    struct StringInfo *si;
                    ULONG lo_cyl, hi_cyl;
                    const char *err;
                    struct EasyStruct es;

                    /* Read start cylinder */
                    si = (struct StringInfo *)start_gad->SpecialInfo;
                    lo_cyl = (ULONG)strtol((char *)si->Buffer, NULL, 10);

                    /* Read end cylinder.
                       "<n>[K|M|G]" or "+<n>[K|M|G]" = size from start cyl.
                       Pure number (with or without leading +) = cylinder value. */
                    si = (struct StringInfo *)end_gad->SpecialInfo;
                    {
                        const char *p = (const char *)si->Buffer;
                        char *endp;
                        ULONG size_num;
                        char  suf;
                        if (*p == '+') p++;
                        size_num = (ULONG)strtoul(p, &endp, 10);
                        suf = *endp;
                        if (suf == 'K' || suf == 'k' || suf == 'M' || suf == 'm' ||
                            suf == 'G' || suf == 'g') {
                            UQUAD mult = (suf == 'K' || suf == 'k') ? (UQUAD)1024 :
                                         (suf == 'M' || suf == 'm') ? (UQUAD)1024 * 1024 :
                                                                       (UQUAD)1024 * 1024 * 1024;
                            UQUAD size_secs = ((UQUAD)size_num * mult) >> 9;
                            ULONG lba_start = MBR_CylToLBA(lo_cyl, heads, sectors);
                            UQUAD lba_end   = (UQUAD)lba_start + (size_secs > 0 ? size_secs - 1 : 0);
                            ULONG cyl_secs  = heads * sectors;
                            hi_cyl = (cyl_secs > 0) ? (ULONG)(lba_end / cyl_secs) : lo_cyl;
                            if (hi_cyl < lo_cyl) hi_cyl = lo_cyl;
                        } else {
                            hi_cyl = (ULONG)strtoul((const char *)si->Buffer, NULL, 10);
                        }
                    }

                    if (heads == 0 || sectors == 0) {
                        es.es_StructSize   = sizeof(es);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
                        es.es_TextFormat   = (UBYTE *)GS(MSG_MBR_GEOM_ZERO);
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                        EasyRequest(win, &es, NULL);
                        continue;
                    }

                    err = validate_mbr_range(lo_cyl, hi_cyl, rdb, mbr,
                                             own_slot);
                    if (err) {
                        es.es_StructSize   = sizeof(es);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
                        es.es_TextFormat   = (UBYTE *)err;
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                        EasyRequest(win, &es, NULL);
                        continue;
                    }

                    result->type      = s_cycle_types[cur_type_idx];
                    result->active    = (BOOL)((active_gad->Flags & GFLG_SELECTED) != 0);
                    result->present   = TRUE;
                    result->lba_start = MBR_CylToLBA(lo_cyl, heads, sectors);
                    result->lba_size  = MBR_CylToLBA(hi_cyl + 1, heads, sectors)
                                      - result->lba_start;
                    if (result->lba_size == 0)
                        result->lba_size = heads * sectors;

                    ok = TRUE;
                    running = FALSE;
                    break;
                }
            }
        }
    }

    RemoveGList(win, glist, -1);
    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    return ok;
}

/* ------------------------------------------------------------------ */
/* offer_add_mbr_part                                                   */
/* ------------------------------------------------------------------ */

void offer_add_mbr_part(struct Window *win, struct BlockDev *bd,
                        struct MBRInfo *mbr, const struct RDBInfo *rdb)
{
    struct EasyStruct es;
    struct MBRPart result;
    UBYTE  free_slot;
    UWORD  i;

    es.es_StructSize = sizeof(es);
    es.es_Flags      = 0;
    es.es_Title      = (UBYTE *)DISKPART_VERTITLE;

    if (!mbr || !mbr->valid) {
        es.es_TextFormat   = (UBYTE *)GS(MSG_MBR_NO_MBR);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
        return;
    }

    free_slot = MBR_MAX_PARTS;
    for (i = 0; i < MBR_MAX_PARTS; i++) {
        if (!mbr->parts[i].present) { free_slot = (UBYTE)i; break; }
    }
    if (free_slot >= MBR_MAX_PARTS) {
        es.es_TextFormat   = (UBYTE *)GS(MSG_MBR_FULL);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
        return;
    }

    memset(&result, 0, sizeof(result));
    if (!mbr_edit_dialog(win, bd, mbr, rdb, MBR_MAX_PARTS, &result))
        return;

    snprintf(result.name, sizeof(result.name), "MBR%u", free_slot + 1);
    mbr->parts[free_slot] = result;

    if (!MBR_Write(bd, mbr)) {
        mbr->parts[free_slot].present = FALSE;
        es.es_Title        = (UBYTE *)GS(MSG_ERROR_TITLE);
        es.es_TextFormat   = (UBYTE *)GS(MSG_MBR_WRITE_FAIL);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* offer_edit_mbr_part                                                  */
/* ------------------------------------------------------------------ */

void offer_edit_mbr_part(struct Window *win, struct BlockDev *bd,
                         struct MBRInfo *mbr, const struct RDBInfo *rdb,
                         UBYTE slot)
{
    struct EasyStruct es;
    struct MBRPart backup, result;

    if (!mbr || !mbr->valid || slot >= MBR_MAX_PARTS) return;
    if (!mbr->parts[slot].present) return;

    backup = mbr->parts[slot];
    memset(&result, 0, sizeof(result));

    if (!mbr_edit_dialog(win, bd, mbr, rdb, slot, &result))
        return;

    snprintf(result.name, sizeof(result.name), "MBR%u", slot + 1);
    mbr->parts[slot] = result;

    if (!MBR_Write(bd, mbr)) {
        mbr->parts[slot] = backup;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_ERROR_TITLE);
        es.es_TextFormat   = (UBYTE *)GS(MSG_MBR_WRITE_FAIL);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* offer_zero_mbr_part                                                  */
/* ------------------------------------------------------------------ */

void offer_zero_mbr_part(struct Window *win, struct BlockDev *bd,
                         const struct RDBInfo *rdb, const struct MBRInfo *mbr,
                         UBYTE slot)
{
    struct PartInfo fake;
    ULONG heads, sectors;

    if (!mbr || !mbr->valid || slot >= MBR_MAX_PARTS) return;
    if (!mbr->parts[slot].present) return;
    if (!rdb || !rdb->valid) return;

    heads   = rdb->heads   > 0 ? rdb->heads   : 1;
    sectors = rdb->sectors > 0 ? rdb->sectors : 1;

    memset(&fake, 0, sizeof(fake));
    strncpy(fake.drive_name, mbr->parts[slot].name,
            sizeof(fake.drive_name) - 1);
    fake.low_cyl    = MBR_LBAToCyl(mbr->parts[slot].lba_start, heads, sectors);
    fake.high_cyl   = MBR_LBAToCyl(
        mbr->parts[slot].lba_start + mbr->parts[slot].lba_size - 1,
        heads, sectors);
    fake.heads      = rdb->heads;
    fake.sectors    = rdb->sectors;
    fake.block_size = 512;

    offer_zero_partition(win, bd, rdb, &fake);
}
