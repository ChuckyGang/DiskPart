/*
 * main.c — DiskPart two-level device selection.
 *
 * Level 1: list of exec device driver names that responded to probing.
 * Level 2: list of units for the chosen driver, showing disk name/size.
 * Level 3: partition editor (partview.c).
 *
 * AmigaOS 3.1+ (Kickstart v37+), m68k-amiga-elf-gcc (Bartman toolchain).
 * GadTools UI — no MUI, no external library dependencies.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <devices/inputevent.h>
#ifndef IEQUALIFIER_DOUBLECLICK
#define IEQUALIFIER_DOUBLECLICK 0x8000
#endif
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "devices.h"
#include "partview.h"

/* ------------------------------------------------------------------ */
/* Library bases — SysBase set by main() before any LP call            */
/* ------------------------------------------------------------------ */

struct ExecBase      *SysBase;
struct DosLibrary    *DOSBase        = NULL;
struct IntuitionBase *IntuitionBase  = NULL;
struct GfxBase       *GfxBase        = NULL;
struct Library       *GadToolsBase   = NULL;
struct Library       *AslBase        = NULL;

/* ------------------------------------------------------------------ */
/* Gadget IDs                                                           */
/* ------------------------------------------------------------------ */

#define GID_LIST    1
#define GID_SELECT  2
#define GID_SHOWALL 3
#define GID_QUIT    4
#define GID_MANUAL  5

#define RESULT_MANUAL (-3)

/* ------------------------------------------------------------------ */
/* Static data (too large for stack)                                   */
/* ------------------------------------------------------------------ */

static struct DevNameList dev_names;
static struct UnitList    unit_list;
static char               manual_devname[64];

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
    static const char * const keys[] = { "ide", "scsi", "flash", "usb", "uae", "gvp", "phase5", "ppc", "sdhc", "emmc", "nvme", NULL };
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
    es.es_Title        = (UBYTE *)"DiskPart";
    es.es_TextFormat   = (UBYTE *)"Exit DiskPart?";
    es.es_GadgetFormat = (UBYTE *)"Yes|No";
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
    BOOL            show_all  = FALSE;

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
        UWORD lv_h    = (UWORD)(font_h + 2) * 10;
        UWORD lbl_h   = (UWORD)(font_h + 2);  /* label floats above gadget top */
        UWORD str_y   = bor_t + pad + lv_h + pad + lbl_h; /* room for label above */
        UWORD btn_y   = str_y + btn_h + pad;
        UWORD win_h   = btn_y + btn_h + pad + bor_b;
        /* Three buttons: [Select] [Show All] [Quit] */
        UWORD btn_w   = (inner_w - pad * 2 - pad * 2) / 3;

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
            ng.ng_GadgetText = "Manual device name:";
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
            ng.ng_GadgetText = "Select";
            ng.ng_GadgetID   = GID_SELECT;
            prev = CreateGadgetA(BUTTON_KIND, str_gad, &ng, bt);
            if (!prev) goto cleanup;

            ng.ng_LeftEdge   = bor_l + pad + btn_w + pad;
            ng.ng_GadgetText = "Show All";
            ng.ng_GadgetID   = GID_SHOWALL;
            showall_gad = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
            if (!showall_gad) goto cleanup;

            ng.ng_LeftEdge   = bor_l + pad + (btn_w + pad) * 2;
            ng.ng_GadgetText = "Quit";
            ng.ng_GadgetID   = GID_QUIT;
            prev = CreateGadgetA(BUTTON_KIND, showall_gad, &ng, bt);
            if (!prev) goto cleanup;
        }

        {
            struct TagItem win_tags[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width,     win_w },
                { WA_Height,    win_h },
                { WA_Title,     (ULONG)"DiskPart - Select Device" },
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
                        relabel[0].ti_Data  = (ULONG)(show_all ? "Filter" : "Show All");
                        GT_SetGadgetAttrsA(showall_gad, win, NULL, relabel);
                        RefreshGList(showall_gad, win, NULL, 1);
                        sel = -1;
                        break;
                    }
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
    UWORD  x;           /* left text margin */
    UWORD  baseline;    /* font baseline from top of cell */
    UWORD  line_h;      /* pixels per text row             */
    UWORD  y_status;    /* y of the "Testing unit N" row   */
    UWORD  y_result0;   /* y of first result row           */
    UWORD  results;     /* number of result rows written   */
    char   title[80];   /* Intuition keeps a pointer — must outlive win */
};

