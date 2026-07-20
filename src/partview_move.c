/*
 * partview_move.c - Partition move and filesystem grow operations.
 *
 * Contains: check_ffs_root, move_progress_fn, draw_move_warn_text,
 *           offer_move_partition, ffs_grow_progress, offer_ffs_grow,
 *           offer_pfs_grow, offer_sfs_grow.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "devices.h"
#include "version.h"
#include "ffsresize.h"
#include "pfsresize.h"
#include "shrinkinfo.h"
#include "sfsresize.h"
#include "partmove.h"
#include "partview_internal.h"
#include "quickformat.h"
#include "locale_support.h"

extern struct ExecBase      *SysBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *GadToolsBase;

void check_ffs_root(struct Window *win, struct BlockDev *bd,
                            const struct RDBInfo *rdb, WORD sel)
{
    struct EasyStruct es;
    static char msg[640];
    ULONG *buf = NULL;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_MOVE_CHK_ROOT_TITLE);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);

    if (!bd) {
        es.es_TextFormat = (UBYTE *)GS(MSG_MOVE_NO_DEVICE);
        EasyRequest(win, &es, NULL);
        return;
    }
    if (!rdb || sel < 0 || (ULONG)sel >= rdb->num_parts) {
        es.es_TextFormat = (UBYTE *)GS(MSG_MOVE_NO_PART_SEL);
        EasyRequest(win, &es, NULL);
        return;
    }

    const struct PartInfo *pi = &rdb->parts[sel];
    ULONG heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;

    if (heads == 0 || sectors == 0) {
        es.es_TextFormat = (UBYTE *)GS(MSG_MOVE_GEOM_ZERO);
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (ULONG *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        es.es_TextFormat = (UBYTE *)GS(MSG_MOVE_OUT_OF_MEM);
        EasyRequest(win, &es, NULL);
        return;
    }

    ULONG part_abs   = pi->low_cyl * heads * sectors;
    ULONG num_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;
    ULONG root       = num_blocks / 2;
    ULONG root_abs   = part_abs + root;

    if (!BlockDev_ReadBlock(bd, root_abs, buf)) {
        DP_SNPRINTF(msg,
                GS(MSG_MOVE_CHK_READ_FAIL_FMT),
                pi->drive_name,
                (unsigned long)part_abs,
                (unsigned long)num_blocks,
                (unsigned long)root,
                (unsigned long)root_abs,
                (unsigned long)root_abs);
        es.es_TextFormat = (UBYTE *)msg;
        EasyRequest(win, &es, NULL);
        FreeVec(buf);
        return;
    }

    /* Verify checksum: sum of all 128 longs must be 0 */
    ULONG sum = 0;
    for (ULONG i = 0; i < 128; i++) sum += buf[i];
    BOOL cs_ok     = (sum == 0);
    BOOL type_ok   = (buf[0] == 2);          /* T_SHORT */
    BOOL sec_ok    = (buf[127] == 1);        /* ST_ROOT */
    BOOL own_ok    = (buf[1] == root);
    BOOL bm_valid  = (buf[78] == 0xFFFFFFFFUL);
    /* FFS does NOT validate own_key - confirmed: KS 3.1 accepts own_key=0 on
       live partitions. own_ok is informational only. */
    BOOL looks_ok  = type_ok && sec_ok && cs_ok && bm_valid;

    /* Also read boot block to show bb[2] */
    ULONG bb2 = 0;
    {
        ULONG *bb = (ULONG *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
        if (bb) {
            if (BlockDev_ReadBlock(bd, part_abs, bb))
                bb2 = bb[2];
            FreeVec(bb);
        }
    }

    /* disk_size (L[4]) = total partition blocks; root should be at disk_size/2 */
    ULONG disk_size   = buf[4];
    BOOL  dsz_ok      = (disk_size == num_blocks);

    DP_SNPRINTF(msg,
            GS(MSG_MOVE_CHK_REPORT_FMT),
            pi->drive_name, (unsigned long)heads, (unsigned long)sectors,
            (unsigned long)root, (unsigned long)root_abs,
            (unsigned long)bb2,
            (unsigned long)buf[0],  type_ok ? GS(MSG_MOVE_CHK_OK)
                                            : GS(MSG_MOVE_CHK_WRONG_2),
            (unsigned long)buf[1],  (unsigned long)root,
                own_ok ? "" : GS(MSG_MOVE_CHK_FFS_IGNORES),
            (unsigned long)disk_size, (unsigned long)num_blocks,
                dsz_ok ? "" : GS(MSG_MOVE_CHK_MISMATCH),
            cs_ok ? GS(MSG_MOVE_CHK_YES) : GS(MSG_MOVE_CHK_NO),
            (unsigned long)buf[78], bm_valid ? GS(MSG_MOVE_CHK_VALID)
                                             : GS(MSG_MOVE_CHK_INVALID),
            (unsigned long)buf[127], sec_ok ? GS(MSG_MOVE_CHK_OK)
                                            : GS(MSG_MOVE_CHK_WRONG_1),
            (unsigned long)buf[79],
            (unsigned long)buf[104],
            looks_ok ? GS(MSG_MOVE_CHK_ROOT_VALID)
                     : GS(MSG_MOVE_CHK_ROOT_INVALID));

    es.es_TextFormat = (UBYTE *)msg;
    EasyRequest(win, &es, NULL);
    FreeVec(buf);
}

/* ================================================================== */
/* Partition Move                                                       */
/* ================================================================== */

/* Gadget IDs for the move confirmation dialog */
#define MVDLG_NEWCYL  201
#define MVDLG_BACKUP  202
#define MVDLG_MOVE    203
#define MVDLG_CANCEL  204

/* ------------------------------------------------------------------ */
/* Progress callback for PART_Move.                                    */
/* Draws a filled bar with %, then the phase text below it.            */
/* ------------------------------------------------------------------ */
struct MoveProgUD {
    struct Window *win;
};

