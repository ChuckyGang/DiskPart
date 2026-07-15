/*
 * diskselect.c - Reusable two-level "pick a device, then a unit" GUI flow.
 * See diskselect.h.
 *
 * Extracted from main.c so the flow can be reopened mid-session (e.g. to
 * pick a destination disk for a whole-disk copy) without duplicating the
 * window code or fighting over main.c's own persistent DevNameList/UnitList.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "devices.h"
#include "diskselect.h"
#include "guilv.h"
#include "locale_support.h"
#include "version.h"

/* ------------------------------------------------------------------ */
/* Gadget IDs - implementation detail, not part of the public API      */
/* ------------------------------------------------------------------ */

#define GID_LIST     1
#define GID_SELECT   2
#define GID_SHOWALL  3
#define GID_QUIT     4
#define GID_MANUAL   5
#define GID_USEIMAGE 6
#define GID_PROBE_CANCEL 13

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void list_init(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

/* Case-insensitive substring search. */
static BOOL str_contains_ci(const char *hay, const char *needle)
{
    ULONG nlen = strlen(needle);
    while (*hay) {
        ULONG i;
        BOOL  match = TRUE;
        for (i = 0; i < nlen; i++) {
            char h = hay[i], n = needle[i];
            if (!h) { match = FALSE; break; }  /* hay shorter than needle */
            if (h >= 'A' && h <= 'Z') h += 32;
            if (h != n) { match = FALSE; break; }
        }
        if (match) return TRUE;
        hay++;
    }
    return FALSE;
}

/* Returns TRUE if devname looks like a storage device worth showing by default. */
static BOOL is_storage_device(const char *name)
{
    static const char * const keys[] = { "ide", "scsi", "flash", "usb", "uae", "gvp", "phase5", "ppc", "sd", "mmc", "nvme", "card", "warp", NULL };
    UWORD i;
    for (i = 0; keys[i]; i++)
        if (str_contains_ci(name, keys[i])) return TRUE;
    return FALSE;
}

/*
 * Fill *lst / nodes[] from dn, applying filter when show_all=FALSE.
 * map[display_index] = dn index (so selection can be translated back).
 * Returns number of entries added.
 */
static UWORD build_name_list(struct DevNameList *dn, struct List *lst,
                              struct Node *nodes, WORD *map, BOOL show_all)
{
    UWORD i, count = 0;
    list_init(lst);
    for (i = 0; i < dn->count; i++) {
        if (!show_all && !is_storage_device(dn->names[i])) continue;
        nodes[count].ln_Name = dn->display[i];
        nodes[count].ln_Type = NT_USER;
        nodes[count].ln_Pri  = 0;
        AddTail(lst, &nodes[count]);
        map[count] = (WORD)i;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Level-1 window: choose a device driver name                         */
/* ------------------------------------------------------------------ */

/* Custom listview row renderer shared by the device-select and unit-select
   windows below. Both just list plain Node->ln_Name strings, but we still
   need a callback so we control how the selected row is marked: XOR-invert
   the row (COMPLEMENT draw mode) rather than filling with FILLPEN. FILLPEN
   can be visually indistinguishable from BACKGROUNDPEN on some real-hardware
   Workbench palettes, which makes GadTools' own default highlighting (and a
   naive FILLPEN fill) invisible even though it's technically being drawn.
   COMPLEMENT mode inverts whatever's there regardless of screen depth or
   palette, so it's always visible. */
/* Persistent "currently selected" row index, shared by the device-select
   and unit-select windows below (they're never open at the same time - see
   namelist_lv_hook.h_Data). Both windows reset this to -1 on entry so a
   selection from a previous session/window never bleeds into a fresh one. */
static WORD g_namelist_sel = -1;

static ULONG namelist_lv_render(void)
{
    register struct Hook      *h    __asm__("a0");
    register struct LVDrawMsg *msg  __asm__("a1");
    register struct Node      *node __asm__("a2");
    struct Hook      *_h    = h;
    struct LVDrawMsg *_msg  = msg;
    struct Node      *_node = node;
#define h    _h
#define msg  _msg
#define node _node

    struct RastPort  *rp;
    struct Rectangle *b;
    BOOL   sel;
    UWORD  bg_pen, fg_pen;
    const char *name;
    UWORD  len;
    WORD   idx;

    if (msg->lvdm_MethodID != LV_DRAW) return LVCB_OK;

    rp  = msg->lvdm_RastPort;
    b   = &msg->lvdm_Bounds;
    /* lvdm_State only reports LVR_SELECTED while the mouse button is held
       down over the row (live click-tracking) - it reverts to LVR_NORMAL
       the instant the button is released, on every ROM/platform tested,
       so it can't be used alone to show a persistent "this is the chosen
       item" mark. h_Data points at whichever node array is currently in
       use (name_nodes or unit_nodes - set right before the gadget using
       this hook is created); comparing this row's index against
       g_namelist_sel is the persistent flag instead. */
    idx = (WORD)(node - (struct Node *)h->h_Data);
    sel = (msg->lvdm_State == LVR_SELECTED ||
           msg->lvdm_State == LVR_SELECTEDDISABLED) ||
          (idx == g_namelist_sel);

    bg_pen = (UWORD)msg->lvdm_DrawInfo->dri_Pens[BACKGROUNDPEN];
    fg_pen = (UWORD)msg->lvdm_DrawInfo->dri_Pens[TEXTPEN];

    SetAPen(rp, (LONG)bg_pen);
    SetDrMd(rp, JAM2);
    RectFill(rp, b->MinX, b->MinY, b->MaxX, b->MaxY);

    name = node->ln_Name ? node->ln_Name : "";
    len  = (UWORD)strlen(name);

    SetAPen(rp, (LONG)fg_pen);
    SetDrMd(rp, JAM1);
    Move(rp, b->MinX + 2, b->MinY + (WORD)rp->TxBaseline);
    Text(rp, name, len);

    if (sel) {
        SetDrMd(rp, COMPLEMENT);
        RectFill(rp, b->MinX, b->MinY, b->MaxX, b->MaxY);
        SetDrMd(rp, JAM1);
    }

    return LVCB_OK;
#undef h
#undef msg
#undef node
}

static struct Hook namelist_lv_hook;   /* h_Entry/h_Data set before each use below */

static BOOL confirm_exit(struct Window *win)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
    es.es_TextFormat   = (UBYTE *)GS(MSG_MAIN_EXIT_BODY);
    es.es_GadgetFormat = (UBYTE *)GS(MSG_YES_NO);
    return (BOOL)(EasyRequestArgs(win, &es, NULL, NULL) == 1);
}

WORD DiskSelect_PickDeviceName(struct DevNameList *dn, char *manual_devname_out,
                               BOOL allow_image)
{
    struct Screen  *scr       = NULL;
    APTR            vi        = NULL;
    struct Gadget  *glist     = NULL;
    struct Gadget  *gctx      = NULL;
    struct Gadget  *lv_gad    = NULL;
    struct Gadget  *showall_gad = NULL;
    struct Gadget  *str_gad   = NULL;
    struct Gadget  *useimage_gad = NULL;
    struct Window  *win       = NULL;
    WORD            sel       = -1;
    WORD            result    = -1;
    WORD            dbl_list_sel = -1;
    ULONG           dbl_list_sec = 0;
    ULONG           dbl_list_mic = 0;
    static char     dev_title[80];   /* Intuition keeps the pointer */
    /* Sticky across window reopens so a user-enabled Show All survives a
       round-trip through the partition editor.  Otherwise devices outside
       the default keyword filter (e.g. warpSD.device) appear to vanish
       after they're used. */
    static BOOL     show_all  = FALSE;

    struct Node name_nodes[MAX_DEV_NAMES];
    struct List name_list;
    WORD        sel_map[MAX_DEV_NAMES];
    UWORD       display_count;

    /* g_namelist_sel is shared with DiskSelect_PickUnit() - reset it so a
       selection from a previous window/session never shows highlighted
       here before the user has clicked anything. */
    g_namelist_sel = -1;

    display_count = build_name_list(dn, &name_list, name_nodes, sel_map, show_all);

    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;

    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD font_x  = scr->RastPort.Font ? (UWORD)scr->RastPort.Font->tf_XSize : 8;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 400;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 4;
        /* Format device display strings now that we know the font width.
           Listview pixel width minus scrollbar (~16px) and frame borders
           (~4px each side) and text indent (~2px), divided by font x-size. */
        {
            UWORD lv_px  = inner_w - pad * 2;
            UWORD cols   = (lv_px > 30) ? (UWORD)((lv_px - 30) / font_x) : 20;
            DevNameList_FormatDisplay(dn, cols);
        }
        UWORD btn_h   = font_h + 6;
        UWORD lbl_h   = (UWORD)(font_h + 2);  /* label floats above gadget top */
        /* Size the listview to fit the actual device count.
           Min 10 rows, max whatever fits on screen above the fixed UI below it. */
        UWORD lv_h;
        {
            UWORD row_h     = (UWORD)(font_h + 2);
            UWORD fixed_h   = bor_t + pad          /* top */
                            + pad + lbl_h           /* label above string gadget */
                            + btn_h + pad           /* string gadget */
                            + btn_h + pad           /* button row */
                            + bor_b;                /* bottom border */
            UWORD max_rows  = (scr->Height > fixed_h + row_h * 4)
                              ? (UWORD)((scr->Height - fixed_h) / row_h) : 10;
            UWORD want_rows = (display_count > 10) ? display_count : 10;
            if (want_rows > max_rows) want_rows = max_rows;
            lv_h = row_h * want_rows;
        }
        UWORD str_y   = bor_t + pad + lv_h + pad + lbl_h; /* room for label above */
        UWORD btn_y   = str_y + btn_h + pad;
        UWORD win_h   = btn_y + btn_h + pad + bor_b;
        /* Three or four buttons: [Select] [Show All] ([Use Image]) [Quit] */
        UWORD nbtns   = allow_image ? 4 : 3;
        UWORD btn_w   = (inner_w - pad * 2 - pad * (nbtns - 1)) / nbtns;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            struct TagItem bt[]      = { { TAG_DONE, 0 } };
            struct TagItem lv_tags[] = {
                { GTLV_Labels,   (ULONG)&name_list      },
                { GTLV_CallBack, (ULONG)&namelist_lv_hook },
                { TAG_DONE,    0                 }
            };
            struct Gadget *prev;

            namelist_lv_hook.h_Entry    = (HOOKFUNC)namelist_lv_render;
            namelist_lv_hook.h_SubEntry = NULL;
            namelist_lv_hook.h_Data     = (APTR)name_nodes;

            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;
            ng.ng_LeftEdge   = bor_l + pad;
            ng.ng_TopEdge    = bor_t + pad;
            ng.ng_Width      = inner_w - pad * 2;
            ng.ng_Height     = lv_h;
            ng.ng_GadgetID   = GID_LIST;

            lv_gad = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lv_tags);
            if (!lv_gad) goto cleanup;

            ng.ng_TopEdge    = str_y;
            ng.ng_LeftEdge   = bor_l + pad;
            ng.ng_Width      = inner_w - pad * 2;
            ng.ng_Height     = btn_h;
            ng.ng_GadgetText = GS(MSG_MAIN_MANUAL_DEV);
            ng.ng_GadgetID   = GID_MANUAL;
            ng.ng_Flags      = PLACETEXT_ABOVE;
            {
                struct TagItem str_tags[] = {
                    { GTST_MaxChars, 63 },
                    { TAG_DONE,      0  }
                };
                str_gad = CreateGadgetA(STRING_KIND, lv_gad, &ng, str_tags);
                if (!str_gad) goto cleanup;
            }
            ng.ng_Flags = 0;

            ng.ng_TopEdge = btn_y;
            ng.ng_Height  = btn_h;
            ng.ng_Width   = btn_w;

            ng.ng_LeftEdge   = bor_l + pad;
            ng.ng_GadgetText = GS(MSG_MAIN_SELECT);
            ng.ng_GadgetID   = GID_SELECT;
            prev = CreateGadgetA(BUTTON_KIND, str_gad, &ng, bt);
            if (!prev) goto cleanup;

            ng.ng_LeftEdge   = bor_l + pad + btn_w + pad;
            ng.ng_GadgetText = show_all ? GS(MSG_MAIN_FILTER) : GS(MSG_MAIN_SHOWALL);
            ng.ng_GadgetID   = GID_SHOWALL;
            showall_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
            if (!showall_gad) goto cleanup;
            prev = showall_gad;

            if (allow_image) {
                ng.ng_LeftEdge   = bor_l + pad + (btn_w + pad) * 2;
                ng.ng_GadgetText = GS(MSG_MAIN_USE_IMAGE);
                ng.ng_GadgetID   = GID_USEIMAGE;
                useimage_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
                if (!useimage_gad) goto cleanup;
                prev = useimage_gad;
            }

            ng.ng_LeftEdge   = bor_l + pad + (btn_w + pad) * (nbtns - 1);
            ng.ng_GadgetText = GS(MSG_MAIN_QUIT);
            ng.ng_GadgetID   = GID_QUIT;
            prev = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
            if (!prev) goto cleanup;
        }

        DP_SNPRINTF(dev_title, "%s%s", DISKPART_VERTITLE,
                GS(MSG_MAIN_TITLE_SELECT_DEV));

        {
            struct TagItem win_tags[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width,     win_w },
                { WA_Height,    win_h },
                { WA_Title,     (ULONG)dev_title },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                IDCMP_GADGETDOWN  | IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                WFLG_SIMPLE_REFRESH },
                { TAG_DONE,     0 }
            };
            win = OpenWindowTagList(NULL, win_tags);
        }
    }

    UnlockPubScreen(NULL, scr);
    scr = NULL;
    if (!win) goto cleanup;

    GT_RefreshWindow(win, NULL);

    {
        BOOL running   = TRUE;
        BOOL do_select = FALSE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG          iclass = imsg->Class;
                UWORD          code   = imsg->Code;
                ULONG          ev_sec = imsg->Seconds;
                ULONG          ev_mic = imsg->Micros;
                struct Gadget *gad    = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (iclass) {
                case IDCMP_CLOSEWINDOW:
                    if (confirm_exit(win)) { result = DISKSEL_EXIT; running = FALSE; }
                    break;
                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == GID_LIST)
                        sel = (WORD)code;
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GID_LIST:
                        sel = (WORD)code;
                        /* Persist the highlight past GADGETUP - see the
                           comment on lvdm_State in namelist_lv_render().
                           GT_SetGadgetAttrsA(GTLV_Selected,...) alone does
                           NOT bring the row back once the mouse button is
                           released (confirmed on real KS3.1/3.2 hardware),
                           so gui_force_lv_redraw() forces the listview to
                           fully detach/reattach - a plain RefreshGList
                           isn't enough to make GTLV_CallBack re-render. */
                        g_namelist_sel = sel;
                        gui_force_lv_redraw(gad, win, &name_list);
                        /* Fixed-window double-click test rather than
                           IEQUALIFIER_DOUBLECLICK - see quick_double_click()
                           comment above; the qualifier isn't reliable on
                           real hardware. */
                        if (sel >= 0 && sel == dbl_list_sel &&
                            quick_double_click(dbl_list_sec, dbl_list_mic,
                                               ev_sec, ev_mic)) {
                            dbl_list_sel = -1;
                            do_select    = TRUE;
                        } else {
                            dbl_list_sel = sel;
                            dbl_list_sec = ev_sec;
                            dbl_list_mic = ev_mic;
                        }
                        break;
                    case GID_SELECT:
                        do_select = TRUE;
                        break;
                    case GID_MANUAL:   /* Enter pressed in string gadget */
                        do_select = TRUE;
                        break;
                    case GID_SHOWALL:
                    {
                        struct TagItem detach[] = { { GTLV_Labels, ~0UL      }, { TAG_DONE, 0 } };
                        struct TagItem reattach[]= { { GTLV_Labels, 0        }, { TAG_DONE, 0 } };
                        struct TagItem relabel[] = { { GA_Text,     0        }, { TAG_DONE, 0 } };
                        show_all = !show_all;
                        GT_SetGadgetAttrsA(lv_gad, win, NULL, detach);
                        display_count = build_name_list(dn, &name_list, name_nodes,
                                                        sel_map, show_all);
                        reattach[0].ti_Data = (ULONG)&name_list;
                        GT_SetGadgetAttrsA(lv_gad, win, NULL, reattach);
                        relabel[0].ti_Data  = (ULONG)(show_all ? GS(MSG_MAIN_FILTER) : GS(MSG_MAIN_SHOWALL));
                        GT_SetGadgetAttrsA(showall_gad, win, NULL, relabel);
                        RefreshGList(showall_gad, win, NULL, 1);
                        sel = -1;
                        /* The list just got rebuilt/reindexed - drop any
                           highlight and any in-flight double-click tracking,
                           otherwise a click landing on the old dbl_list_sel
                           index within the timing window would misfire a
                           double-click-select on a re-numbered row. */
                        g_namelist_sel = -1;
                        dbl_list_sel   = -1;
                        break;
                    }
                    case GID_USEIMAGE:
                        result  = DISKSEL_IMAGE;
                        running = FALSE;
                        break;
                    case GID_QUIT:
                        running = FALSE;
                        break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    break;
                }

                if (do_select) {
                    do_select = FALSE;
                    {
                        const char *typed =
                            ((struct StringInfo *)str_gad->SpecialInfo)->Buffer;
                        if (typed[0] != '\0') {
                            strncpy(manual_devname_out, typed, 63);
                            manual_devname_out[63] = '\0';
                            result  = DISKSEL_MANUAL;
                            running = FALSE;
                        } else if (sel >= 0 && sel < (WORD)display_count) {
                            result  = sel_map[sel];
                            running = FALSE;
                        }
                    }
                }
            }
        }
    }

cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return result;
}

/* ------------------------------------------------------------------ */
/* Probe progress window                                               */
/*                                                                     */
/* Opens before unit probing starts. One status line updates in-place  */
/* ("Testing unit N...") and found units accumulate below it.         */
/* ------------------------------------------------------------------ */

#define PROBE_WIN_MAX_RESULTS  12

struct ProbeWin {
    struct Window *win;
    APTR           vi;        /* GadTools visual info (Cancel button) */
    struct Gadget *glist;     /* gadget context list to free          */
    BOOL           cancelled; /* set TRUE once Cancel is clicked       */
    UWORD  x;           /* left text margin */
    UWORD  baseline;    /* font baseline from top of cell */
    UWORD  line_h;      /* pixels per text row             */
    UWORD  y_status;    /* y of the "Testing unit N" row   */
    UWORD  y_result0;   /* y of first result row           */
    UWORD  results;     /* number of result rows written   */
    char   title[80];   /* Intuition keeps a pointer - must outlive win */
};

static BOOL probe_win_cb(void *ud, ULONG unit, UWORD phase, const char *info)
{
    struct ProbeWin *pw = (struct ProbeWin *)ud;
    struct RastPort *rp;
    char   buf[128];
    WORD   len, pad;

    if (!pw->win) return TRUE;   /* no window -> nothing to cancel with */
    rp = pw->win->RPort;

    switch (phase) {
    case PROBE_START:
        DP_SNPRINTF(buf, GS(MSG_MAIN_TESTING_UNIT_FMT), (unsigned long)unit);
        /* Pad to 60 chars so previous longer text is fully erased */
        len = (WORD)strlen(buf);
        for (pad = len; pad < 60; pad++) buf[pad] = ' ';
        buf[60] = '\0';
        SetAPen(rp, 1);
        Move(rp, pw->x, pw->y_status);
        Text(rp, buf, strlen(buf));
        break;

    case PROBE_FOUND:
        if (pw->results >= PROBE_WIN_MAX_RESULTS) break;
        DP_SNPRINTF(buf, GS(MSG_MAIN_UNIT_FMT), (unsigned long)unit,
                info ? info : GS(MSG_MAIN_FOUND));
        SetAPen(rp, 1);
        Move(rp, pw->x, pw->y_result0 + (WORD)pw->results * (WORD)pw->line_h);
        Text(rp, buf, strlen(buf));
        pw->results++;
        break;

    case PROBE_EMPTY:
        /* Nothing - status line already shows "Testing unit N..." */
        break;
    }

    /* Drain any pending input. A Cancel click aborts the rest of the scan.
       Note: this can only be serviced between units - while a single unit's
       DoIO() is blocked in the driver, no message is processed. */
    {
        struct IntuiMessage *imsg;
        while ((imsg = GT_GetIMsg(pw->win->UserPort)) != NULL) {
            ULONG          cls = imsg->Class;
            struct Gadget *gad = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            if (cls == IDCMP_GADGETUP && gad &&
                gad->GadgetID == GID_PROBE_CANCEL)
                pw->cancelled = TRUE;
        }
    }

    return (BOOL)!pw->cancelled;
}