static void probe_win_cb(void *ud, ULONG unit, UWORD phase, const char *info)
{
    struct ProbeWin *pw = (struct ProbeWin *)ud;
    struct RastPort *rp;
    char   buf[128];
    WORD   len, pad, i;

    if (!pw->win) return;
    rp = pw->win->RPort;

    switch (phase) {
    case PROBE_START:
        sprintf(buf, "Testing unit %lu...", (unsigned long)unit);
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
        sprintf(buf, "Unit %lu  %s", (unsigned long)unit,
                info ? info : "found");
        SetAPen(rp, 1);
        Move(rp, pw->x, pw->y_result0 + (WORD)pw->results * (WORD)pw->line_h);
        Text(rp, buf, strlen(buf));
        pw->results++;
        break;

    case PROBE_EMPTY:
        /* Nothing — status line already shows "Testing unit N..." */
        break;
    }

    (void)i;  /* suppress unused warning */
}

static void probe_win_open(struct ProbeWin *pw, const char *devname)
{
    struct Screen *scr;
    UWORD fh, bor_l, bor_t, bor_r, bor_b, pad, lh, win_w, win_h, rows;

    memset(pw, 0, sizeof(*pw));
    sprintf(pw->title, "Probing %s", devname);

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
    win_w = 420;
    win_h = bor_t + pad + rows * lh + pad + bor_b;

    {
        struct TagItem win_tags[] = {
            { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
            { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
            { WA_Width,     win_w  },
            { WA_Height,    win_h  },
            { WA_Title,     (ULONG)pw->title },
            { WA_PubScreen, (ULONG)scr },
            { WA_IDCMP,     0 },
            { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH },
            { TAG_DONE,     0 }
        };
        pw->win = OpenWindowTagList(NULL, win_tags);
    }

    UnlockPubScreen(NULL, scr);
    if (!pw->win) return;

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
        sprintf(hdr, "Device: %s", devname);
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
    if (pw->win) { CloseWindow(pw->win); pw->win = NULL; }
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

    sprintf(win_title, "DiskPart - %s", devname);

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
        UWORD lv_h    = (UWORD)(font_h + 2) * 10;
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
                    ng.ng_GadgetText = "Select";
                    ng.ng_GadgetID   = GID_SELECT;
                    sel_gad = CreateGadgetA(BUTTON_KIND, lv_gad, &ng, bt);
                    if (!sel_gad) goto cleanup;

                    ng.ng_LeftEdge   = bor_l + pad + half_w + pad;
                    ng.ng_GadgetText = "Back";
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

int main(void)
{
    /* Must be first: SysBase lives at AbsExecBase (address 4) */
    SysBase = *((struct ExecBase **)4UL);

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    if (!DOSBase) goto cleanup;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase) goto cleanup;

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    if (!GfxBase) goto cleanup;

    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (!GadToolsBase) goto cleanup;

    AslBase = OpenLibrary("asl.library", 37);
    /* Not fatal — file requester simply won't be available */

    /* Startup warning — shown before the slow device scan */
    {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)"DiskPart - WARNING";
        es.es_TextFormat   = (UBYTE *)
            "EXPERIMENTAL SOFTWARE\n\n"
            "DiskPart can modify the partition table\n"
            "of your hard drive or other block device.\n\n"
            "Incorrect use WILL cause permanent data loss.\n\n"
            "Make sure you have a FULL BACKUP of any disk\n"
            "you intend to work with before proceeding.\n\n"
            "The author accepts NO responsibility for\n"
            "any loss of data caused by this software.";
        es.es_GadgetFormat = (UBYTE *)"I have a backup - Continue|Quit";
        if (EasyRequestArgs(NULL, &es, NULL, NULL) != 1)
            goto cleanup;
    }

    /* Scan for block device driver names — instant, no I/O */
    Devices_Scan(&dev_names);

    /* Navigation: driver name → unit → partition editor */
    {
        WORD name_idx;
        while ((name_idx = run_devname_window()) != -1 &&
               name_idx != RESULT_EXIT) {
            const char *devname = (name_idx == RESULT_MANUAL)
                                  ? manual_devname
                                  : dev_names.names[name_idx];
            WORD unit_idx;
            BOOL quit = FALSE;

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
                else if (unit_idx == RESULT_EXIT)
                    quit = TRUE;
            }
            if (quit || unit_idx == RESULT_EXIT) break;
        }
    }

cleanup:
    if (AslBase)       CloseLibrary(AslBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (DOSBase)       CloseLibrary((struct Library *)DOSBase);

    return 0;
}