static void move_progress_fn(void *ud, ULONG done, ULONG total, const char *phase)
{
    struct MoveProgUD *pu = (struct MoveProgUD *)ud;
    struct Window   *pw;
    struct RastPort *rp;
    WORD x1, y1, x2;
    WORD bar_x, bar_y, bar_w, bar_h, filled_w;
    char  pct_str[8];
    ULONG pct;
    UWORD plen, phlen, i;

    if (!pu || !pu->win) return;
    pw = pu->win;

    rp = pw->RPort;
    x1 = pw->BorderLeft;
    y1 = pw->BorderTop;
    x2 = (WORD)(pw->Width  - 1 - pw->BorderRight);

    /* Clear interior */
    SetAPen(rp, 0);
    RectFill(rp, x1, y1, x2, (WORD)(pw->Height - 1 - pw->BorderBottom));

    bar_x = (WORD)(x1 + 4);
    bar_y = (WORD)(y1 + 4);
    bar_w = (WORD)(x2 - x1 - 8);
    bar_h = 6;

    if (bar_w > 0) {
        pct      = (total > 0) ? (done * 100UL / total) : 0;
        filled_w = (total > 0) ? (WORD)((ULONG)bar_w * done / total) : 0;

        /* Empty track */
        SetAPen(rp, 2);
        RectFill(rp, bar_x, bar_y,
                 (WORD)(bar_x + bar_w), (WORD)(bar_y + bar_h));
        /* Filled portion */
        if (filled_w > 0) {
            SetAPen(rp, 3);
            RectFill(rp, bar_x, bar_y,
                     (WORD)(bar_x + filled_w), (WORD)(bar_y + bar_h));
        }
        /* Border */
        SetAPen(rp, 1);
        Move(rp, bar_x, bar_y);
        Draw(rp, (WORD)(bar_x + bar_w), bar_y);
        Draw(rp, (WORD)(bar_x + bar_w), (WORD)(bar_y + bar_h));
        Draw(rp, bar_x, (WORD)(bar_y + bar_h));
        Draw(rp, bar_x, bar_y);

        /* Percentage string */
        pct_str[0] = (UBYTE)('0' + pct / 100);
        pct_str[1] = (UBYTE)('0' + (pct / 10) % 10);
        pct_str[2] = (UBYTE)('0' + pct % 10);
        pct_str[3] = '%';
        pct_str[4] = '\0';
        /* Trim leading zeros but keep at least "0%" */
        plen = 4;
        if (pct_str[0] == '0') {
            pct_str[0] = pct_str[1];
            pct_str[1] = pct_str[2];
            pct_str[2] = pct_str[3];
            pct_str[3] = '\0'; plen = 3;
            if (pct_str[0] == '0') {
                pct_str[0] = pct_str[1];
                pct_str[1] = pct_str[2];
                pct_str[2] = '\0'; plen = 2;
            }
        }
        /* Draw % centered in bar (pen 1 over unfilled, pen 0 over filled) */
        {
            WORD tx = (WORD)(bar_x + (bar_w - (WORD)(plen * rp->TxWidth)) / 2);
            WORD ty = (WORD)(bar_y + 1 + rp->TxBaseline);
            if (ty < (WORD)(bar_y + bar_h)) {
                SetAPen(rp, 1);
                Move(rp, tx, ty);
                Text(rp, (STRPTR)pct_str, (WORD)plen);
            }
        }
    }

    /* Phase text */
    SetAPen(rp, 1);
    phlen = 0; while (phase[phlen]) phlen++;
    Move(rp, (WORD)(x1 + 6),
         (WORD)(bar_y + bar_h + 4 + rp->TxBaseline));
    Text(rp, (STRPTR)phase, (WORD)phlen);

    (void)i; /* suppress unused warning */
}

/* ------------------------------------------------------------------ */
/* Draw static warning text in the move confirm dialog.               */
/* Called on open and on every IDCMP_REFRESHWINDOW.                    */
/* ------------------------------------------------------------------ */
static void draw_move_warn_text(struct Window *pw,
                                 const char *pname,
                                 ULONG lo, ULONG hi)
{
    struct RastPort *rp = pw->RPort;
    WORD x  = (WORD)(pw->BorderLeft + 4);
    WORD lh = (WORD)(rp->TxHeight + 2);
    WORD y  = (WORD)(pw->BorderTop + 4);
    static char line[72];
    UWORD len;

#define WTEXT(s) \
    len = 0; while ((s)[len]) len++; \
    Move(rp, x, (WORD)(y + rp->TxBaseline)); \
    Text(rp, (STRPTR)(s), (WORD)len); \
    y = (WORD)(y + lh);

    SetAPen(rp, 1);

    WTEXT(GS(MSG_MOVE_WARN_L1))
    y = (WORD)(y + lh / 2);

    DP_SNPRINTF(line, GS(MSG_MOVE_WARN_PART_FMT),
            pname, (unsigned long)lo, (unsigned long)hi);
    WTEXT(line)
    WTEXT(GS(MSG_MOVE_WARN_COPIED))

    y = (WORD)(y + lh / 2);

    WTEXT(GS(MSG_MOVE_WARN_POWER1))
    WTEXT(GS(MSG_MOVE_WARN_POWER2))

    y = (WORD)(y + lh / 2);

    WTEXT(GS(MSG_MOVE_WARN_TIME1))
    WTEXT(GS(MSG_MOVE_WARN_TIME2))
    WTEXT(GS(MSG_MOVE_WARN_TIME3))

#undef WTEXT
}

