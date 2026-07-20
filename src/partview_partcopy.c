/*
 * partview_partcopy.c - GUI "Copy Partition to Another Disk" (Advanced menu).
 *
 * Reopens the two-level device/unit selector (diskselect.h) to pick a
 * destination disk, lists that disk's partitions in a GadTools listview,
 * and clones the source partition onto the chosen destination partition
 * with partclone.c's PartClone_PartToPart (block copy + geometry adopt +
 * SFS offset fixup), then writes the destination RDB.
 *
 * Local DevNameList/UnitList storage, per diskselect.h's header comment.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>

#include "clib.h"
#include "rdb.h"
#include "devices.h"
#include "diskselect.h"
#include "partclone.h"
#include "progresswin.h"
#include "guilv.h"
#include "partview_internal.h"
#include "locale_support.h"

#define PCP_GID_LIST   1
#define PCP_GID_SELECT 2
#define PCP_GID_CANCEL 3

/* Sentinel index returned by the picker for the "create new partition" row. */
#define PCP_CREATE_NEW  0x7FFF

/* ---- listview render hook: persistent selection highlight ----
   A plain GadTools listview only shows LVR_SELECTED while the button is
   held (reverts on release, every ROM), so the chosen row never stays
   marked.  Same fix as diskselect.c: a module-level selected index + a
   COMPLEMENT overlay in a custom GTLV_CallBack render hook, and
   gui_force_lv_redraw() to re-run the hook after a click. */
static WORD  pcp_sel_idx = -1;
static struct Node *pcp_nodes_base;   /* h_Data: the node array in use */

static ULONG pcp_lv_render(void)
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
    struct RastPort *rp;
    struct Rectangle *b;
    BOOL sel;
    UWORD bg_pen, fg_pen, len;
    const char *name;
    WORD idx;

    if (msg->lvdm_MethodID != LV_DRAW) return LVCB_OK;
    rp = msg->lvdm_RastPort;
    b  = &msg->lvdm_Bounds;
    idx = (WORD)(node - (struct Node *)h->h_Data);
    sel = (msg->lvdm_State == LVR_SELECTED ||
           msg->lvdm_State == LVR_SELECTEDDISABLED) ||
          (idx == pcp_sel_idx);
    bg_pen = (UWORD)msg->lvdm_DrawInfo->dri_Pens[BACKGROUNDPEN];
    fg_pen = (UWORD)msg->lvdm_DrawInfo->dri_Pens[TEXTPEN];
    SetAPen(rp, (LONG)bg_pen); SetDrMd(rp, JAM2);
    RectFill(rp, b->MinX, b->MinY, b->MaxX, b->MaxY);
    name = node->ln_Name ? node->ln_Name : "";
    len  = (UWORD)strlen(name);
    SetAPen(rp, (LONG)fg_pen); SetDrMd(rp, JAM1);
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

static struct Hook pcp_lv_hook;

/* ---- destination disk picker (same flow as partview_diskcopy.c) ---- */
static BOOL pcp_devname_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return FALSE;
        a++; b++;
    }
    return (BOOL)(*a == '\0' && *b == '\0');
}

static BOOL pcp_pick_dest_disk(char *devname_out, ULONG *unit_out)
{
    static struct DevNameList dn;
    static struct UnitList    ul;
    static char               manual[64];
    WORD name_idx;

    Devices_Scan(&dn);
    for (;;) {
        const char *devname;
        WORD        unit_idx;
        name_idx = DiskSelect_PickDeviceName(&dn, manual, FALSE);
        if (name_idx == -1 || name_idx == DISKSEL_EXIT) return FALSE;
        devname = (name_idx == DISKSEL_MANUAL) ? manual : dn.names[name_idx];
        if (!DiskSelect_ProbeUnits(devname, &ul)) continue;
        unit_idx = DiskSelect_PickUnit(devname, &ul);
        if (unit_idx == DISKSEL_EXIT) return FALSE;
        if (unit_idx < 0) continue;
        strncpy(devname_out, devname, 63); devname_out[63] = '\0';
        *unit_out = ul.entries[unit_idx].unit;
        return TRUE;
    }
}

static void pcp_msg(struct Window *win, const char *title, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof(es); es.es_Flags = 0;
    es.es_Title = (UBYTE *)title; es.es_TextFormat = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
    EasyRequest(win, &es, NULL);
}

