/*
 * partview_move.c — Partition move and filesystem grow operations.
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
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "devices.h"
#include "version.h"
#include "ffsresize.h"
#include "pfsresize.h"
#include "sfsresize.h"
#include "partmove.h"
#include "partview_internal.h"

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
    es.es_Title        = (UBYTE *)"Check FFS Root";
    es.es_GadgetFormat = (UBYTE *)"OK";

    if (!bd) {
        es.es_TextFormat = (UBYTE *)"No device open.";
        EasyRequest(win, &es, NULL);
        return;
    }
    if (!rdb || sel < 0 || (ULONG)sel >= rdb->num_parts) {
        es.es_TextFormat = (UBYTE *)"No partition selected.\nSelect a partition from the list first.";
        EasyRequest(win, &es, NULL);
        return;
    }

    const struct PartInfo *pi = &rdb->parts[sel];
    ULONG heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;

    if (heads == 0 || sectors == 0) {
        es.es_TextFormat = (UBYTE *)"Cannot check: partition geometry\nhas heads=0 or sectors=0.";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (ULONG *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        es.es_TextFormat = (UBYTE *)"Out of memory.";
        EasyRequest(win, &es, NULL);
        return;
    }

    ULONG part_abs   = pi->low_cyl * heads * sectors;
    ULONG num_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;
    ULONG root       = num_blocks / 2;
    ULONG root_abs   = part_abs + root;

    if (!BlockDev_ReadBlock(bd, root_abs, buf)) {
        sprintf(msg,
                "Partition %s\n"
                "part_abs=%lu  num_blks=%lu\n"
                "Expected root: rel=%lu abs=%lu\n\n"
                "READ FAILED at abs %lu",
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
    /* FFS does NOT validate own_key — confirmed: KS 3.1 accepts own_key=0 on
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

    sprintf(msg,
            "Partition %s  heads=%lu secs=%lu\n"
            "Expected root: rel=%lu abs=%lu\n"
            "Boot bb[2]=%lu  (0=use num_blks/2)\n\n"
            "L[0]  type    =0x%lX  (%s)\n"
            "L[1]  own_key =%lu  (expect %lu)%s\n"
            "L[4]  disk_sz =%lu  (expect %lu)%s\n"
            "L[5]  chksum  ok=%s\n"
            "L[78] bm_flag =0x%lX  (%s)\n"
            "L[127]sec_type=0x%lX  (%s)\n"
            "L[79] bm[0]   =%lu\n"
            "L[104]bm_ext  =%lu\n\n"
            "%s",
            pi->drive_name, (unsigned long)heads, (unsigned long)sectors,
            (unsigned long)root, (unsigned long)root_abs,
            (unsigned long)bb2,
            (unsigned long)buf[0],  type_ok ? "ok" : "WRONG,expect 2",
            (unsigned long)buf[1],  (unsigned long)root,
                own_ok ? "" : " (FFS ignores)",
            (unsigned long)disk_size, (unsigned long)num_blocks,
                dsz_ok ? "" : " MISMATCH",
            cs_ok ? "YES" : "NO",
            (unsigned long)buf[78], bm_valid ? "valid" : "INVALID",
            (unsigned long)buf[127], sec_ok ? "ok" : "WRONG,expect 1",
            (unsigned long)buf[79],
            (unsigned long)buf[104],
            looks_ok ? "==> ROOT IS VALID" : "==> ROOT IS INVALID");

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

    WTEXT("WARNING:  MOVING A PARTITION COPIES ALL DATA ON DISK.")
    y = (WORD)(y + lh / 2);

    sprintf(line, "Partition %s (cyl %lu-%lu) will be physically",
            pname, (unsigned long)lo, (unsigned long)hi);
    WTEXT(line)
    WTEXT("copied to the cylinder you enter above.")

    y = (WORD)(y + lh / 2);

    WTEXT("POWER LOSS OR CRASH DURING THIS PROCESS WILL")
    WTEXT("PERMANENTLY DESTROY YOUR DATA.  No rollback.")

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

    sprintf(cyl_str, "%lu",
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

    /* 5 warning text lines + 2 half-gaps */
    warn_h  = (UWORD)((5 + 1) * (font_h + 2) + pad * 2 + (font_h + 2));

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
        ng.ng_GadgetText = "New start cyl";
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
        sprintf(chk_lbl, "I have a current backup of %s", pi->drive_name);
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
        ng.ng_GadgetText = "Move Partition";
        ng.ng_GadgetID   = MVDLG_MOVE;
        btn_move = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
        if (!btn_move) goto mv_cleanup;
        prev = btn_move;

        ng.ng_LeftEdge   = (UWORD)(bor_l + pad * 2 + half);
        ng.ng_GadgetText = "Cancel";
        ng.ng_GadgetID   = MVDLG_CANCEL;
        if (!CreateGadgetA(BUTTON_KIND, prev, &ng, bt)) goto mv_cleanup;
    }

    {
        struct TagItem wt[] = {
            { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
            { WA_Width,     win_w  },
            { WA_Height,    win_h  },
            { WA_Title,     (ULONG)"WARNING: Move Partition - Data Hazard" },
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
                        struct EasyStruct es = {
                            sizeof(struct EasyStruct), 0,
                            "Move Partition",
                            "Tick the backup confirmation checkbox\n"
                            "before proceeding.",
                            "OK"
                        };
                        EasyRequest(dlg, &es, NULL);
                        break;
                    }

                    if (!PART_CanMove(rdb, pi, new_lo, &new_hi_tmp, can_err)) {
                        struct EasyStruct es = {
                            sizeof(struct EasyStruct), 0,
                            "Cannot Move Partition",
                            can_err,
                            "OK"
                        };
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
                { WA_Title,     (ULONG)"Moving Partition..." },
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
                    sprintf(ok_msg,
                        "Partition %s moved successfully.\n"
                        "RDB written automatically.\n"
                        "Reboot to use the moved partition.\n\n"
                        "%s",
                        pi->drive_name, err_buf);
                } else {
                    sprintf(ok_msg,
                        "Partition %s moved successfully.\n"
                        "WARNING: RDB write FAILED.\n"
                        "Click Write to save the RDB.\n\n"
                        "%s",
                        pi->drive_name, err_buf);
                }
                ok_es.es_StructSize   = sizeof(ok_es);
                ok_es.es_Flags        = 0;
                ok_es.es_Title        = (UBYTE *)"Partition Moved";
                ok_es.es_TextFormat   = (UBYTE *)ok_msg;
                ok_es.es_GadgetFormat = (UBYTE *)"OK";
                EasyRequest(win, &ok_es, NULL);
                result = TRUE;
            } else {
                struct EasyStruct err_es;
                static char err_msg[384];
                sprintf(err_msg,
                    "Move FAILED:\n%s\n\n"
                    "Data may be partially written.\n"
                    "Restore from backup before rebooting.",
                    err_buf);
                err_es.es_StructSize   = sizeof(err_es);
                err_es.es_Flags        = 0;
                err_es.es_Title        = (UBYTE *)"Move Failed";
                err_es.es_TextFormat   = (UBYTE *)err_msg;
                err_es.es_GadgetFormat = (UBYTE *)"OK";
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
struct GrowProgUD {
    struct Window *win;
    UWORD step;
    UWORD total;
};

static void ffs_grow_progress(void *ud, const char *msg)
{
    struct GrowProgUD *pu = (struct GrowProgUD *)ud;
    struct Window *pw;
    struct RastPort *rp;
    WORD x1, y1, x2, y2;
    WORD bar_x, bar_y, bar_w, bar_h, filled_w;
    UWORD len;

    if (!pu || !pu->win) return;
    pw = pu->win;
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

    /* Message text below the bar */
    SetAPen(rp, 1);
    Move(rp, (WORD)(x1 + 6),
         (WORD)(bar_y + bar_h + 4 + rp->TxBaseline));
    for (len = 0; msg[len]; len++) {}
    Text(rp, (STRPTR)msg, (WORD)len);
}

void offer_ffs_grow(struct Window *win, struct BlockDev *bd,
                           const struct RDBInfo *rdb, struct PartInfo *pi,
                           ULONG old_hi)
{
    struct EasyStruct es;
    char errbuf[256];  /* must hold FFS_GrowPartition diagnostic — keep in sync */

    if (pi->high_cyl <= old_hi) return;
    if (!FFS_IsSupportedType(pi->dos_type)) return;
    if (!bd) return;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"EXPERIMENTAL: Grow Filesystem";
    es.es_TextFormat   = (UBYTE *)
        "This will write FFS bitmap blocks directly to disk.\n"
        "This feature is EXPERIMENTAL and may corrupt data.\n"
        "Always have a backup before proceeding.\n\n"
        "Grow FFS filesystem on partition %s?";
    es.es_GadgetFormat = (UBYTE *)"Grow Filesystem|Skip";

    if (EasyRequest(win, &es, NULL, pi->drive_name) == 1) {
        struct Screen *scr = win->WScreen;
        UWORD font_h = scr->Font ? scr->Font->ta_YSize : 8;
        UWORD bor_t  = (UWORD)(scr->WBorTop + font_h + 1);
        UWORD pw_w   = 360;
        UWORD pw_h   = (UWORD)(bor_t + scr->WBorBottom + font_h + 26);
        struct TagItem prog_tags[] = {
            { WA_Left,      (ULONG)((scr->Width  - pw_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - pw_h) / 2) },
            { WA_Width,     (ULONG)pw_w  },
            { WA_Height,    (ULONG)pw_h  },
            { WA_Title,     (ULONG)"Growing FFS Filesystem..." },
            { WA_PubScreen, (ULONG)scr   },
            { WA_Flags,     (ULONG)WFLG_DRAGBAR },
            { WA_IDCMP,     0             },
            { TAG_END,      0             }
        };
        struct Window *prog_win = OpenWindowTagList(NULL, prog_tags);
        struct GrowProgUD prog_ud;
        prog_ud.win   = prog_win;
        prog_ud.step  = 0;
        prog_ud.total = 13;  /* FFS_PROGRESS call count */

        BOOL result = FFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                        ffs_grow_progress, &prog_ud);
        if (prog_win) CloseWindow(prog_win);
        if (result) {
            struct EasyStruct ok_es;
            static char ok_msg[512];
            sprintf(ok_msg,
                    "FFS filesystem on %%s grown successfully.\n"
                    "Write RDB to disk, then REBOOT to use the new space.\n"
                    "(FFS picks up the new cylinder range only after reboot.)\n\n"
                    "Diagnostic: %s", errbuf);
            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)"Filesystem Grown";
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &ok_es, NULL, pi->drive_name);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            sprintf(full_msg, "FFS grow failed:\n%s", errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)"Filesystem Grow Failed";
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &err_es, NULL);
        }
    }
}