/* ------------------------------------------------------------------ */
/* offer_move_partition                                                 */
/* ------------------------------------------------------------------ */
BOOL offer_move_partition(struct Window *win,
                                  struct BlockDev *bd,
                                  struct RDBInfo *rdb,
                                  struct PartInfo *pi,
                                  ULONG default_lo)   /* 0 = use pi->low_cyl */
{
    struct Screen  *scr     = NULL;
    APTR            vi      = NULL;
    struct Gadget  *glist   = NULL;
    struct Gadget  *gctx    = NULL;
    struct Gadget  *cyl_gad = NULL;
    struct Gadget  *chk_gad = NULL;
    struct Gadget  *btn_move = NULL;
    struct Window  *dlg     = NULL;
    BOOL   result    = FALSE;
    BOOL   running   = FALSE;
    BOOL   do_move   = FALSE;
    BOOL   backup_ok = FALSE;
    ULONG  new_lo    = 0;
    char   cyl_str[12];
    char   err_buf[256];

    UWORD font_h, bor_l, bor_t, bor_b, inner_w, pad, row_h;
    UWORD win_w, win_h, warn_h, gad_x, gad_w;
    UWORD str_y, chk_y, btn_y, half;

    DP_SNPRINTF(cyl_str, "%lu",
            (unsigned long)(default_lo ? default_lo : pi->low_cyl));

    scr = LockPubScreen(NULL);
    if (!scr) return FALSE;

    font_h  = scr->Font ? scr->Font->ta_YSize : 8;
    bor_l   = scr->WBorLeft;
    bor_t   = (UWORD)(scr->WBorTop + font_h + 1);
    bor_b   = scr->WBorBottom;
    pad     = 4;
    row_h   = (UWORD)(font_h + 4);
    inner_w = 420;
    win_w   = (UWORD)(bor_l + inner_w + scr->WBorRight);

    /* 8 warning text lines + 3 half-gaps */
    warn_h  = (UWORD)((8 + 2) * (font_h + 2) + pad * 2 + (font_h + 2));

    str_y = (UWORD)(bor_t + warn_h);
    chk_y = (UWORD)(str_y + row_h + pad);
    btn_y = (UWORD)(chk_y + row_h + pad);
    win_h = (UWORD)(btn_y + row_h + pad + bor_b);

    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto mv_cleanup;

    gctx = CreateContext(&glist);
    if (!gctx) goto mv_cleanup;

    gad_x = (UWORD)(bor_l + 110);
    gad_w = (UWORD)(inner_w - 110 - pad);

    /* STRING_KIND: new start cylinder */
    {
        struct NewGadget ng;
        struct TagItem st[] = {
            { GTST_String,   (ULONG)cyl_str },
            { GTST_MaxChars, 10 },
            { TAG_DONE, 0 }
        };
        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;
        ng.ng_LeftEdge   = gad_x;
        ng.ng_TopEdge    = str_y;
        ng.ng_Width      = (UWORD)(gad_w / 2);
        ng.ng_Height     = row_h;
        ng.ng_GadgetText = GS(MSG_MOVE_NEW_START_CYL);
        ng.ng_GadgetID   = MVDLG_NEWCYL;
        ng.ng_Flags      = PLACETEXT_LEFT;
        cyl_gad = CreateGadgetA(STRING_KIND, gctx, &ng, st);
        if (!cyl_gad) goto mv_cleanup;
    }

    /* CHECKBOX_KIND: backup confirmation */
    {
        struct NewGadget ng;
        struct TagItem cbt[] = { { GTCB_Checked, FALSE }, { TAG_DONE, 0 } };
        static char chk_lbl[48];
        DP_SNPRINTF(chk_lbl, GS(MSG_MOVE_BACKUP_CHK_FMT), pi->drive_name);
        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;
        ng.ng_LeftEdge   = (UWORD)(bor_l + pad);
        ng.ng_TopEdge    = chk_y;
        ng.ng_Width      = inner_w;
        ng.ng_Height     = row_h;
        ng.ng_GadgetText = chk_lbl;
        ng.ng_GadgetID   = MVDLG_BACKUP;
        ng.ng_Flags      = PLACETEXT_RIGHT;
        chk_gad = CreateGadgetA(CHECKBOX_KIND, cyl_gad, &ng, cbt);
        if (!chk_gad) goto mv_cleanup;
    }

    /* BUTTON_KIND: Move Partition | Cancel */
    half = (UWORD)((inner_w - pad * 3) / 2);
    {
        struct NewGadget ng;
        struct TagItem bt[] = { { TAG_DONE, 0 } };
        struct Gadget *prev = chk_gad;
        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;
        ng.ng_TopEdge    = btn_y;
        ng.ng_Height     = row_h;
        ng.ng_Width      = half;
        ng.ng_Flags      = PLACETEXT_IN;

        ng.ng_LeftEdge   = (UWORD)(bor_l + pad);
        ng.ng_GadgetText = GS(MSG_MOVE_BTN_MOVE);
        ng.ng_GadgetID   = MVDLG_MOVE;
        btn_move = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
        if (!btn_move) goto mv_cleanup;
        prev = btn_move;

        ng.ng_LeftEdge   = (UWORD)(bor_l + pad * 2 + half);
        ng.ng_GadgetText = GS(MSG_CANCEL);
        ng.ng_GadgetID   = MVDLG_CANCEL;
        if (!CreateGadgetA(BUTTON_KIND, prev, &ng, bt)) goto mv_cleanup;
    }

    {
        struct TagItem wt[] = {
            { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
            { WA_Width,     win_w  },
            { WA_Height,    win_h  },
            { WA_Title,     (ULONG)GS(MSG_MOVE_DLG_TITLE) },
            { WA_Gadgets,   (ULONG)glist },
            { WA_PubScreen, (ULONG)scr   },
            { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW },
            { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                            WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH },
            { TAG_DONE, 0 }
        };
        dlg = OpenWindowTagList(NULL, wt);
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!dlg) goto mv_cleanup;

    GT_RefreshWindow(dlg, NULL);
    draw_move_warn_text(dlg, pi->drive_name, pi->low_cyl, pi->high_cyl);
    ActivateGadget(cyl_gad, dlg, NULL);

    running = TRUE;
    while (running) {
        struct IntuiMessage *imsg;
        WaitPort(dlg->UserPort);
        while ((imsg = GT_GetIMsg(dlg->UserPort)) != NULL) {
            ULONG  iclass = imsg->Class;
            UWORD  icode  = imsg->Code;
            struct Gadget *igad = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);

            switch (iclass) {
            case IDCMP_REFRESHWINDOW:
                GT_RefreshWindow(dlg, NULL);
                draw_move_warn_text(dlg, pi->drive_name,
                                    pi->low_cyl, pi->high_cyl);
                break;

            case IDCMP_CLOSEWINDOW:
                running = FALSE;
                break;

            case IDCMP_GADGETUP:
                switch (igad->GadgetID) {
                case MVDLG_BACKUP:
                    backup_ok = (BOOL)(icode != 0);
                    break;

                case MVDLG_CANCEL:
                    running = FALSE;
                    break;

                case MVDLG_MOVE: {
                    char can_err[128];
                    ULONG new_hi_tmp;

                    /* Read target cylinder from string gadget */
                    new_lo = 0;
                    if (cyl_gad) {
                        struct StringInfo *si =
                            (struct StringInfo *)cyl_gad->SpecialInfo;
                        if (si) new_lo = strtoul(si->Buffer, NULL, 10);
                    }

                    if (!backup_ok) {
                        struct EasyStruct es;
                        es.es_StructSize   = sizeof(struct EasyStruct);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)GS(MSG_MOVE_BTN_MOVE);
                        es.es_TextFormat   = (UBYTE *)GS(MSG_MOVE_TICK_BACKUP);
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                        EasyRequest(dlg, &es, NULL);
                        break;
                    }

                    if (!PART_CanMove(rdb, pi, new_lo, &new_hi_tmp, can_err)) {
                        struct EasyStruct es;
                        es.es_StructSize   = sizeof(struct EasyStruct);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)GS(MSG_MOVE_CANNOT_MOVE_TITLE);
                        es.es_TextFormat   = (UBYTE *)can_err;
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                        EasyRequest(dlg, &es, NULL);
                        break;
                    }

                    do_move = TRUE;
                    running = FALSE;
                    break;
                }
                } /* switch igad->GadgetID */
                break;
            } /* switch iclass */
        } /* while GT_GetIMsg */
    } /* while running */

    /* Close dialog before opening progress window */
    CloseWindow(dlg); dlg = NULL;
    FreeGadgets(glist); glist = NULL;
    if (vi) { FreeVisualInfo(vi); vi = NULL; }

    if (do_move) {
        struct Screen *pscr = LockPubScreen(NULL);
        if (pscr) {
            UWORD pfh  = pscr->Font ? pscr->Font->ta_YSize : 8;
            UWORD pbor = (UWORD)(pscr->WBorTop + pfh + 1);
            UWORD pw_w = 380;
            UWORD pw_h = (UWORD)(pbor + pscr->WBorBottom + pfh + 26);
            struct TagItem pt[] = {
                { WA_Left,      (ULONG)((pscr->Width  - pw_w) / 2) },
                { WA_Top,       (ULONG)((pscr->Height - pw_h) / 2) },
                { WA_Width,     pw_w  },
                { WA_Height,    pw_h  },
                { WA_Title,     (ULONG)GS(MSG_MOVE_PROG_TITLE) },
                { WA_PubScreen, (ULONG)pscr },
                { WA_Flags,     WFLG_DRAGBAR },
                { WA_IDCMP,     0 },
                { TAG_END, 0 }
            };
            struct Window *prog_win = OpenWindowTagList(NULL, pt);
            struct MoveProgUD prog_ud;
            BOOL moved;

            UnlockPubScreen(NULL, pscr);

            prog_ud.win = prog_win;
            moved = PART_Move(bd, rdb, pi, new_lo, err_buf,
                              move_progress_fn, &prog_ud);
            if (prog_win) CloseWindow(prog_win);

            if (moved) {
                BOOL wrote_rdb = RDB_Write(bd, rdb);
                struct EasyStruct ok_es;
                static char ok_msg[384];
                if (wrote_rdb) {
                    DP_SNPRINTF(ok_msg,
                        GS(MSG_MOVE_OK_RDB_WRITTEN_FMT),
                        pi->drive_name, err_buf);
                } else {
                    DP_SNPRINTF(ok_msg,
                        GS(MSG_MOVE_OK_RDB_FAILED_FMT),
                        pi->drive_name, err_buf);
                }
                ok_es.es_StructSize   = sizeof(ok_es);
                ok_es.es_Flags        = 0;
                ok_es.es_Title        = (UBYTE *)GS(MSG_MOVE_MOVED_TITLE);
                ok_es.es_TextFormat   = (UBYTE *)ok_msg;
                ok_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                EasyRequest(win, &ok_es, NULL);
                result = TRUE;
            } else {
                struct EasyStruct err_es;
                static char err_msg[384];
                DP_SNPRINTF(err_msg,
                    GS(MSG_MOVE_FAILED_FMT),
                    err_buf);
                err_es.es_StructSize   = sizeof(err_es);
                err_es.es_Flags        = 0;
                err_es.es_Title        = (UBYTE *)GS(MSG_MOVE_FAILED_TITLE);
                err_es.es_TextFormat   = (UBYTE *)err_msg;
                err_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                EasyRequest(win, &err_es, NULL);
            }
        }
    }

