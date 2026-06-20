/*
 * partview_zero.c - Zero out a partition's data blocks.
 *
 * Accessible via Advanced > Zero Partition...
 * Writes zeros to every physical block in the selected partition.
 * Useful before disk imaging: zeroed blocks compress far better than
 * stale filesystem data.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include "clib.h"
#include "rdb.h"
#include "devices.h"
#include "partmove.h"
#include "partview_internal.h"
#include "locale_support.h"

extern struct ExecBase      *SysBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;

/* ------------------------------------------------------------------ */
/* Progress window drawing (same pattern as partview_move.c)           */
/* ------------------------------------------------------------------ */

struct ZeroProgUD {
    struct Window *win;
};

static void zero_progress_fn(void *ud, ULONG done, ULONG total,
                              const char *phase)
{
    struct ZeroProgUD *pu = (struct ZeroProgUD *)ud;
    struct Window   *pw;
    struct RastPort *rp;
    WORD x1, y1, x2;
    WORD bar_x, bar_y, bar_w, bar_h, filled_w;
    char  pct_str[8];
    ULONG pct;
    UWORD plen, phlen;

    if (!pu || !pu->win) return;
    pw = pu->win;
    rp = pw->RPort;
    x1 = pw->BorderLeft;
    y1 = pw->BorderTop;
    x2 = (WORD)(pw->Width  - 1 - pw->BorderRight);

    SetAPen(rp, 0);
    RectFill(rp, x1, y1, x2, (WORD)(pw->Height - 1 - pw->BorderBottom));

    bar_x = (WORD)(x1 + 4);
    bar_y = (WORD)(y1 + 4);
    bar_w = (WORD)(x2 - x1 - 8);
    bar_h = 6;

    if (bar_w > 0) {
        pct      = (total > 0) ? (done * 100UL / total) : 0;
        filled_w = (total > 0) ? (WORD)((ULONG)bar_w * done / total) : 0;

        SetAPen(rp, 2);
        RectFill(rp, bar_x, bar_y,
                 (WORD)(bar_x + bar_w), (WORD)(bar_y + bar_h));
        if (filled_w > 0) {
            SetAPen(rp, 3);
            RectFill(rp, bar_x, bar_y,
                     (WORD)(bar_x + filled_w), (WORD)(bar_y + bar_h));
        }
        SetAPen(rp, 1);
        Move(rp, bar_x, bar_y);
        Draw(rp, (WORD)(bar_x + bar_w), bar_y);
        Draw(rp, (WORD)(bar_x + bar_w), (WORD)(bar_y + bar_h));
        Draw(rp, bar_x, (WORD)(bar_y + bar_h));
        Draw(rp, bar_x, bar_y);

        pct_str[0] = (UBYTE)('0' + pct / 100);
        pct_str[1] = (UBYTE)('0' + (pct / 10) % 10);
        pct_str[2] = (UBYTE)('0' + pct % 10);
        pct_str[3] = '%';
        pct_str[4] = '\0';
        plen = 4;
        if (pct_str[0] == '0') {
            pct_str[0] = pct_str[1]; pct_str[1] = pct_str[2];
            pct_str[2] = pct_str[3]; pct_str[3] = '\0'; plen = 3;
            if (pct_str[0] == '0') {
                pct_str[0] = pct_str[1]; pct_str[1] = pct_str[2];
                pct_str[2] = '\0'; plen = 2;
            }
        }
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

    SetAPen(rp, 1);
    phlen = 0; while (phase[phlen]) phlen++;
    Move(rp, (WORD)(x1 + 6),
         (WORD)(bar_y + bar_h + 4 + rp->TxBaseline));
    Text(rp, (STRPTR)phase, (WORD)phlen);
}

/* ------------------------------------------------------------------ */
/* offer_zero_partition                                                 */
/* ------------------------------------------------------------------ */
void offer_zero_partition(struct Window *win, struct BlockDev *bd,
                          const struct RDBInfo *rdb, struct PartInfo *pi)
{
    struct EasyStruct es;
    static char confirm_body[256];
    char err_buf[256];
    LONG r;

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_ZERO_CONFIRM_TITLE);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_ZERO_CONFIRM_GADGETS);

    DP_SNPRINTF(confirm_body, GS(MSG_ZERO_CONFIRM_BODY_FMT),
                pi->drive_name,
                (unsigned long)pi->low_cyl,
                (unsigned long)pi->high_cyl);
    es.es_TextFormat = (UBYTE *)confirm_body;

    r = EasyRequest(win, &es, NULL);
    if (r != 1) return;   /* Cancel */

    {
        struct Screen *pscr = LockPubScreen(NULL);
        struct Window *prog_win = NULL;

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
                { WA_Title,     (ULONG)GS(MSG_ZERO_PROG_TITLE) },
                { WA_PubScreen, (ULONG)pscr },
                { WA_Flags,     WFLG_DRAGBAR },
                { WA_IDCMP,     0 },
                { TAG_END, 0 }
            };
            prog_win = OpenWindowTagList(NULL, pt);
            UnlockPubScreen(NULL, pscr);
        }

        {
            struct ZeroProgUD prog_ud;
            ULONG heads   = pi->heads   > 0 ? pi->heads   : rdb->heads;
            ULONG sectors = pi->sectors > 0 ? pi->sectors : rdb->sectors;
            ULONG total_blocks = (pi->high_cyl - pi->low_cyl + 1) * heads * sectors;
            BOOL ok;

            prog_ud.win = prog_win;
            ok = PART_Zero(bd, rdb, pi, err_buf, zero_progress_fn, &prog_ud);

            if (prog_win) { CloseWindow(prog_win); prog_win = NULL; }

            es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
            if (ok) {
                static char ok_msg[128];
                DP_SNPRINTF(ok_msg, GS(MSG_ZERO_OK_FMT),
                            pi->drive_name, (unsigned long)total_blocks);
                es.es_Title      = (UBYTE *)GS(MSG_ZERO_OK_TITLE);
                es.es_TextFormat = (UBYTE *)ok_msg;
            } else {
                es.es_Title      = (UBYTE *)GS(MSG_ZERO_FAIL_TITLE);
                es.es_TextFormat = (UBYTE *)err_buf;
            }
            EasyRequest(win, &es, NULL);
        }
    }
}