void offer_pfs_grow(struct Window *win, struct BlockDev *bd,
                           const struct RDBInfo *rdb, struct PartInfo *pi,
                           ULONG old_hi)
{
    struct EasyStruct es;
    char errbuf[256];

    if (pi->high_cyl <= old_hi) return;
    if (!PFS_IsSupportedType(pi->dos_type)) return;
    if (!bd) return;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"EXPERIMENTAL: Grow Filesystem";
    es.es_TextFormat   = (UBYTE *)
        "This will update PFS3/PFS2 filesystem metadata directly on disk.\n"
        "This feature is EXPERIMENTAL and may corrupt data.\n"
        "Always have a backup before proceeding.\n\n"
        "Grow PFS filesystem on partition %s?";
    es.es_GadgetFormat = (UBYTE *)"Grow Filesystem|Skip";

    if (EasyRequest(win, &es, NULL, pi->drive_name) == 1) {
        struct Screen *scr = win->WScreen;
        UWORD font_h = scr->Font ? scr->Font->ta_YSize : 8;
        UWORD bor_t  = (UWORD)(scr->WBorTop + font_h + 1);
        UWORD pw_w   = 360;
        UWORD pw_h   = (UWORD)(bor_t + scr->WBorBottom + font_h + 26);
        struct TagItem prog_tags[] = {
            { WA_Left,      (ULONG)((scr->Width  - pw_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - pw_h) / 2) },
            { WA_Width,     (ULONG)pw_w  },
            { WA_Height,    (ULONG)pw_h  },
            { WA_Title,     (ULONG)"Growing PFS Filesystem..." },
            { WA_PubScreen, (ULONG)scr   },
            { WA_Flags,     (ULONG)WFLG_DRAGBAR },
            { WA_IDCMP,     0             },
            { TAG_END,      0             }
        };
        struct Window *prog_win = OpenWindowTagList(NULL, prog_tags);
        struct GrowProgUD prog_ud;
        prog_ud.win   = prog_win;
        prog_ud.step  = 0;
        prog_ud.total = 6;   /* PFS_PROGRESS call count */

        BOOL result = PFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                        ffs_grow_progress, &prog_ud);
        if (prog_win) CloseWindow(prog_win);
        if (result) {
            struct EasyStruct ok_es;
            static char ok_msg[512];
            sprintf(ok_msg,
                    "PFS filesystem on %%s grown successfully.\n"
                    "Write RDB to disk, then REBOOT to use the new space.\n\n"
                    "Diagnostic: %s", errbuf);
            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)"Filesystem Grown";
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &ok_es, NULL, pi->drive_name);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            sprintf(full_msg, "PFS grow failed:\n%s", errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)"Filesystem Grow Failed";
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &err_es, NULL);
        }
    }
}