mv_cleanup:
    if (dlg)   CloseWindow(dlg);
    if (glist) FreeGadgets(glist);
    if (vi)    FreeVisualInfo(vi);
    if (scr)   UnlockPubScreen(NULL, scr);
    return result;
}

/* ------------------------------------------------------------------ */
/* Offer to grow an FFS/OFS filesystem after a partition was extended.  */
/* Called from all three "edit partition" code paths.                   */
/* ------------------------------------------------------------------ */
/* GParted-style grow progress: a top progress bar plus a retained,
   scrolling list of every step performed.  Earlier steps stay visible
   (prefixed "  "); the step in progress is marked "> ".  The full
   operation is narrated - unmount, the filesystem internals, RDB write
   and remount - so the user can always see what the tool is doing. */
#define GROW_LOG_MAX   24   /* steps retained in the scroll-back buffer   */
#define GROW_LOG_LINE  48   /* max chars stored per step line (incl NUL)  */
#define GROW_LOG_VIS   12   /* step rows visible in the window at once     */

/* Single shared line buffer - grows are strictly modal (one at a time),
   so a file-static store keeps GrowProgUD small enough for the stack. */
static char growlog_buf[GROW_LOG_MAX][GROW_LOG_LINE];

struct GrowProgUD {
    struct Window *win;
    UWORD step;     /* completed steps, drives the bar  */
    UWORD total;    /* expected step count for the bar  */
    UWORD count;    /* lines currently in growlog_buf   */
};

static void grow_str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    if (max <= 0) return;
    if (!src) src = "";
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void ffs_grow_progress(void *ud, const char *msg)
{
    struct GrowProgUD *pu = (struct GrowProgUD *)ud;
    struct Window *pw;
    struct RastPort *rp;
    WORD x1, y1, x2, y2;
    WORD bar_x, bar_y, bar_w, bar_h, filled_w;
    WORD line_h, list_y, row;
    UWORD i, start, len;

    if (!pu || !pu->win) return;
    pw = pu->win;

    /* Append this step to the retained log (scroll the buffer when full). */
    if (pu->count < GROW_LOG_MAX) {
        grow_str_copy(growlog_buf[pu->count], msg, GROW_LOG_LINE);
        pu->count++;
    } else {
        for (i = 1; i < GROW_LOG_MAX; i++)
            grow_str_copy(growlog_buf[i - 1], growlog_buf[i], GROW_LOG_LINE);
        grow_str_copy(growlog_buf[GROW_LOG_MAX - 1], msg, GROW_LOG_LINE);
    }
    if (pu->step < pu->total) pu->step++;

    rp = pw->RPort;
    x1 = pw->BorderLeft;
    y1 = pw->BorderTop;
    x2 = (WORD)(pw->Width  - 1 - pw->BorderRight);
    y2 = (WORD)(pw->Height - 1 - pw->BorderBottom);

    /* Clear interior */
    SetAPen(rp, 0);
    RectFill(rp, x1, y1, x2, y2);

    /* Progress bar */
    bar_x = (WORD)(x1 + 4);
    bar_y = (WORD)(y1 + 4);
    bar_w = (WORD)(x2 - x1 - 8);
    bar_h = 6;

    if (bar_w > 0) {
        filled_w = (pu->total > 0)
            ? (WORD)((ULONG)bar_w * pu->step / pu->total)
            : 0;

        /* Empty track (pen 2 = shine/highlight) */
        SetAPen(rp, 2);
        RectFill(rp, bar_x, bar_y,
                 (WORD)(bar_x + bar_w), (WORD)(bar_y + bar_h));

        /* Filled portion (pen 3 = fill pen) */
        if (filled_w > 0) {
            SetAPen(rp, 3);
            RectFill(rp, bar_x, bar_y,
                     (WORD)(bar_x + filled_w), (WORD)(bar_y + bar_h));
        }

        /* 1-pixel border (pen 1 = text/foreground) */
        SetAPen(rp, 1);
        Move(rp, bar_x, bar_y);
        Draw(rp, (WORD)(bar_x + bar_w), bar_y);
        Draw(rp, (WORD)(bar_x + bar_w), (WORD)(bar_y + bar_h));
        Draw(rp, bar_x, (WORD)(bar_y + bar_h));
        Draw(rp, bar_x, bar_y);
    }

    /* Retained step list below the bar.  Show the last GROW_LOG_VIS lines;
       the layer clips any text wider than the window so long lines are safe. */
    line_h = (WORD)(rp->TxHeight > 0 ? rp->TxHeight : 8);
    list_y = (WORD)(bar_y + bar_h + 4);
    start  = (pu->count > GROW_LOG_VIS) ? (UWORD)(pu->count - GROW_LOG_VIS) : 0;
    row    = 0;
    SetAPen(rp, 1);
    for (i = start; i < pu->count; i++, row++) {
        char rowbuf[GROW_LOG_LINE + 2];
        int  p = 0, k = 0;
        WORD ty = (WORD)(list_y + row * line_h + rp->TxBaseline);
        /* "> " marks the current step, "  " marks completed ones. */
        rowbuf[p++] = (i + 1 == pu->count) ? '>' : ' ';
        rowbuf[p++] = ' ';
        while (growlog_buf[i][k] && p < (int)sizeof(rowbuf) - 1)
            rowbuf[p++] = growlog_buf[i][k++];
        rowbuf[p] = '\0';
        Move(rp, (WORD)(x1 + 6), ty);
        for (len = 0; rowbuf[len]; len++) {}
        Text(rp, (STRPTR)rowbuf, (WORD)len);
    }
}