static void probe_win_open(struct ProbeWin *pw, const char *devname)
{
    struct Screen *scr;
    struct Gadget *gctx = NULL;
    UWORD fh, bor_l, bor_t, bor_r, bor_b, pad, lh, win_w, win_h, rows;
    UWORD btn_h, btn_w, btn_y;

    memset(pw, 0, sizeof(*pw));
    {
        int n = DP_SNPRINTF(pw->title, "%s", DISKPART_VERTITLE);
        sprintf(pw->title + n, GS(MSG_MAIN_TITLE_PROBING_FMT), devname);
    }

    scr = LockPubScreen(NULL);
    if (!scr) return;

    fh    = scr->Font->ta_YSize;
    bor_l = (UWORD)scr->WBorLeft;
    bor_t = (UWORD)scr->WBorTop + fh + 1;
    bor_r = (UWORD)scr->WBorRight;
    bor_b = (UWORD)scr->WBorBottom;
    pad   = 4;
    lh    = fh + 2;
    /* 1 header row + 1 status row + result rows */
    rows  = 2 + PROBE_WIN_MAX_RESULTS;
    btn_h = fh + 6;
    btn_w = 100;
    win_w = 420;
    /* room for header/status/results, then a Cancel button row */
    win_h = bor_t + pad + rows * lh + pad + btn_h + pad + bor_b;
    btn_y = bor_t + pad + rows * lh + pad;

    /* Build the Cancel button. If GadTools setup fails we still open the
       window without a button - probing just can't be cancelled then. */
    pw->vi = GetVisualInfoA(scr, NULL);
    if (pw->vi) {
        gctx = CreateContext(&pw->glist);
        if (gctx) {
            struct NewGadget ng;
            struct TagItem   bt[] = { { TAG_DONE, 0 } };
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = pw->vi;
            ng.ng_TextAttr   = scr->Font;
            ng.ng_LeftEdge   = (win_w - btn_w) / 2;
            ng.ng_TopEdge    = btn_y;
            ng.ng_Width      = btn_w;
            ng.ng_Height     = btn_h;
            ng.ng_GadgetText = GS(MSG_CANCEL);
            ng.ng_GadgetID   = GID_PROBE_CANCEL;
            CreateGadgetA(BUTTON_KIND, gctx, &ng, bt);
        }
    }

    {
        struct TagItem win_tags[] = {
            { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
            { WA_Width,     win_w  },
            { WA_Height,    win_h  },
            { WA_Title,     (ULONG)pw->title },
            { WA_PubScreen, (ULONG)scr },
            { WA_Gadgets,   (ULONG)pw->glist },
            { WA_IDCMP,     pw->glist ? IDCMP_GADGETUP : 0 },
            { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH },
            { TAG_DONE,     0 }
        };
        pw->win = OpenWindowTagList(NULL, win_tags);
    }

    UnlockPubScreen(NULL, scr);
    if (!pw->win) return;

    if (pw->glist)
        GT_RefreshWindow(pw->win, NULL);

    pw->x        = bor_l + pad;
    pw->line_h   = lh;
    pw->baseline = pw->win->RPort->Font
                   ? pw->win->RPort->Font->tf_Baseline
                   : (UWORD)(fh - 1);

    /* Row 0: device name header */
    pw->y_status  = bor_t + pad +       pw->baseline;
    pw->y_result0 = bor_t + pad + lh  + pw->baseline;

    /* Draw the header line (row 0) now; status/results come via callback */
    {
        char hdr[80];
        DP_SNPRINTF(hdr, GS(MSG_MAIN_DEVICE_FMT), devname);
        SetAPen(pw->win->RPort, 1);
        Move(pw->win->RPort, pw->x, pw->y_status);
        Text(pw->win->RPort, hdr, strlen(hdr));
    }

    /* Push status and results down one more row below header */
    pw->y_status  += lh;
    pw->y_result0 += lh;
}

static void probe_win_close(struct ProbeWin *pw)
{
    if (pw->win) {
        if (pw->glist) RemoveGList(pw->win, pw->glist, -1);
        CloseWindow(pw->win);
        pw->win = NULL;
    }
    if (pw->glist) { FreeGadgets(pw->glist); pw->glist = NULL; }
    if (pw->vi)    { FreeVisualInfo(pw->vi); pw->vi    = NULL; }
}

BOOL DiskSelect_ProbeUnits(const char *devname, struct UnitList *ul)
{
    static struct ProbeWin pw;
    probe_win_open(&pw, devname);
    Devices_GetUnitsForName(devname, ul, probe_win_cb, &pw);
    probe_win_close(&pw);
    return (BOOL)(ul->count != 0);
}

/* ------------------------------------------------------------------ */
/* Level-2 window: choose a unit for devname                           */
/* ------------------------------------------------------------------ */

WORD DiskSelect_PickUnit(const char *devname, struct UnitList *ul)
{
    struct Screen  *scr    = NULL;
    APTR            vi     = NULL;
    struct Gadget  *glist  = NULL;
    struct Gadget  *gctx   = NULL;
    struct Window  *win    = NULL;
    WORD            sel    = -1;
    WORD            result = -1;
    WORD            dbl_list_sel = -1;
    ULONG           dbl_list_sec = 0;
    ULONG           dbl_list_mic = 0;
    static char     win_title[80];

    struct Node unit_nodes[MAX_KNOWN_DEVICES];
    struct List ulist;
    UWORD i;

    /* g_namelist_sel is shared with DiskSelect_PickDeviceName() - reset it
       so a selection from a previous window/session never shows highlighted
       here before the user has clicked anything. */
    g_namelist_sel = -1;

    list_init(&ulist);
    for (i = 0; i < ul->count; i++) {
        unit_nodes[i].ln_Name = ul->entries[i].display;
        unit_nodes[i].ln_Type = NT_USER;
        unit_nodes[i].ln_Pri  = 0;
        AddTail(&ulist, &unit_nodes[i]);
    }

    {
        int n = DP_SNPRINTF(win_title, "%s", DISKPART_VERTITLE);
        sprintf(win_title + n, GS(MSG_MAIN_TITLE_DASH_FMT), devname);
    }

    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;

    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 520;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 4;
        UWORD btn_h   = font_h + 6;
        /* Size the listview to the number of units found, min 4, capped by screen. */
        UWORD lv_h;
        {
            UWORD row_h     = (UWORD)(font_h + 2);
            UWORD fixed_h   = bor_t + pad + pad + btn_h + pad + bor_b;
            UWORD max_rows  = (scr->Height > fixed_h + row_h * 4)
                              ? (UWORD)((scr->Height - fixed_h) / row_h) : 4;
            UWORD want_rows = (ul->count > 4) ? (UWORD)ul->count : 4;
            if (want_rows > max_rows) want_rows = max_rows;
            lv_h = row_h * want_rows;
        }
        UWORD win_h   = bor_t + pad + lv_h + pad + btn_h + pad + bor_b;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

            ng.ng_LeftEdge   = bor_l + pad;
            ng.ng_TopEdge    = bor_t + pad;
            ng.ng_Width      = inner_w - pad * 2;
            ng.ng_Height     = lv_h;
            ng.ng_GadgetText = NULL;
            ng.ng_GadgetID   = GID_LIST;
            ng.ng_Flags      = 0;

            {
                struct TagItem lv_tags[] = {
                    { GTLV_Labels,   (ULONG)&ulist            },
                    { GTLV_CallBack, (ULONG)&namelist_lv_hook },
                    { TAG_DONE,    0              }
                };
                struct Gadget *lv_gad;

                namelist_lv_hook.h_Entry    = (HOOKFUNC)namelist_lv_render;
                namelist_lv_hook.h_SubEntry = NULL;
                namelist_lv_hook.h_Data     = (APTR)unit_nodes;

                lv_gad = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lv_tags);
                if (!lv_gad) goto cleanup;

                {
                    UWORD btn_y  = bor_t + pad + lv_h + pad;
                    UWORD half_w = (inner_w - pad * 2 - pad) / 2;
                    struct TagItem bt[] = { { TAG_DONE, 0 } };
                    struct Gadget *sel_gad, *back_gad;

                    ng.ng_TopEdge    = btn_y;
                    ng.ng_Height     = btn_h;
                    ng.ng_Width      = half_w;
                    ng.ng_LeftEdge   = bor_l + pad;
                    ng.ng_GadgetText = GS(MSG_MAIN_SELECT);
                    ng.ng_GadgetID   = GID_SELECT;
                    sel_gad = CreateGadgetA(BUTTON_KIND, lv_gad, &ng, bt);
                    if (!sel_gad) goto cleanup;

                    ng.ng_LeftEdge   = bor_l + pad + half_w + pad;
                    ng.ng_GadgetText = GS(MSG_MAIN_BACK);
                    ng.ng_GadgetID   = GID_QUIT;
                    back_gad = CreateGadgetA(BUTTON_KIND, sel_gad, &ng, bt);
                    if (!back_gad) goto cleanup;
                }
            }

            {
                struct TagItem win_tags[] = {
                    { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                    { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                    { WA_Width,     win_w },
                    { WA_Height,    win_h },
                    { WA_Title,     (ULONG)win_title },
                    { WA_Gadgets,   (ULONG)glist },
                    { WA_PubScreen, (ULONG)scr },
                    { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                    IDCMP_GADGETDOWN  | IDCMP_REFRESHWINDOW },
                    { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                    WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                    WFLG_SIMPLE_REFRESH },
                    { TAG_DONE,     0 }
                };
                win = OpenWindowTagList(NULL, win_tags);
            }
        }
    }

    UnlockPubScreen(NULL, scr);
    scr = NULL;
    if (!win) goto cleanup;

    GT_RefreshWindow(win, NULL);

    {
        BOOL running   = TRUE;
        BOOL do_select = FALSE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG          iclass = imsg->Class;
                UWORD          code   = imsg->Code;
                ULONG          ev_sec = imsg->Seconds;
                ULONG          ev_mic = imsg->Micros;
                struct Gadget *gad    = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (iclass) {
                case IDCMP_CLOSEWINDOW:
                    if (confirm_exit(win)) { result = DISKSEL_EXIT; running = FALSE; }
                    break;
                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == GID_LIST)
                        sel = (WORD)code;
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GID_LIST:
                        sel = (WORD)code;
                        /* Persist the highlight past GADGETUP - see the
                           comment in DiskSelect_PickDeviceName()'s GID_LIST
                           handler and in namelist_lv_render(). */
                        g_namelist_sel = sel;
                        gui_force_lv_redraw(gad, win, &ulist);
                        /* Fixed-window double-click test rather than
                           IEQUALIFIER_DOUBLECLICK - see quick_double_click()
                           comment above; the qualifier isn't reliable on
                           real hardware. */
                        if (sel >= 0 && sel == dbl_list_sel &&
                            quick_double_click(dbl_list_sec, dbl_list_mic,
                                               ev_sec, ev_mic)) {
                            dbl_list_sel = -1;
                            do_select    = TRUE;
                        } else {
                            dbl_list_sel = sel;
                            dbl_list_sec = ev_sec;
                            dbl_list_mic = ev_mic;
                        }
                        break;
                    case GID_SELECT:
                        do_select = TRUE;
                        break;
                    case GID_QUIT:   /* "Back" button */
                        running = FALSE;
                        break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    break;
                }

                if (do_select) {
                    do_select = FALSE;
                    if (sel >= 0 && sel < (WORD)ul->count) {
                        result  = sel;
                        running = FALSE;
                    }
                }
            }
        }
    }

cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return result;
}
