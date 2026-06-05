/*
 * main.c - DiskPart two-level device selection.
 *
 * Level 1: list of exec device driver names that responded to probing.
 * Level 2: list of units for the chosen driver, showing disk name/size.
 * Level 3: partition editor (partview.c).
 *
 * AmigaOS 2.x+ (Kickstart v37+), m68k-amiga-elf-gcc (Bartman toolchain).
 * GadTools UI - no MUI, no external library dependencies.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <devices/inputevent.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#ifndef IEQUALIFIER_DOUBLECLICK
#define IEQUALIFIER_DOUBLECLICK 0x8000
#endif
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/icon.h>

#include "cli.h"
#include "clib.h"
#include "devices.h"
#include "locale_support.h"
#include "partview.h"
#include "rdb.h"
#include "version.h"

static const char diskpart_ver[] = "$VER: DiskPart 0.1 (2026)";

/* ------------------------------------------------------------------ */
/* Library bases - SysBase set by main() before any LP call            */
/* ------------------------------------------------------------------ */

struct ExecBase      *SysBase;
struct DosLibrary    *DOSBase        = NULL;
struct IntuitionBase *IntuitionBase  = NULL;
struct GfxBase       *GfxBase        = NULL;
struct Library       *GadToolsBase   = NULL;
struct Library       *AslBase        = NULL;
struct Library       *IconBase       = NULL;
struct Library       *ExpansionBase  = NULL;

/* Populated by Bartman _start when launched from Workbench (see
 * support/gcc8_c_support.c).  NULL on CLI launch or under toolchains
 * that don't supply a custom _start (e.g. Bebbo) - WB tooltype
 * lookup will then be a no-op. */
struct WBStartup *DiskPart_WBStartup = NULL;

/* ------------------------------------------------------------------ */
/* Gadget IDs                                                           */
/* ------------------------------------------------------------------ */

#define GID_LIST     1
#define GID_SELECT   2
#define GID_SHOWALL  3
#define GID_QUIT     4
#define GID_MANUAL   5
#define GID_USEIMAGE 6

#define RESULT_MANUAL (-3)
#define RESULT_IMAGE  (-4)

/* Gadget IDs for image-size dialog */
#define GID_SZ_STR  10
#define GID_SZ_OK   11
#define GID_SZ_CANC 12

/* Gadget ID for the probe-progress window's Cancel button */
#define GID_PROBE_CANCEL 13

/* ------------------------------------------------------------------ */
/* Static data (too large for stack)                                   */
/* ------------------------------------------------------------------ */

static struct DevNameList dev_names;
static struct UnitList    unit_list;
static char               manual_devname[64];
/* Holds "FILE:<path>" form for an image-file backend chosen via "Use Image". */
static char               image_devname[256];
/* Plain path (without "FILE:" prefix), used for existence checks and creation. */
static char               image_path[256];

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
 * Fill *lst / nodes[] from dev_names, applying filter when show_all=FALSE.
 * map[display_index] = dev_names index (so selection can be translated back).
 * Returns number of entries added.
 */