/* Open a grow-progress window sized to hold the bar + GROW_LOG_VIS step
   rows, and initialise the progress state.  Returns NULL on failure (the
   grow still runs; ffs_grow_progress simply becomes a no-op). */
static struct Window *grow_open_progress(struct Window *win, CONST_STRPTR title,
                                         struct GrowProgUD *pu, UWORD total)
{
    struct Screen *scr = win->WScreen;
    UWORD font_h = scr->Font ? scr->Font->ta_YSize : 8;
    UWORD bor_t  = (UWORD)(scr->WBorTop + font_h + 1);
    UWORD pw_w   = 420;
    UWORD pw_h   = (UWORD)(bor_t + scr->WBorBottom
                           + 4 + 6 + 4 + GROW_LOG_VIS * font_h + 4);
    struct TagItem prog_tags[] = {
        { WA_Left,      0 },
        { WA_Top,       0 },
        { WA_Width,     0 },
        { WA_Height,    0 },
        { WA_Title,     0 },
        { WA_PubScreen, 0 },
        { WA_Flags,     (ULONG)WFLG_DRAGBAR },
        { WA_IDCMP,     0 },
        { TAG_END,      0 }
    };

    if (pw_w > (UWORD)(scr->Width - 8)) pw_w = (UWORD)(scr->Width - 8);
    prog_tags[0].ti_Data = (ULONG)((scr->Width  - pw_w) / 2);
    prog_tags[1].ti_Data = (ULONG)((scr->Height - pw_h) / 2);
    prog_tags[2].ti_Data = (ULONG)pw_w;
    prog_tags[3].ti_Data = (ULONG)pw_h;
    prog_tags[4].ti_Data = (ULONG)title;
    prog_tags[5].ti_Data = (ULONG)scr;

    pu->win   = OpenWindowTagList(NULL, prog_tags);
    pu->step  = 0;
    pu->total = total;
    pu->count = 0;
    return pu->win;
}

/* Narrate one bracket step (unmount/remount/RDB/etc.) into the progress
   log.  fmt may contain a single %s for the drive/volume name. */
static void grow_say(struct GrowProgUD *pu, CONST_STRPTR fmt, const char *arg)
{
    char buf[GROW_LOG_LINE];
    if (arg) DP_SNPRINTF(buf, fmt, arg);
    else     grow_str_copy(buf, fmt, sizeof(buf));
    ffs_grow_progress(pu, buf);
}