/* ---- destination-partition list picker ---- */
/* Returns the selected partition index in drdb, or -1 on cancel. */
static WORD pcp_pick_partition(struct Window *parent, struct RDBInfo *drdb,
                               const char *devname)
{
    struct Screen *scr;
    APTR   vi = NULL;
    struct Gadget *glist = NULL, *gctx;
    struct Window *win = NULL;
    static char rows[MAX_PARTITIONS + 1][64];
    static struct Node nodes[MAX_PARTITIONS + 1];
    struct List list;
    static char title[80];
    WORD result = -1;
    UWORD i, nrows;

    pcp_sel_idx = -1;

    /* manual exec list init (NewList is inline-only in some NDKs) */
    list.lh_Head     = (struct Node *)&list.lh_Tail;
    list.lh_Tail     = NULL;
    list.lh_TailPred = (struct Node *)&list.lh_Head;
    for (i = 0; i < drdb->num_parts; i++) {
        struct PartInfo *p = &drdb->parts[i];
        char dt[16], sz[20];
        ULONG hh = p->heads   > 0 ? p->heads   : drdb->heads;
        ULONG ss = p->sectors > 0 ? p->sectors : drdb->sectors;
        FormatDosType(p->dos_type, dt);
        FormatSize((UQUAD)(p->high_cyl - p->low_cyl + 1) * hh * ss * 512UL, sz);
        DP_SNPRINTF(rows[i], "%-10s %-8s %s", p->drive_name, dt, sz);
        nodes[i].ln_Name = rows[i];
        nodes[i].ln_Type = NT_USER; nodes[i].ln_Pri = 0;
        AddTail(&list, &nodes[i]);
    }
    /* trailing "create a new partition" row */
    strncpy(rows[drdb->num_parts], GS(MSG_PCP_NEW_ENTRY), 63);
    rows[drdb->num_parts][63] = '\0';
    nodes[drdb->num_parts].ln_Name = rows[drdb->num_parts];
    nodes[drdb->num_parts].ln_Type = NT_USER; nodes[drdb->num_parts].ln_Pri = 0;
    AddTail(&list, &nodes[drdb->num_parts]);
    nrows = drdb->num_parts + 1;
    pcp_nodes_base = nodes;
    pcp_lv_hook.h_Entry    = (HOOKFUNC)pcp_lv_render;
    pcp_lv_hook.h_SubEntry = NULL;
    pcp_lv_hook.h_Data     = (APTR)nodes;
    DP_SNPRINTF(title, GS(MSG_PCP_PICK_TITLE_FMT), devname);

    scr = LockPubScreen(NULL);
    if (!scr) return -1;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) { UnlockPubScreen(NULL, scr); return -1; }

    {
        UWORD font_h = scr->Font->ta_YSize;
        UWORD bor_l = (UWORD)scr->WBorLeft;
        UWORD bor_t = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r = (UWORD)scr->WBorRight;
        UWORD bor_b = (UWORD)scr->WBorBottom;
        UWORD win_w = 460, pad = 4;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD btn_h = font_h + 6;
        UWORD row_h = (UWORD)(font_h + 2);
        UWORD rows_shown = (nrows > 4) ? nrows : 4;
        UWORD lv_h = row_h * rows_shown;
        UWORD win_h = bor_t + pad + lv_h + pad + btn_h + pad + bor_b;
        struct NewGadget ng;
        struct Gadget *lv_gad, *sel_gad, *cancel_gad;
        struct TagItem lv_tags[] = {
            { GTLV_Labels,   (ULONG)&list },
            { GTLV_CallBack, (ULONG)&pcp_lv_hook },
            { TAG_DONE, 0 }
        };
        struct TagItem bt[] = { { TAG_DONE, 0 } };

        gctx = CreateContext(&glist);
        if (!gctx) { UnlockPubScreen(NULL, scr); return -1; }

        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi; ng.ng_TextAttr = scr->Font;
        ng.ng_LeftEdge = bor_l + pad; ng.ng_TopEdge = bor_t + pad;
        ng.ng_Width = inner_w - pad * 2; ng.ng_Height = lv_h;
        ng.ng_GadgetID = PCP_GID_LIST;
        lv_gad = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lv_tags);
        if (!lv_gad) { UnlockPubScreen(NULL, scr); FreeGadgets(glist); return -1; }

        {
            UWORD btn_y = bor_t + pad + lv_h + pad;
            UWORD half_w = (inner_w - pad * 2 - pad) / 2;
            ng.ng_TopEdge = btn_y; ng.ng_Height = btn_h; ng.ng_Width = half_w;
            ng.ng_LeftEdge = bor_l + pad;
            ng.ng_GadgetText = GS(MSG_PCP_CLONE_HERE);
            ng.ng_GadgetID = PCP_GID_SELECT;
            sel_gad = CreateGadgetA(BUTTON_KIND, lv_gad, &ng, bt);
            ng.ng_LeftEdge = bor_l + pad + half_w + pad;
            ng.ng_GadgetText = GS(MSG_MAIN_BACK);
            ng.ng_GadgetID = PCP_GID_CANCEL;
            cancel_gad = CreateGadgetA(BUTTON_KIND, sel_gad ? sel_gad : lv_gad, &ng, bt);
            (void)cancel_gad;
        }

        {
            struct TagItem win_tags[] = {
                { WA_Left, (ULONG)((scr->Width - win_w) / 2) },
                { WA_Top,  (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width, win_w }, { WA_Height, win_h },
                { WA_Title, (ULONG)title }, { WA_Gadgets, (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                            IDCMP_GADGETDOWN | IDCMP_REFRESHWINDOW },
                { WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                            WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, win_tags);
        }
    }
    UnlockPubScreen(NULL, scr);
    if (!win) { FreeGadgets(glist); return -1; }
    GT_RefreshWindow(win, NULL);

    {
        BOOL running = TRUE;
        WORD sel = -1;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW:
                    running = FALSE; break;
                case IDCMP_GADGETDOWN:
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == PCP_GID_LIST) {
                        sel = (WORD)code;
                        pcp_sel_idx = sel;            /* persist the highlight */
                        gui_force_lv_redraw(gad, win, &list);
                    } else if (gad->GadgetID == PCP_GID_SELECT) {
                        if (sel >= 0 && sel < (WORD)nrows)
                            result = (sel == (WORD)drdb->num_parts)
                                     ? PCP_CREATE_NEW : sel;
                        running = FALSE;
                    } else if (gad->GadgetID == PCP_GID_CANCEL) {
                        running = FALSE;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }
    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    return result;
}

/* ---- progress adapter ---- */
static void pcp_progress(void *ud, ULONG done, ULONG total, const char *phase)
{
    (void)phase;
    ProgressWin_Callback(ud, done, total);
}

BOOL copy_partition_to_disk(struct Window *win, struct BlockDev *bd,
                            struct RDBInfo *rdb, struct PartInfo *src,
                            const char *cur_devname, ULONG cur_unit)
{
    char   dest_devname[64];
    ULONG  dest_unit;
    struct BlockDev *dest = NULL;
    static struct RDBInfo drdb;   /* large - keep out of the stack */
    WORD   dpi_idx;
    struct PartInfo *dpi;
    char   body[400], err[128];

    if (!src) { pcp_msg(win, GS(MSG_PCP_TITLE), GS(MSG_PV_PC_NO_SEL)); return FALSE; }
    if (!bd)  { pcp_msg(win, GS(MSG_PCP_TITLE), GS(MSG_IMG_DEV_NOT_ACCESSIBLE)); return FALSE; }

    if (!pcp_pick_dest_disk(dest_devname, &dest_unit)) return FALSE;

    dest = BlockDev_Open(dest_devname, dest_unit);
    if (!dest) {
        DP_SNPRINTF(body, GS(MSG_DC_OPEN_FAIL_FMT), dest_devname);
        pcp_msg(win, GS(MSG_PCP_TITLE), body);
        return FALSE;
    }
    if (dest->block_size != bd->block_size) {
        pcp_msg(win, GS(MSG_PCP_TITLE), GS(MSG_DC_BLOCKSIZE_MISMATCH));
        BlockDev_Close(dest); return FALSE;
    }
    memset(&drdb, 0, sizeof(drdb));
    if (!RDB_Read(dest, &drdb) || !drdb.valid) {
        pcp_msg(win, GS(MSG_PCP_TITLE), GS(MSG_PCP_DEST_NO_RDB));
        BlockDev_Close(dest); return FALSE;
    }


    if (!src->heads)   src->heads   = rdb->heads;
    if (!src->sectors) src->sectors = rdb->sectors;

    dpi_idx = pcp_pick_partition(win, &drdb, dest_devname);
    if (dpi_idx < 0) { RDB_FreeCode(&drdb); BlockDev_Close(dest); return FALSE; }

    if (dpi_idx == PCP_CREATE_NEW) {
        /* Create a partition in the DESTINATION disk's geometry, sized to
           hold the source's blocks (rounded up to a cylinder), in the first
           free gap, auto-named, added to the destination RDB.  The engine
           keeps this footprint and drops the source's blocks in. */
        ULONG sh    = src->heads   > 0 ? src->heads   : rdb->heads;
        ULONG ss    = src->sectors > 0 ? src->sectors : rdb->sectors;
        ULONG spb   = (src->block_size >= 1024) ? (src->block_size / 512) : 1;
        ULONG src_dev = (src->high_cyl - src->low_cyl + 1) * sh * ss * spb;
        ULONG dbpc  = drdb.heads * drdb.sectors * spb;
        ULONG cyl_span = dbpc ? (src_dev + dbpc - 1) / dbpc : 0;
        ULONG low;
        char  nm[8];
        UWORD n;

        if (drdb.num_parts >= MAX_PARTITIONS || cyl_span == 0) {
            pcp_msg(win, GS(MSG_PCP_TITLE), GS(MSG_PCP_DEST_NO_PARTS));
            RDB_FreeCode(&drdb); BlockDev_Close(dest); return FALSE;
        }
        if (!PartClone_FindGap(&drdb, cyl_span, drdb.heads, drdb.sectors, &low)) {
            DP_SNPRINTF(body, GS(MSG_PC_NOGAP_FMT), (unsigned long)cyl_span);
            pcp_msg(win, GS(MSG_PCP_TITLE), body);
            RDB_FreeCode(&drdb); BlockDev_Close(dest); return FALSE;
        }
        dpi = &drdb.parts[drdb.num_parts];
        memset(dpi, 0, sizeof(*dpi));
        for (n = 0; n < 100; n++) {
            UWORD j; BOOL taken = FALSE;
            DP_SNPRINTF(nm, "DH%lu", (unsigned long)n);
            for (j = 0; j < drdb.num_parts; j++)
                if (pcp_devname_eq_ci(drdb.parts[j].drive_name, nm)) { taken = TRUE; break; }
            if (!taken) break;
        }
        strncpy(dpi->drive_name, nm, sizeof(dpi->drive_name) - 1);
        dpi->low_cyl    = low;
        dpi->high_cyl   = low + cyl_span - 1;
        dpi->heads      = drdb.heads;          /* destination disk geometry */
        dpi->sectors    = drdb.sectors;
        dpi->block_size = src->block_size;     /* match source's FS blocks   */
        dpi->dos_type   = src->dos_type;
        dpi->sectors_per_block = src->sectors_per_block;
        dpi->reserved_blks     = src->reserved_blks;
        dpi->mask       = 0x7FFFFFFEUL;
        dpi->max_transfer = 0x0FFFFFFEUL;
        dpi->num_buffer = 30;
        if (!PartClone_ValidateFootprint(&drdb, dpi->low_cyl, dpi->high_cyl,
                                         dpi->heads, dpi->sectors, -1,
                                         err, sizeof(err))) {
            pcp_msg(win, GS(MSG_PCP_TITLE), err);
            RDB_FreeCode(&drdb); BlockDev_Close(dest); return FALSE;
        }
        drdb.num_parts++;
    } else {
        dpi = &drdb.parts[dpi_idx];
        /* self-clone guard (same disk + same partition) */
        if (pcp_devname_eq_ci(dest_devname, cur_devname) &&
            dest_unit == cur_unit &&
            dpi->low_cyl == src->low_cyl && dpi->high_cyl == src->high_cyl) {
            pcp_msg(win, GS(MSG_PCP_TITLE), GS(MSG_PC_CLONE_SAME));
            RDB_FreeCode(&drdb); BlockDev_Close(dest); return FALSE;
        }
        /* overwrite in place: destination footprint unchanged (the engine
           checks it is large enough / exact-size for FFS). */
    }

    {
        struct EasyStruct es;
        DP_SNPRINTF(body, GS(MSG_PCP_CONFIRM_FMT),
                    src->drive_name, dpi->drive_name, dest_devname,
                    dpi->drive_name);
        es.es_StructSize = sizeof(es); es.es_Flags = 0;
        es.es_Title = (UBYTE *)GS(MSG_PCP_TITLE);
        es.es_TextFormat = (UBYTE *)body;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_PCP_CONFIRM_GADGETS);
        if (EasyRequest(win, &es, NULL) != 1) {
            RDB_FreeCode(&drdb); BlockDev_Close(dest); return FALSE;
        }
    }

    BOOL wrote = FALSE;
    {
        static struct ProgressWin prog;
        BOOL ok;
        DP_SNPRINTF(prog.title, GS(MSG_PCP_PROG_FMT), dpi->drive_name);
        ProgressWin_Open(&prog, prog.title);
        err[0] = '\0';
        ok = PartClone_PartToPart(bd, src, dest, &drdb, dpi,
                                  pcp_progress, &prog, err, sizeof(err));
        if (ok) {
            if (!RDB_Write(dest, &drdb)) {
                ok = FALSE;
                strncpy(err, "RDB write failed", sizeof(err) - 1);
            } else {
                wrote = TRUE;
            }
        }
        ProgressWin_Close(&prog);
        if (ok && dpi_idx == PCP_CREATE_NEW)
            DP_SNPRINTF(body, GS(MSG_PCP_NEW_OK_FMT), dpi->drive_name);
        else if (ok)
            DP_SNPRINTF(body, GS(MSG_PCP_OK_FMT),
                        src->drive_name, dpi->drive_name);
        else
            DP_SNPRINTF(body, GS(MSG_PCP_FAIL_FMT), err);
        pcp_msg(win, GS(MSG_PCP_TITLE), body);
    }

    RDB_FreeCode(&drdb);
    BlockDev_Close(dest);
    return wrote;
}