static UWORD build_name_list(struct List *lst, struct Node *nodes,
                              WORD *map, BOOL show_all)
{
    UWORD i, count = 0;
    list_init(lst);
    for (i = 0; i < dev_names.count; i++) {
        if (!show_all && !is_storage_device(dev_names.names[i])) continue;
        nodes[count].ln_Name = dev_names.display[i];
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
/*                                                                     */
/* Returns index into dev_names.names[], or -1 on Quit / close.       */
/* ------------------------------------------------------------------ */

#define RESULT_EXIT (-2)   /* close-window confirmed: exit program */

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

static WORD run_devname_window(void)
{
    struct Screen  *scr       = NULL;
    APTR            vi        = NULL;
    struct Gadget  *glist     = NULL;
    struct Gadget  *gctx      = NULL;
    struct Gadget  *lv_gad    = NULL;
    struct Gadget  *showall_gad = NULL;
    struct Gadget  *str_gad   = NULL;
    struct Window  *win       = NULL;
    WORD            sel       = -1;
    WORD            result    = -1;
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

    display_count = build_name_list(&name_list, name_nodes, sel_map, show_all);

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
            DevNameList_FormatDisplay(&dev_names, cols);
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
        /* Four buttons: [Select] [Show All] [Use Image] [Quit] */
        UWORD btn_w   = (inner_w - pad * 2 - pad * 3) / 4;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            struct TagItem bt[]      = { { TAG_DONE, 0 } };
            struct TagItem lv_tags[] = {
                { GTLV_Labels, (ULONG)&name_list },
                { TAG_DONE,    0                 }
            };
            struct Gadget *prev;

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

            ng.ng_LeftEdge   = bor_l + pad + (btn_w + pad) * 2;
            ng.ng_GadgetText = GS(MSG_MAIN_USE_IMAGE);
            ng.ng_GadgetID   = GID_USEIMAGE;
            prev = CreateGadgetA(BUTTON_KIND, showall_gad, &ng, bt);
            if (!prev) goto cleanup;

            ng.ng_LeftEdge   = bor_l + pad + (btn_w + pad) * 3;
            ng.ng_GadgetText = GS(MSG_MAIN_QUIT);
            ng.ng_GadgetID   = GID_QUIT;
            prev = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
            if (!prev) goto cleanup;
        }

        sprintf(dev_title, "%s%s", DISKPART_VERTITLE,
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
                UWORD          qual   = imsg->Qualifier;
                struct Gadget *gad    = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (iclass) {
                case IDCMP_CLOSEWINDOW:
                    if (confirm_exit(win)) { result = RESULT_EXIT; running = FALSE; }
                    break;
                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == GID_LIST)
                        sel = (WORD)code;
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GID_LIST:
                        sel = (WORD)code;
                        if (qual & IEQUALIFIER_DOUBLECLICK) do_select = TRUE;
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
                        display_count = build_name_list(&name_list, name_nodes,
                                                        sel_map, show_all);
                        reattach[0].ti_Data = (ULONG)&name_list;
                        GT_SetGadgetAttrsA(lv_gad, win, NULL, reattach);
                        relabel[0].ti_Data  = (ULONG)(show_all ? GS(MSG_MAIN_FILTER) : GS(MSG_MAIN_SHOWALL));
                        GT_SetGadgetAttrsA(showall_gad, win, NULL, relabel);
                        RefreshGList(showall_gad, win, NULL, 1);
                        sel = -1;
                        break;
                    }
                    case GID_USEIMAGE:
                        result  = RESULT_IMAGE;
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
                            strncpy(manual_devname, typed, 63);
                            manual_devname[63] = '\0';
                            result  = RESULT_MANUAL;
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
        sprintf(buf, GS(MSG_MAIN_TESTING_UNIT_FMT), (unsigned long)unit);
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
        sprintf(buf, GS(MSG_MAIN_UNIT_FMT), (unsigned long)unit,
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
        int n = sprintf(pw->title, "%s", DISKPART_VERTITLE);
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
        sprintf(hdr, GS(MSG_MAIN_DEVICE_FMT), devname);
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

/* ------------------------------------------------------------------ */
/* Level-2 window: choose a unit for devname                           */
/*                                                                     */
/* Returns index into unit_list.entries[], or -1 on Back / close.     */
/* ------------------------------------------------------------------ */

static WORD run_unitsel_window(const char *devname)
{
    struct Screen  *scr    = NULL;
    APTR            vi     = NULL;
    struct Gadget  *glist  = NULL;
    struct Gadget  *gctx   = NULL;
    struct Window  *win    = NULL;
    WORD            sel    = -1;
    WORD            result = -1;
    static char     win_title[80];

    struct Node unit_nodes[MAX_KNOWN_DEVICES];
    struct List ulist;
    UWORD i;

    list_init(&ulist);
    for (i = 0; i < unit_list.count; i++) {
        unit_nodes[i].ln_Name = unit_list.entries[i].display;
        unit_nodes[i].ln_Type = NT_USER;
        unit_nodes[i].ln_Pri  = 0;
        AddTail(&ulist, &unit_nodes[i]);
    }

    {
        int n = sprintf(win_title, "%s", DISKPART_VERTITLE);
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
            UWORD want_rows = (unit_list.count > 4) ? (UWORD)unit_list.count : 4;
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
                    { GTLV_Labels, (ULONG)&ulist },
                    { TAG_DONE,    0              }
                };
                struct Gadget *lv_gad =
                    CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lv_tags);
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
                UWORD          qual   = imsg->Qualifier;
                struct Gadget *gad    = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (iclass) {
                case IDCMP_CLOSEWINDOW:
                    if (confirm_exit(win)) { result = RESULT_EXIT; running = FALSE; }
                    break;
                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == GID_LIST)
                        sel = (WORD)code;
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GID_LIST:
                        sel = (WORD)code;
                        if (qual & IEQUALIFIER_DOUBLECLICK) do_select = TRUE;
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
                    if (sel >= 0 && sel < (WORD)unit_list.count) {
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

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Image-file backend: picker + size dialog + create-if-missing       */
/* ------------------------------------------------------------------ */

/* Parse a size string like "100M", "2G", "536870912". */
static UQUAD parse_size_bytes_gui(const char *s)
{
    UQUAD val = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') val = val * 10 + (UQUAD)(*s++ - '0');
    if      (*s == 'K' || *s == 'k') val *= 1024UL;
    else if (*s == 'M' || *s == 'm') val *= 1024UL * 1024UL;
    else if (*s == 'G' || *s == 'g') val *= 1024UL * 1024UL * 1024UL;
    return val;
}

/* Open ASL file requester in DoSaveMode (so the user may type a name that
 * doesn't yet exist). Joins fr_Drawer + fr_File into out (size outsz).
 * Returns TRUE if the user picked a path. */
static BOOL pick_image_path(char *out, ULONG outsz)
{
    struct FileRequester *fr;
    BOOL chosen = FALSE;

    if (!AslBase) {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_INFO_TITLE);
        es.es_TextFormat   = (UBYTE *)GS(MSG_MAIN_ASL_UNAVAIL);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequestArgs(NULL, &es, NULL, NULL);
        return FALSE;
    }

    {
        struct TagItem asl_tags[] = {
            { ASLFR_TitleText,    (ULONG)GS(MSG_MAIN_ASL_TITLE) },
            { ASLFR_DoSaveMode,   TRUE },
            { ASLFR_InitialDrawer,(ULONG)"" },
            { ASLFR_InitialFile,  (ULONG)"" },
            { TAG_DONE, 0 }
        };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, asl_tags);
    }
    if (!fr) return FALSE;

    if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
        strncpy(out, fr->fr_Drawer ? fr->fr_Drawer : "", outsz - 1);
        out[outsz - 1] = '\0';
        AddPart((UBYTE *)out, (UBYTE *)fr->fr_File, outsz);
        chosen = TRUE;
    }
    FreeAslRequest(fr);
    return chosen;
}

/* Returns TRUE if path refers to an existing file (not a directory). */
static BOOL file_exists(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock) return FALSE;
    {
        struct FileInfoBlock *fib =
            (struct FileInfoBlock *)AllocVec(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR);
        BOOL is_file = FALSE;
        if (fib) {
            if (Examine(lock, fib))
                is_file = (fib->fib_DirEntryType < 0);
            FreeVec(fib);
        }
        UnLock(lock);
        return is_file;
    }
}