int offer_ffs_grow(struct Window *win, struct BlockDev *bd,
                          const struct RDBInfo *rdb, struct PartInfo *pi,
                          ULONG old_hi)
{
    struct EasyStruct es;
    char errbuf[256];  /* must hold FFS_GrowPartition diagnostic - keep in sync */
    char umerr[80];    /* why unmount failed (in-use), for diagnostics          */
    char rmerr[80];    /* why remount failed, for diagnostics                   */
    BOOL can_remount;
    BOOL no_unmount = FALSE;  /* TRUE once the user opts to grow a busy volume  */
    int  rc = GROW_NEED_REBOOT;
    umerr[0] = '\0';
    rmerr[0] = '\0';

    if (pi->high_cyl <= old_hi) return GROW_NONE;
    if (!FFS_IsSupportedType(pi->dos_type)) return GROW_NONE;
    if (!bd) return GROW_NONE;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROW_TITLE);
    es.es_TextFormat   = (UBYTE *)GS(MSG_MOVE_GROW_FFS_BODY_FMT);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_MOVE_GROW_GADGETS);

    if (EasyRequest(win, &es, NULL, pi->drive_name) != 1) return GROW_NONE;

    {
        struct GrowProgUD prog_ud;
        struct Window *prog_win;
        char mnt[40];
        BOOL result;

        /* Open the step-log window up front so the unmount - which may itself
           pop an OS "insert volume" requester - is narrated and never looks
           like an unexplained prompt.  Total counts: unmount + 13 FFS steps
           + remount + online + done. */
        prog_win = grow_open_progress(win, GS(MSG_MOVE_GROW_FFS_PROG_TITLE),
                                      &prog_ud, 17);

        /* The grow writes filesystem blocks directly, so it MUST run offline:
           we unmount the volume first, grow it, then remount with the new
           geometry (no reboot).  We never grow a live volume - if it's in use
           we can't unmount, so we refuse and leave everything untouched
           (growing under a live handler corrupts the volume). */
        grow_say(&prog_ud, GS(MSG_GROW_PROG_UNMOUNTING_FMT), pi->drive_name);
        can_remount = UnmountDevice(pi->drive_name, umerr, sizeof(umerr));
        if (!can_remount) {
            /* The volume can't be unmounted (boot partition, or open files).
               Offer to grow it in place instead: the FFS grow still inhibits
               its writes, then we lock the volume and require a reboot.  If
               the user declines, leave everything untouched. */
            struct EasyStruct offer_es;
            static char offer_msg[256];
            DP_SNPRINTF(offer_msg,
                    GS(MSG_MOVE_BUSY_OFFER_FMT),
                    pi->drive_name, umerr[0] ? umerr : GS(MSG_MOVE_IN_USE));
            offer_es.es_StructSize   = sizeof(offer_es);
            offer_es.es_Flags        = 0;
            offer_es.es_Title        = (UBYTE *)GS(MSG_MOVE_BUSY_TITLE);
            offer_es.es_TextFormat   = (UBYTE *)offer_msg;
            offer_es.es_GadgetFormat = (UBYTE *)GS(MSG_MOVE_BUSY_OFFER_GADGETS);
            if (EasyRequest(win, &offer_es, NULL) != 1) {
                if (prog_win) CloseWindow(prog_win);
                pi->high_cyl = old_hi;             /* undo the size change */
                return GROW_ABORTED;
            }
            no_unmount = TRUE;
        }

        result = FFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                   ffs_grow_progress, &prog_ud);
        if (result) {
            struct EasyStruct ok_es;
            static char ok_msg[512];

            if (no_unmount) {
                /* Grown in place. The live handler still holds the OLD
                   DosEnvec/root, so re-inhibit to lock the volume (the FFS
                   grow released its own inhibit) and persist the RDB now -
                   the new size must be on disk before the reboot, or the
                   relocated root and the on-disk geometry would disagree. */
                BOOL wrote_rdb;
                char inh[40];
                DP_SNPRINTF(inh, "%s:", pi->drive_name);
                Inhibit((STRPTR)inh, DOSTRUE);
                grow_say(&prog_ud, GS(MSG_GROW_PROG_WRITING_RDB), NULL);
                wrote_rdb = RDB_Write(bd, (struct RDBInfo *)rdb);
                grow_say(&prog_ud, GS(MSG_GROW_PROG_DONE), NULL);
                if (prog_win) CloseWindow(prog_win);
                rc = GROW_NEED_REBOOT;
                DP_SNPRINTF(ok_msg,
                        wrote_rdb ? GS(MSG_MOVE_FFS_NOUNMOUNT_OK_FMT)
                                  : GS(MSG_MOVE_FFS_NOUNMOUNT_RDBFAIL_FMT),
                        pi->drive_name, errbuf);
            } else {
            grow_say(&prog_ud, GS(MSG_GROW_PROG_REMOUNTING_FMT), pi->drive_name);
            if (MountPartition(bd, pi, mnt, rmerr, sizeof(rmerr))) {
                /* Grown and remounted with the new geometry - no reboot. */
                /* Bring the volume online now so a lock orphaned during the
                   grow (e.g. Workbench's icon lock) revalidates silently
                   rather than popping an "insert volume" requester. */
                grow_say(&prog_ud, GS(MSG_GROW_PROG_ONLINE), NULL);
                MaterializeVolume(mnt);
                grow_say(&prog_ud, GS(MSG_GROW_PROG_DONE), NULL);
                rc = GROW_REMOUNTED;
                DP_SNPRINTF(ok_msg,
                        GS(MSG_MOVE_FFS_REMOUNTED_FMT),
                        pi->drive_name, mnt, errbuf);
            } else {
                /* Grow worked but the volume couldn't be remounted live. */
                grow_say(&prog_ud, GS(MSG_GROW_PROG_DONE), NULL);
                rc = GROW_NEED_REBOOT;
                DP_SNPRINTF(ok_msg,
                        GS(MSG_MOVE_FFS_REMOUNT_FAIL_FMT),
                        pi->drive_name, rmerr[0] ? rmerr : GS(MSG_MOVE_QMARK), errbuf);
            }
            if (prog_win) CloseWindow(prog_win);
            }
            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROWN_TITLE);
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &ok_es, NULL);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            /* Grow failed - restore the original size and remount so the user
               keeps a working volume.  Under no_unmount we never unmounted
               (the FFS grow already released its inhibit), so don't remount. */
            pi->high_cyl = old_hi;
            if (!no_unmount) {
                grow_say(&prog_ud, GS(MSG_GROW_PROG_REMOUNTING_FMT), pi->drive_name);
                MountPartition(bd, pi, mnt, rmerr, sizeof(rmerr));
            }
            if (prog_win) CloseWindow(prog_win);
            rc = GROW_ABORTED;
            DP_SNPRINTF(full_msg,
                    GS(MSG_MOVE_FFS_FAIL_RESTORED_FMT),
                    errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROW_FAIL_TITLE);
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &err_es, NULL);
        }
    }
    return rc;
}

int offer_pfs_grow(struct Window *win, struct BlockDev *bd,
                          const struct RDBInfo *rdb, struct PartInfo *pi,
                          ULONG old_hi)
{
    struct EasyStruct es;
    char errbuf[256];

    /* PFS grow keeps its own Inhibit/RDB-write handling and still requires a
       reboot; the unmount/remount no-reboot path is FFS-only for now. */
    if (pi->high_cyl <= old_hi) return GROW_NONE;
    if (!PFS_IsSupportedType(pi->dos_type)) return GROW_NONE;
    if (!bd) return GROW_NONE;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROW_TITLE);
    es.es_TextFormat   = (UBYTE *)GS(MSG_MOVE_GROW_PFS_BODY_FMT);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_MOVE_GROW_GADGETS);

    if (EasyRequest(win, &es, NULL, pi->drive_name) == 1) {
        struct GrowProgUD prog_ud;
        struct Window *prog_win =
            grow_open_progress(win, GS(MSG_MOVE_GROW_PFS_PROG_TITLE),
                               &prog_ud, 6);  /* PFS_PROGRESS call count */

        BOOL result = PFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                        ffs_grow_progress, &prog_ud);
        if (result) {
            BOOL wrote_rdb;
            struct EasyStruct ok_es;
            static char ok_msg[512];
            /* PFS_GrowPartition writes the rootblock but NOT the RDB.  Persist
               the new partition geometry now (as the CLI and SFS paths do):
               if the user reboots before a manual Write, the grown rootblock
               would disagree with the old on-disk high_cyl - an inconsistent
               volume, exactly the unsafe state to avoid. */
            grow_say(&prog_ud, GS(MSG_GROW_PROG_WRITING_RDB), NULL);
            wrote_rdb = RDB_Write(bd, (struct RDBInfo *)rdb);
            grow_say(&prog_ud, GS(MSG_GROW_PROG_DONE), NULL);
            if (prog_win) CloseWindow(prog_win);
            DP_SNPRINTF(ok_msg,
                    wrote_rdb ? GS(MSG_MOVE_PFS_OK_RDB_WRITTEN_FMT)
                              : GS(MSG_MOVE_PFS_OK_RDB_FAILED_FMT),
                    pi->drive_name, errbuf);
            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROWN_TITLE);
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &ok_es, NULL);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            /* Grow failed - restore the displayed size so the partition view
               doesn't keep showing a phantom grown partition that was never
               written to disk (which would mislead the user). */
            pi->high_cyl = old_hi;
            if (prog_win) CloseWindow(prog_win);
            DP_SNPRINTF(full_msg, GS(MSG_MOVE_PFS_FAIL_FMT), errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROW_FAIL_TITLE);
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &err_es, NULL);
            return GROW_ABORTED;
        }
        return GROW_NEED_REBOOT;
    }
    return GROW_NONE;
}