void offer_sfs_grow(struct Window *win, struct BlockDev *bd,
                           struct RDBInfo *rdb, struct PartInfo *pi,
                           ULONG old_hi)
{
    struct EasyStruct es;
    char errbuf[256];

    if (pi->high_cyl <= old_hi) return;
    if (!SFS_IsSupportedType(pi->dos_type)) return;
    if (!bd) return;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"EXPERIMENTAL: Grow Filesystem";
    es.es_TextFormat   = (UBYTE *)
        "This will update SmartFileSystem metadata directly on disk.\n"
        "This feature is EXPERIMENTAL and may corrupt data.\n"
        "Always have a backup before proceeding.\n\n"
        "Grow SFS filesystem on partition %s?";
    es.es_GadgetFormat = (UBYTE *)"Grow Filesystem|Skip";

    if (EasyRequest(win, &es, NULL, pi->drive_name) == 1) {
        struct Screen *scr = win->WScreen;
        UWORD font_h = scr->Font ? scr->Font->ta_YSize : 8;
        UWORD bor_t  = (UWORD)(scr->WBorTop + font_h + 1);
        UWORD pw_w   = 360;
        UWORD pw_h   = (UWORD)(bor_t + scr->WBorBottom + font_h + 26);
        struct TagItem prog_tags[] = {
            { WA_Left,      (ULONG)((scr->Width  - pw_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - pw_h) / 2) },
            { WA_Width,     (ULONG)pw_w  },
            { WA_Height,    (ULONG)pw_h  },
            { WA_Title,     (ULONG)"Growing SFS Filesystem..." },
            { WA_PubScreen, (ULONG)scr   },
            { WA_Flags,     (ULONG)WFLG_DRAGBAR },
            { WA_IDCMP,     0             },
            { TAG_END,      0             }
        };
        struct Window *prog_win = OpenWindowTagList(NULL, prog_tags);
        struct GrowProgUD prog_ud;
        prog_ud.win   = prog_win;
        prog_ud.step  = 0;
        prog_ud.total = 14;  /* SFS_PROGRESS call count */

        BOOL result = SFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                        ffs_grow_progress, &prog_ud);
        if (prog_win) CloseWindow(prog_win);
        if (result) {
            BOOL wrote_rdb = RDB_Write(bd, rdb);
            struct EasyStruct ok_es;
            static char ok_msg[512];
            if (wrote_rdb) {
                sprintf(ok_msg,
                        "SFS filesystem on %s grown successfully.\n"
                        "RDB written automatically.\n"
                        "%s: is INHIBITED (inaccessible) until reboot.\n"
                        "Reboot NOW to use the new space.\n\n"
                        "Diagnostic: %s",
                        pi->drive_name, pi->drive_name, errbuf);
            } else {
                sprintf(ok_msg,
                        "SFS filesystem on %s grown successfully.\n"
                        "WARNING: RDB write FAILED.\n"
                        "Click Write to save the RDB before rebooting.\n"
                        "%s: is INHIBITED (inaccessible) until reboot.\n"
                        "1. Click Write (save RDB)\n"
                        "2. Reboot NOW\n\n"
                        "Diagnostic: %s",
                        pi->drive_name, pi->drive_name, errbuf);
            }
            ok_es.es_StructSize   = sizeof(ok_es);
            ok_es.es_Flags        = 0;
            ok_es.es_Title        = (UBYTE *)"Filesystem Grown";
            ok_es.es_TextFormat   = (UBYTE *)ok_msg;
            ok_es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &ok_es, NULL);
        } else {
            struct EasyStruct err_es;
            static char full_msg[384];
            sprintf(full_msg, "SFS grow failed:\n%s", errbuf);
            err_es.es_StructSize   = sizeof(err_es);
            err_es.es_Flags        = 0;
            err_es.es_Title        = (UBYTE *)"Filesystem Grow Failed";
            err_es.es_TextFormat   = (UBYTE *)full_msg;
            err_es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &err_es, NULL);
        }
    }
}