/* Helper for image_size_dialog: fetch the font baseline from the open window. */
static UWORD scr_font_baseline(struct Window *win)
{
    if (win && win->RPort && win->RPort->Font)
        return win->RPort->Font->tf_Baseline;
    return 8;
}

/* Modal dialog: asks for an image-file size. Pre-fills "100M".
 * Returns TRUE and writes the typed string into out (size outsz)
 * when the user clicks OK. Returns FALSE on Cancel / close. */
static BOOL image_size_dialog(const char *path, char *out, ULONG outsz)
{
    struct Screen  *scr   = NULL;
    APTR            vi    = NULL;
    struct Gadget  *glist = NULL, *gctx = NULL, *str_gad = NULL, *prev;
    struct Window  *win   = NULL;
    BOOL            ok    = FALSE;
    static char     prompt[300];

    sprintf(prompt, GS(MSG_MAIN_IMG_NOEXIST_FMT), path);

    scr = LockPubScreen(NULL);
    if (!scr) return FALSE;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) { UnlockPubScreen(NULL, scr); return FALSE; }

    {
        UWORD font_h = scr->Font->ta_YSize;
        UWORD font_x = scr->RastPort.Font ? (UWORD)scr->RastPort.Font->tf_XSize : 8;
        UWORD bor_l  = (UWORD)scr->WBorLeft;
        UWORD bor_t  = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r  = (UWORD)scr->WBorRight;
        UWORD bor_b  = (UWORD)scr->WBorBottom;
        UWORD pad    = 6;
        UWORD btn_h  = font_h + 6;
        UWORD lbl_h  = font_h + 2;
        UWORD prompt_h = lbl_h * 2;       /* two-line prompt */
        UWORD win_w  = 480;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD str_y  = bor_t + pad + prompt_h + pad;
        UWORD btn_y  = str_y + btn_h + pad;
        UWORD win_h  = btn_y + btn_h + pad + bor_b;
        UWORD btn_w  = (inner_w - pad * 2 - pad) / 2;

        gctx = CreateContext(&glist);
        if (!gctx) goto done;

        {
            struct NewGadget ng;
            struct TagItem bt[]       = { { TAG_DONE, 0 } };
            struct TagItem str_tags[] = {
                { GTST_MaxChars, 31 },
                { GTST_String,   (ULONG)"100M" },
                { TAG_DONE,      0 }
            };
            (void)font_x;

            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

            ng.ng_LeftEdge   = bor_l + pad;
            ng.ng_TopEdge    = str_y;
            ng.ng_Width      = inner_w - pad * 2;
            ng.ng_Height     = btn_h;
            ng.ng_GadgetText = GS(MSG_MAIN_SIZE_PROMPT);
            ng.ng_GadgetID   = GID_SZ_STR;
            ng.ng_Flags      = PLACETEXT_ABOVE;
            str_gad = CreateGadgetA(STRING_KIND, gctx, &ng, str_tags);
            if (!str_gad) goto done;
            ng.ng_Flags = 0;

            ng.ng_TopEdge    = btn_y;
            ng.ng_Height     = btn_h;
            ng.ng_Width      = btn_w;

            ng.ng_LeftEdge   = bor_l + pad;
            ng.ng_GadgetText = GS(MSG_MAIN_CREATE);
            ng.ng_GadgetID   = GID_SZ_OK;
            prev = CreateGadgetA(BUTTON_KIND, str_gad, &ng, bt);
            if (!prev) goto done;

            ng.ng_LeftEdge   = bor_l + pad + btn_w + pad;
            ng.ng_GadgetText = GS(MSG_CANCEL);
            ng.ng_GadgetID   = GID_SZ_CANC;
            prev = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
            if (!prev) goto done;
        }

        {
            struct TagItem win_tags[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width,     win_w },
                { WA_Height,    win_h },
                { WA_Title,     (ULONG)GS(MSG_MAIN_NEW_IMAGE_TITLE) },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                WFLG_SIMPLE_REFRESH },
                { TAG_DONE,     0 }
            };
            win = OpenWindowTagList(NULL, win_tags);
        }

        UnlockPubScreen(NULL, scr);
        scr = NULL;
        if (!win) goto done;

        GT_RefreshWindow(win, NULL);

        /* Draw the prompt text inside the window above the string gadget. */
        {
            const char *p = prompt;
            UWORD       y = bor_t + pad + scr_font_baseline(win);
            UWORD       x = bor_l + pad;
            char        line[160];
            UWORD       li;
            SetAPen(win->RPort, 1);
            while (*p) {
                li = 0;
                while (*p && *p != '\n' && li < sizeof(line) - 1)
                    line[li++] = *p++;
                line[li] = '\0';
                if (*p == '\n') p++;
                Move(win->RPort, x, y);
                Text(win->RPort, line, strlen(line));
                y += lbl_h;
            }
        }

        {
            BOOL running = TRUE;
            while (running) {
                struct IntuiMessage *imsg;
                WaitPort(win->UserPort);
                while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                    ULONG          iclass = imsg->Class;
                    struct Gadget *gad    = (struct Gadget *)imsg->IAddress;
                    GT_ReplyIMsg(imsg);
                    switch (iclass) {
                    case IDCMP_CLOSEWINDOW:
                        running = FALSE;
                        break;
                    case IDCMP_GADGETUP:
                        switch (gad->GadgetID) {
                        case GID_SZ_OK:
                        case GID_SZ_STR:    /* Enter pressed in string gadget */
                        {
                            const char *typed =
                                ((struct StringInfo *)str_gad->SpecialInfo)->Buffer;
                            strncpy(out, typed, outsz - 1);
                            out[outsz - 1] = '\0';
                            ok = TRUE;
                            running = FALSE;
                            break;
                        }
                        case GID_SZ_CANC:
                            running = FALSE;
                            break;
                        }
                        break;
                    case IDCMP_REFRESHWINDOW:
                        GT_BeginRefresh(win);
                        GT_EndRefresh(win, TRUE);
                        break;
                    }
                }
            }
        }
    }

done:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return ok;
}

/* If path doesn't exist, ask the user for a size and create the file.
 * Returns TRUE if path is now ready to open as an image, FALSE on
 * cancel or any error. */
static BOOL prepare_image(const char *path)
{
    char  size_str[32];
    UQUAD size_bytes;
    struct BlockDev *bd;

    if (file_exists(path)) return TRUE;

    if (!image_size_dialog(path, size_str, sizeof(size_str)))
        return FALSE;

    size_bytes = parse_size_bytes_gui(size_str);
    if (size_bytes < 512) {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_INFO_TITLE);
        es.es_TextFormat   = (UBYTE *)GS(MSG_MAIN_SIZE_MIN);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequestArgs(NULL, &es, NULL, NULL);
        return FALSE;
    }
    /* dos.library Seek is signed 32-bit. */
    if (size_bytes > (UQUAD)0x7FFFFE00UL) {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_INFO_TITLE);
        es.es_TextFormat   = (UBYTE *)GS(MSG_MAIN_SIZE_MAX);
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequestArgs(NULL, &es, NULL, NULL);
        return FALSE;
    }

    bd = BlockDev_CreateFile(path, size_bytes);
    if (!bd) {
        struct EasyStruct es;
        static char       body[300];
        sprintf(body, GS(MSG_MAIN_IMG_CREATE_FAIL_FMT), path);
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)GS(MSG_INFO_TITLE);
        es.es_TextFormat   = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequestArgs(NULL, &es, NULL, NULL);
        return FALSE;
    }
    BlockDev_Close(bd);
    return TRUE;
}