int offer_sfs_grow(struct Window *win, struct BlockDev *bd,
                           struct RDBInfo *rdb, struct PartInfo *pi,
                           ULONG old_hi)
{
    struct EasyStruct es;
    char errbuf[256];

    /* SFS grow auto-writes the RDB and leaves the volume inhibited; it still
       requires a reboot.  No-reboot remount is FFS-only for now. */
    if (pi->high_cyl <= old_hi) return GROW_NONE;
    if (!SFS_IsSupportedType(pi->dos_type)) return GROW_NONE;
    if (!bd) return GROW_NONE;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROW_TITLE);
    es.es_TextFormat   = (UBYTE *)GS(MSG_MOVE_GROW_SFS_BODY_FMT);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_MOVE_GROW_GADGETS);

    if (EasyRequest(win, &es, NULL, pi->drive_name) == 1) {
        struct GrowProgUD prog_ud;
        /* Total: 14 SFS steps + RDB write + done. */
        struct Window *prog_win =
            grow_open_progress(win, GS(MSG_MOVE_GROW_SFS_PROG_TITLE),
                               &prog_ud, 16);

        BOOL result = SFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                        ffs_grow_progress, &prog_ud);
        if (result) {
            BOOL wrote_rdb;
            struct EasyStruct ok_es;
            static char ok_msg[512];
            grow_say(&prog_ud, GS(MSG_GROW_PROG_WRITING_RDB), NULL);
            wrote_rdb = RDB_Write(bd, rdb);
            grow_say(&prog_ud, GS(MSG_GROW_PROG_DONE), NULL);
            if (prog_win) CloseWindow(prog_win);
            if (wrote_rdb) {
                DP_SNPRINTF(ok_msg,
                        GS(MSG_MOVE_SFS_OK_RDB_WRITTEN_FMT),
                        pi->drive_name, pi->drive_name, errbuf);
            } else {
                DP_SNPRINTF(ok_msg,
                        GS(MSG_MOVE_SFS_OK_RDB_FAILED_FMT),
                        pi->drive_name, pi->drive_name, errbuf);
            }
            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROWN_TITLE);
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &ok_es, NULL);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            /* Grow failed - restore the displayed size so the partition view
               doesn't keep showing a phantom grown partition. */
            pi->high_cyl = old_hi;
            if (prog_win) CloseWindow(prog_win);
            DP_SNPRINTF(full_msg, GS(MSG_MOVE_SFS_FAIL_FMT), errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)GS(MSG_MOVE_GROW_FAIL_TITLE);
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &err_es, NULL);
            return GROW_ABORTED;
        }
        return GROW_NEED_REBOOT;
    }
    return GROW_NONE;
}


/* ================================================================== */
/* offer_shrink - GUI shrink flow, shared by the partition-map drag    */
/* and the Edit dialog.  Called after the user has set a SMALLER       */
/* high_cyl (pi->high_cyl = requested new value, old_lo/old_hi = the   */
/* values before the edit).                                            */
/*                                                                     */
/* Returns GROW_NONE when this is not a plain supported shrink (grown, */
/* low_cyl moved, or not FFS/PFS3/SFS) - the caller falls back to its  */
/* legacy handling.  On GROW_ABORTED pi->high_cyl has been restored.   */
/*                                                                     */
/* Flow per the user's spec: scan first; if the requested size is      */
/* below the data floor, a requester states the minimum ("XX MB is     */
/* minimum") with yes/no to shrink to that minimum instead.  On        */
/* success the RDB is written IMMEDIATELY (all filesystems - unlike    */
/* the FFS grow's deferred write): the GUI shrink is one committed     */
/* operation, with no window where a discard could strand a shrunk     */
/* filesystem behind a stale on-disk envelope.                         */
/* ================================================================== */
int offer_shrink(struct Window *win, struct BlockDev *bd,
                 struct RDBInfo *rdb, struct PartInfo *pi,
                 ULONG old_lo, ULONG old_hi)
{
    struct EasyStruct es;
    struct ShrinkReport rep;
    char  errbuf[256], umerr[80], rmerr[80], mnt[40];
    char  sz1[20], sz2[20];
    static char body[320];
    int   fskind;                 /* 1=FFS 2=PFS 3=SFS */
    BOOL  no_unmount = FALSE, can_remount;
    ULONG requested, new_hi, ncyl_old, bpc_fs, min_cyls, min_high;
    ULONG heads, sectors, blks_cyl;
    int   rc = GROW_NEED_REBOOT;

    if (pi->high_cyl >= old_hi) return GROW_NONE;
    if (pi->low_cyl != old_lo)  return GROW_NONE;   /* moved start: legacy */
    if (!bd) return GROW_NONE;
    if      (FFS_IsSupportedType(pi->dos_type)) fskind = 1;
    else if (PFS_IsSupportedType(pi->dos_type)) fskind = 2;
    else if (SFS_IsSupportedType(pi->dos_type)) fskind = 3;
    else return GROW_NONE;

    umerr[0] = rmerr[0] = '\0';
    heads    = pi->heads   > 0 ? pi->heads   : rdb->heads;
    sectors  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    blks_cyl = heads * sectors;
    if (blks_cyl == 0) return GROW_NONE;

    /* The scanners size the volume from pi->high_cyl - restore the OLD
       value for the scan, keep the user's request aside. */
    requested    = pi->high_cyl;
    pi->high_cyl = old_hi;
    ncyl_old     = old_hi - pi->low_cyl + 1;

    memset(&rep, 0, sizeof(rep)); errbuf[0] = '\0';
    {
        BOOL sok = (fskind == 2) ? PFS_ShrinkInfo(bd, rdb, pi, &rep, errbuf)
                 : (fskind == 3) ? SFS_ShrinkInfo(bd, rdb, pi, &rep, errbuf)
                 :                 FFS_ShrinkInfo(bd, rdb, pi, &rep, errbuf);
        if (!sok) {
            DP_SNPRINTF(body, GS(MSG_PV_SHR_SCANFAIL_FMT),
                        pi->drive_name, errbuf);
            es.es_StructSize   = sizeof(es);
            es.es_Flags        = 0;
            es.es_Title        = (UBYTE *)GS(MSG_PV_SHR_TITLE);
            es.es_TextFormat   = (UBYTE *)body;
            es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &es, NULL);
            return GROW_ABORTED;              /* pi->high_cyl already old */
        }
    }
    bpc_fs = (ncyl_old > 0) ? rep.total_blocks / ncyl_old : 0;
    if (bpc_fs == 0) { return GROW_ABORTED; }
    min_cyls = (rep.min_blocks + bpc_fs - 1) / bpc_fs;
    if (min_cyls == 0) min_cyls = 1;
    min_high = pi->low_cyl + min_cyls - 1;

    if (min_high >= old_hi) {
        DP_SNPRINTF(body, GS(MSG_PV_SHR_ATMIN_FMT), pi->drive_name);
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_PV_SHR_TITLE);
        es.es_TextFormat   = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
        return GROW_ABORTED;
    }

    if (requested < min_high) {
        /* Below the floor: state the minimum, offer it as the size. */
        FormatSize((UQUAD)min_cyls * blks_cyl * 512UL, sz1);
        DP_SNPRINTF(body, GS(MSG_PV_SHR_MIN_FMT),
                    pi->drive_name, sz1, (unsigned long)min_high);
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_PV_SHR_TITLE);
        es.es_TextFormat   = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_SHR_MIN_GADGETS);
        if (EasyRequest(win, &es, NULL) != 1)
            return GROW_ABORTED;
        new_hi = min_high;
    } else {
        new_hi = requested;
        FormatSize((UQUAD)(new_hi - pi->low_cyl + 1) * blks_cyl * 512UL, sz1);
        FormatSize((UQUAD)(old_hi - new_hi) * blks_cyl * 512UL, sz2);
        DP_SNPRINTF(body, GS(MSG_PV_SHR_CONFIRM_FMT),
                    pi->drive_name, sz1, sz2);
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_PV_SHR_TITLE);
        es.es_TextFormat   = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_SHR_GADGETS);
        if (EasyRequest(win, &es, NULL) != 1)
            return GROW_ABORTED;
    }

    pi->high_cyl = new_hi;
    {
        struct GrowProgUD prog_ud;
        struct Window *prog_win;
        BOOL result;

        prog_win = grow_open_progress(win, GS(MSG_PV_SHR_PROG_TITLE),
                                      &prog_ud, 16);

        /* Offline like the CLI: unmount for every filesystem; a busy
           volume gets the same shrink-in-place offer as the grow. */
        grow_say(&prog_ud, GS(MSG_GROW_PROG_UNMOUNTING_FMT), pi->drive_name);
        can_remount = UnmountDevice(pi->drive_name, umerr, sizeof(umerr));
        if (!can_remount) {
            struct EasyStruct offer_es;
            static char offer_msg[256];
            DP_SNPRINTF(offer_msg, GS(MSG_MOVE_BUSY_OFFER_FMT),
                    pi->drive_name, umerr[0] ? umerr : GS(MSG_MOVE_IN_USE));
            offer_es.es_StructSize   = sizeof(offer_es);
            offer_es.es_Flags        = 0;
            offer_es.es_Title        = (UBYTE *)GS(MSG_MOVE_BUSY_TITLE);
            offer_es.es_TextFormat   = (UBYTE *)offer_msg;
            offer_es.es_GadgetFormat = (UBYTE *)GS(MSG_MOVE_BUSY_OFFER_GADGETS);
            if (EasyRequest(win, &offer_es, NULL) != 1) {
                if (prog_win) CloseWindow(prog_win);
                pi->high_cyl = old_hi;
                return GROW_ABORTED;
            }
            no_unmount = TRUE;
        }

        result = (fskind == 2)
                 ? PFS_ShrinkPartition(bd, rdb, pi, old_hi, errbuf,
                                       ffs_grow_progress, &prog_ud)
                 : (fskind == 3)
                 ? SFS_ShrinkPartition(bd, rdb, pi, old_hi, errbuf,
                                       ffs_grow_progress, &prog_ud)
                 : FFS_ShrinkPartition(bd, rdb, pi, old_hi, errbuf,
                                       ffs_grow_progress, &prog_ud);

        if (result) {
            struct EasyStruct ok_es;
            static char ok_msg[512];
            BOOL wrote_rdb;

            /* Commit the new envelope NOW - the engines' reversibility
               window closes here, exactly like the CLI. */
            if (no_unmount) {
                char inh[40];
                DP_SNPRINTF(inh, "%s:", pi->drive_name);
                Inhibit((STRPTR)inh, DOSTRUE);
            }
            grow_say(&prog_ud, GS(MSG_GROW_PROG_WRITING_RDB), NULL);
            wrote_rdb = RDB_Write(bd, rdb);

            FormatSize((UQUAD)(new_hi - pi->low_cyl + 1) * blks_cyl * 512UL,
                       sz1);
            if (!wrote_rdb) {
                rc = GROW_NEED_REBOOT;
                DP_SNPRINTF(ok_msg, GS(MSG_PV_SHR_OK_RDBFAIL_FMT),
                            pi->drive_name);
            } else if (fskind == 1 && !no_unmount) {
                grow_say(&prog_ud, GS(MSG_GROW_PROG_REMOUNTING_FMT),
                         pi->drive_name);
                if (MountPartition(bd, pi, mnt, rmerr, sizeof(rmerr))) {
                    grow_say(&prog_ud, GS(MSG_GROW_PROG_ONLINE), NULL);
                    MaterializeVolume(mnt);
                    rc = GROW_REMOUNTED;
                    DP_SNPRINTF(ok_msg, GS(MSG_PV_SHR_OK_REMOUNT_FMT),
                                pi->drive_name, sz1);
                } else {
                    rc = GROW_NEED_REBOOT;
                    DP_SNPRINTF(ok_msg, GS(MSG_PV_SHR_OK_REBOOT_FMT),
                                pi->drive_name, sz1);
                }
            } else {
                rc = GROW_NEED_REBOOT;
                DP_SNPRINTF(ok_msg, GS(MSG_PV_SHR_OK_REBOOT_FMT),
                            pi->drive_name, sz1);
            }
            grow_say(&prog_ud, GS(MSG_GROW_PROG_DONE), NULL);
            if (prog_win) CloseWindow(prog_win);

            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)GS(MSG_PV_SHR_TITLE);
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &ok_es, NULL);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            /* Engine failed and rolled back - restore the size and (if we
               unmounted) remount with the original envelope. */
            pi->high_cyl = old_hi;
            if (!no_unmount) {
                grow_say(&prog_ud, GS(MSG_GROW_PROG_REMOUNTING_FMT),
                         pi->drive_name);
                MountPartition(bd, pi, mnt, rmerr, sizeof(rmerr));
            }
            if (prog_win) CloseWindow(prog_win);
            rc = GROW_ABORTED;
            DP_SNPRINTF(full_msg, GS(MSG_PV_SHR_FAIL_FMT), errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)GS(MSG_PV_SHR_TITLE);
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            EasyRequest(win, &err_es, NULL);
        }
    }
    return rc;
}