int main(void)
{
    int result = 0;

    /* Must be first: SysBase lives at AbsExecBase (address 4) */
    SysBase = *((struct ExecBase **)4UL);

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    if (!DOSBase) goto cleanup;

    /* Localization: opens locale.library (v38+) + DiskPart.catalog when
     * present.  No-op on Kickstart 2.04 (v37) - GS() then falls back to the
     * built-in English strings.  Opened before CLI dispatch so CLI/script
     * messages are localized too. */
    LocaleOpen();

    /* Opened before the CLI dispatch below so quick-format works in CLI/script
       mode too (a ROM library; not fatal if absent). */
    ExpansionBase = OpenLibrary("expansion.library", 37);

    /* CLI launch with arguments -> CLI mode (no GUI libs needed). */
    {
        struct Process *proc = (struct Process *)FindTask(NULL);
        if (proc->pr_CLI) {
            LONG cli_rc = cli_run();
            if (cli_rc != CLI_NO_ARGS) {
                result = (int)cli_rc;
                goto cleanup;
            }
            /* CLI_NO_ARGS: empty command line, fall through to GUI. */
        }
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase) goto cleanup;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    if (!GfxBase) goto cleanup;

    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (!GadToolsBase) goto cleanup;

    AslBase = OpenLibrary("asl.library", 37);
    /* Not fatal - file requester simply won't be available */

    IconBase = OpenLibrary("icon.library", 37);
    /* Not fatal - only used to read the NOWARNING tooltype from our icon */

    /* Suppress the startup warning if NOWARNING was passed on the CLI
     * or set as a tooltype on the program icon (Workbench launch). */
    {
        BOOL skip_warning = cli_nowarning();

        if (!skip_warning && IconBase && DiskPart_WBStartup &&
            DiskPart_WBStartup->sm_NumArgs >= 1) {
            struct WBArg    *wa  = &DiskPart_WBStartup->sm_ArgList[0];
            struct DiskObject *dobj;
            BPTR             prev_dir;

            prev_dir = CurrentDir(wa->wa_Lock);
            dobj = GetDiskObject((STRPTR)wa->wa_Name);
            if (dobj) {
                if (FindToolType((STRPTR *)dobj->do_ToolTypes,
                                 (STRPTR)"NOWARNING"))
                    skip_warning = TRUE;
                FreeDiskObject(dobj);
            }
            CurrentDir(prev_dir);
        }

        if (!skip_warning) {
            struct EasyStruct es;
            char body[512];
            sprintf(body, GS(MSG_MAIN_WARN_BODY),
                DISKPART_VERSION, DiskPart_BuildStamp);

            es.es_StructSize   = sizeof(es);
            es.es_Flags        = 0;
            es.es_Title        = (UBYTE *)GS(MSG_MAIN_WARN_TITLE);
            es.es_TextFormat   = (UBYTE *)body;
            es.es_GadgetFormat = (UBYTE *)GS(MSG_MAIN_WARN_GADGETS);
            if (EasyRequestArgs(NULL, &es, NULL, NULL) != 1)
                goto cleanup;
        }
    }

    /* Scan for block device driver names - instant, no I/O */
    Devices_Scan(&dev_names);

    /* Navigation: driver name -> unit -> partition editor */
    {
        WORD name_idx;
        while ((name_idx = run_devname_window()) != -1 &&
               name_idx != RESULT_EXIT) {
            const char *devname;
            WORD unit_idx;
            BOOL quit = FALSE;

            /* Image-file backend - skip device probe and unit selection;
             * after the editor closes, return to the device-selection window. */
            if (name_idx == RESULT_IMAGE) {
                if (!pick_image_path(image_path, sizeof(image_path)))
                    continue;
                if (!prepare_image(image_path))
                    continue;
                sprintf(image_devname, "FILE:%s", image_path);
                if (partview_run(image_devname, 0))
                    break;
                continue;
            }

            devname = (name_idx == RESULT_MANUAL)
                      ? manual_devname
                      : dev_names.names[name_idx];

            {
                static struct ProbeWin pw;
                probe_win_open(&pw, devname);
                Devices_GetUnitsForName(devname, &unit_list,
                                        probe_win_cb, &pw);
                probe_win_close(&pw);
            }
            if (unit_list.count == 0) continue;

            while (!quit && (unit_idx = run_unitsel_window(devname)) >= 0) {
                if (partview_run(devname, unit_list.entries[unit_idx].unit))
                    quit = TRUE;
            }
            if (quit || unit_idx == RESULT_EXIT) break;
        }
    }

cleanup:
    if (ExpansionBase) CloseLibrary(ExpansionBase);
    if (IconBase)      CloseLibrary(IconBase);
    if (AslBase)       CloseLibrary(AslBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    LocaleClose();
    if (DOSBase)       CloseLibrary((struct Library *)DOSBase);

    return result;
}
