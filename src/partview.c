/*
 * partview.c - Partition view window for DiskPart.
 *
 * Layout mirrors AmigaPart:
 *   ┌─ Disk Information ──────────────────────────────────┐
 *   │ Device / Size / Geometry / Model / RDB status        │
 *   ├─ Disk Map ──────────────────────────────────────────┤
 *   │  [RDB] [DH0────────] [DH1────────] [free  ·····]    │
 *   │  Cyl 0            Free: 250 MB           Cyl 1039   │
 *   ├─ Partitions ────────────────────────────────────────┤
 *   │  Drive    Lo Cyl    Hi Cyl  Filesystem       Size Boot │
 *   │  DH0            1       519  FFS          250 MB    0 │
 *   ├─ Buttons ───────────────────────────────────────────┤
 *   │  [Init RDB] [Add] [Edit] [Delete]          [Back]   │
 *   └─────────────────────────────────────────────────────┘
 *
 * Drag resize: click and drag partition edges in the map to resize.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/asl.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <devices/scsidisk.h>
#include <exec/errors.h>
#include <libraries/gadtools.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/icon.h>

#include "clib.h"
#include "guilv.h"
#include "locale_support.h"
#include "rdb.h"
#include "mbr.h"
#include "partview.h"
#include "version.h"
#include "ffsresize.h"
#include "pfsresize.h"
#include "sfsresize.h"
#include "partmove.h"
#include "partview_internal.h"
#include "quickformat.h"


/* ------------------------------------------------------------------ */
/* External library bases (defined in main.c)                          */
/* ------------------------------------------------------------------ */

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct Library       *AslBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *GadToolsBase;
extern struct Library       *IconBase;
extern struct WBStartup     *DiskPart_WBStartup;

/* ------------------------------------------------------------------ */
/* Mouse button codes (from devices/inputevent.h IECODE_* values)     */
/* ------------------------------------------------------------------ */

#ifndef SELECTDOWN
#define SELECTDOWN 0x68
#define SELECTUP   0xE8
#endif

#ifndef IEQUALIFIER_DOUBLECLICK
#define IEQUALIFIER_DOUBLECLICK 0x8000
#endif

/* ------------------------------------------------------------------ */
/* Gadget IDs                                                           */
/* ------------------------------------------------------------------ */

#define GID_PARTLIST  1
#define GID_INITRDB   2
#define GID_ADD       3
#define GID_EDIT      4
#define GID_DELETE    5
#define GID_FILESYS   6
#define GID_WRITE     7
#define GID_BACK      8
#define GID_MOVE      11
#define GID_LASTDISK  9
#define GID_LASTLUN   10

/* ------------------------------------------------------------------ */
/* Partition colours - match AmigaPart COLORS list                     */
/* ------------------------------------------------------------------ */

#define NUM_PART_COLORS 8
static const UBYTE PART_R[NUM_PART_COLORS]={0x4A,0xE6,0x27,0x8E,0xE7,0x16,0xF3,0x29};
static const UBYTE PART_G[NUM_PART_COLORS]={0x90,0x7E,0xAE,0x44,0x4C,0xA0,0x9C,0x80};
static const UBYTE PART_B[NUM_PART_COLORS]={0xD9,0x22,0x60,0xAD,0x3C,0x85,0x12,0xB9};
#define C32(b) (((ULONG)(b)<<24)|((ULONG)(b)<<16)|((ULONG)(b)<<8)|(ULONG)(b))

/* ------------------------------------------------------------------ */
/* Custom resize pointer sprite - shown when hovering over the map.   */
/* White ↔ arrow, 16px wide × 7 rows.  Hotspot at col 7, row 3.      */
/* Must be copied to chip RAM before use (see partview_run).          */
/* ------------------------------------------------------------------ */

static const UWORD ptr_resize_src[] = {
    0x0000, 0x0000,   /* row 0: blank */
    0x2004, 0x0000,   /* row 1: arrowhead tips */
    0x6006, 0x0000,   /* row 2: arrowhead inner */
    0xFFFE, 0x0000,   /* row 3: shaft (hotspot row) */
    0x6006, 0x0000,   /* row 4: arrowhead inner */
    0x2004, 0x0000,   /* row 5: arrowhead tips */
    0x0000, 0x0000,   /* row 6: blank */
    0x0000, 0x0000,   /* terminator */
};

/* ------------------------------------------------------------------ */
/* Partition listview - proportional-font column renderer              */
/* ------------------------------------------------------------------ */

/* Column indices */
#define LVCOL_MARK  0   /* '>' selection marker        */
#define LVCOL_DRIVE 1   /* drive name (left-aligned)   */
#define LVCOL_LOCYL 2   /* lo cylinder (right-aligned) */
#define LVCOL_HICYL 3   /* hi cylinder (right-aligned) */
#define LVCOL_FS    4   /* filesystem type             */
#define LVCOL_SIZE  5   /* size (right-aligned)        */
#define LVCOL_BOOT  6   /* boot priority               */
#define LVCOL_COUNT 7

/* Column pixel layout - computed in build_gadgets from the actual font */
static struct {
    UWORD x;    /* left edge of column */
    UWORD w;    /* column width (for right-align) */
} lv_cols[LVCOL_COUNT];

/* Header labels - match order of LVCOL_* (message ids; -1 = no label).
   Looked up via GS() at draw time in draw_col_header(). */
static const LONG lv_hdr[LVCOL_COUNT] = {
    -1, MSG_PV_COL_DRIVE, MSG_PV_COL_LOCYL, MSG_PV_COL_HICYL,
    MSG_PV_COL_FILESYSTEM, MSG_PV_COL_SIZE, MSG_PV_COL_BOOT
};

/* Pointer to current RDB and MBR (set by build_part_list) - used by render hook */
static const struct RDBInfo *lv_rdb;
static const struct MBRInfo *lv_mbr;

/* Total list entries = RDB partitions + up to 4 MBR slots */
#define MAX_LIST_ENTRIES (MAX_PARTITIONS + MBR_MAX_PARTS)

/* Forward declarations needed by lv_render (defined later in file) */
static char        part_strs[MAX_LIST_ENTRIES][80];
static struct Node part_nodes[MAX_LIST_ENTRIES];
/* Set by build_part_list: TRUE if the list entry is an MBR partition,
   and which MBR slot (0-3) it corresponds to. */
static BOOL  part_is_mbr  [MAX_LIST_ENTRIES];
static UBYTE part_mbr_slot[MAX_LIST_ENTRIES];

/* Persistent "currently selected" row index for lv_render() - see the
   comment on lvdm_State there. Set by refresh_listview(). */
static WORD g_part_sel = -1;

/* Names of partitions deleted this session, pending an unmount after the next
   RDB write (kept here rather than on the stack - see s_unmount_count reset in
   partview_run). */
static char  s_unmount_names[MAX_PARTITIONS][32];
static UWORD s_unmount_count;

/* Current MBR (updated whenever MBR is read/written this session). */
static struct MBRInfo *s_mbr;


/* Render hook - AmigaOS calls h_Entry with a0=hook, a1=msg, a2=node.
   Register variables capture those values before GCC can use the regs.
   a0/a1 are caller-saved so GCC never touches them in the prologue;
   a2 is callee-saved so GCC may push it - but PUSH doesn't change the
   register, so the captured value is always the original incoming one. */
static ULONG lv_render(void)
{
    register struct Hook      *h    __asm__("a0");
    register struct LVDrawMsg *msg  __asm__("a1");
    register struct Node      *node __asm__("a2");
    struct Hook      *_h    = h;    /* capture before GCC reuses registers */
    struct LVDrawMsg *_msg  = msg;
    struct Node      *_node = node;
    (void)_h;
#define h    _h
#define msg  _msg
#define node _node

    struct RastPort  *rp;
    struct Rectangle *b;
    BOOL   sel;
    UWORD  bg_pen, fg_pen;
    WORD   idx;
    const  struct PartInfo *pi;
    WORD   base_y;
    char   tmp[24];

    if (msg->lvdm_MethodID != LV_DRAW) return LVCB_OK;

    idx = (WORD)(node - part_nodes);
    /* Guard: must be within the valid range of list entries */
    {
        WORD max_idx = lv_rdb ? (WORD)lv_rdb->num_parts : 0;
        if (lv_mbr && lv_mbr->valid) max_idx += (WORD)MBR_MAX_PARTS;
        if (!lv_rdb || idx < 0 || idx >= max_idx)
            return LVCB_OK;
    }

    rp  = msg->lvdm_RastPort;
    b   = &msg->lvdm_Bounds;
    /* lvdm_State only reports LVR_SELECTED while the mouse button is held
       down over the row (live click-tracking) - it reverts to LVR_NORMAL
       the instant the button is released, on every ROM/platform tested, so
       it can't be used alone to show a persistent "this is the chosen
       item" mark. g_part_sel is set by the event loop on GADGETUP (see
       refresh_listview()) to make the mark persist. */
    sel = (msg->lvdm_State == LVR_SELECTED ||
           msg->lvdm_State == LVR_SELECTEDDISABLED) ||
          (idx == g_part_sel);

    /* Selection is marked by XOR-inverting the whole row at the end of this
       function (COMPLEMENT draw mode) rather than by filling with FILLPEN.
       FILLPEN can be visually indistinguishable from BACKGROUNDPEN on some
       real-hardware Workbench palettes, making pen-based highlighting
       invisible even though it's technically being drawn - COMPLEMENT mode
       works on any screen depth/palette. */
    bg_pen = (UWORD)msg->lvdm_DrawInfo->dri_Pens[BACKGROUNDPEN];
    fg_pen = (UWORD)msg->lvdm_DrawInfo->dri_Pens[TEXTPEN];

    /* Fill background */
    SetAPen(rp, (LONG)bg_pen);
    SetDrMd(rp, JAM2);
    RectFill(rp, b->MinX, b->MinY, b->MaxX, b->MaxY);

    SetAPen(rp, (LONG)fg_pen);
    SetDrMd(rp, JAM1);
    base_y = b->MinY + (WORD)rp->TxBaseline;

#define LV_TEXT(col, str, len) do { \
    Move(rp, (WORD)(b->MinX + (WORD)lv_cols[(col)].x), base_y); \
    Text(rp, (str), (UWORD)(len)); } while(0)

#define LV_RIGHT(col, str, len) do { \
    WORD _tw = (WORD)TextLength(rp, (str), (UWORD)(len)); \
    WORD _rx = (WORD)(b->MinX + (WORD)lv_cols[(col)].x + \
                      (WORD)lv_cols[(col)].w - _tw); \
    Move(rp, _rx, base_y); \
    Text(rp, (str), (UWORD)(len)); } while(0)

    /* Column dividers - vertical line centred in the 6px gap before each
       column from LOCYL onwards.  Drawn before text so glyphs overlay them. */
    {
        UWORD dc;
        SetAPen(rp, (LONG)msg->lvdm_DrawInfo->dri_Pens[SHADOWPEN]);
        SetDrMd(rp, JAM1);
        for (dc = LVCOL_LOCYL; dc < LVCOL_COUNT; dc++) {
            WORD dx = b->MinX + (WORD)lv_cols[dc].x - 3;
            if (dx <= b->MinX || dx >= b->MaxX) continue;
            Move(rp, dx, b->MinY);
            Draw(rp, dx, b->MaxY);
        }
        SetAPen(rp, (LONG)fg_pen);
    }

    /* Selection marker */
    if (sel) { tmp[0] = '>'; LV_TEXT(LVCOL_MARK, tmp, 1); }

    if (part_is_mbr[idx]) {
        /* MBR entry */
        UBYTE slot = part_mbr_slot[idx];
        const struct MBRPart *mp = lv_mbr ? &lv_mbr->parts[slot] : NULL;
        char  tn[12];
        ULONG heads   = lv_rdb ? lv_rdb->heads   : 1;
        ULONG sectors = lv_rdb ? lv_rdb->sectors : 1;
        ULONG lo_cyl  = 0, hi_cyl = 0;
        char  sz[16];
        UQUAD bytes;

        if (!mp) goto lv_done;

        if (heads == 0) heads = 1;
        if (sectors == 0) sectors = 1;
        lo_cyl = MBR_LBAToCyl(mp->lba_start, heads, sectors);
        hi_cyl = MBR_LBAToCyl(mp->lba_start + mp->lba_size - 1, heads, sectors);
        bytes  = (UQUAD)mp->lba_size * 512UL;

        /* Drive name: "MBR1".."MBR4" */
        LV_TEXT(LVCOL_DRIVE, mp->name, strlen(mp->name));

        DP_SNPRINTF(tmp, "%lu", (unsigned long)lo_cyl);
        LV_RIGHT(LVCOL_LOCYL, tmp, strlen(tmp));

        DP_SNPRINTF(tmp, "%lu", (unsigned long)hi_cyl);
        LV_RIGHT(LVCOL_HICYL, tmp, strlen(tmp));

        MBR_TypeName(mp->type, tn);
        LV_TEXT(LVCOL_FS, tn, strlen(tn));

        FormatSize(bytes, sz);
        LV_RIGHT(LVCOL_SIZE, sz, strlen(sz));

        /* Boot column: "*" if active flag set (no numeric priority for MBR) */
        if (mp->active) { tmp[0] = '*'; LV_TEXT(LVCOL_BOOT, tmp, 1); }
    } else {
        /* RDB entry */
        pi = &lv_rdb->parts[idx];

        /* Drive name - clip to the column so a long name never runs into Lo Cyl. */
        {
            const char *nm = pi->drive_name[0] ? pi->drive_name : GS(MSG_PV_NONE);
            UWORD len = (UWORD)strlen(nm);
            UWORD cw  = lv_cols[LVCOL_DRIVE].w;
            while (len > 0 && (UWORD)TextLength(rp, nm, len) > cw) len--;
            LV_TEXT(LVCOL_DRIVE, nm, len);
        }

        /* Lo Cyl */
        DP_SNPRINTF(tmp, "%lu", (unsigned long)pi->low_cyl);
        LV_RIGHT(LVCOL_LOCYL, tmp, strlen(tmp));

        /* Hi Cyl */
        DP_SNPRINTF(tmp, "%lu", (unsigned long)pi->high_cyl);
        LV_RIGHT(LVCOL_HICYL, tmp, strlen(tmp));

        /* Filesystem */
        {
            char dt[16];
            FriendlyDosType(pi->dos_type, dt);
            LV_TEXT(LVCOL_FS, dt, strlen(dt));
        }

        /* Size (right-aligned) */
        {
            char sz[16];
            ULONG cyls  = (pi->high_cyl >= pi->low_cyl)
                          ? pi->high_cyl - pi->low_cyl + 1 : 0;
            ULONG heads = pi->heads   > 0 ? pi->heads   : (lv_rdb ? lv_rdb->heads   : 1);
            ULONG secs  = pi->sectors > 0 ? pi->sectors : (lv_rdb ? lv_rdb->sectors : 1);
            ULONG bsz   = pi->block_size > 0 ? pi->block_size : 512;
            UQUAD bytes = (UQUAD)cyls * heads * secs * bsz;
            FormatSize(bytes, sz);
            LV_RIGHT(LVCOL_SIZE, sz, strlen(sz));
        }

        /* Boot priority - prefix "* " when the partition is bootable. */
        DP_SNPRINTF(tmp, "%s%ld", (pi->flags & 1) ? "* " : "", (long)pi->boot_pri);
        LV_TEXT(LVCOL_BOOT, tmp, strlen(tmp));
    }

lv_done:

    /* XOR-invert the whole row to mark the selection. Palette-independent,
       unlike filling with FILLPEN (see comment above). */
    if (sel) {
        SetDrMd(rp, COMPLEMENT);
        RectFill(rp, b->MinX, b->MinY, b->MaxX, b->MaxY);
        SetDrMd(rp, JAM1);
    }

#undef LV_TEXT
#undef LV_RIGHT

#undef h
#undef msg
#undef node
    return LVCB_OK;
}

static struct Hook lv_hook;   /* h_Entry set to lv_render in build_gadgets */

static struct List part_list;

static void list_init(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

/* Prompt the user for reboot.  If they accept, sleep ~3 seconds (let any
   pending writes drain, screens close, libraries flush) and ColdReboot. */
static void offer_reboot(struct Window *win, const char *msg)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
    es.es_TextFormat   = (UBYTE *)msg;
    es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_REBOOT_LATER);
    if (EasyRequest(win, &es, NULL) == 1) {
        /* Show a small window during the settle delay so the pause before the
           machine resets isn't a mystery. */
        struct Screen *scr = win ? win->WScreen : NULL;
        struct Window *rw  = NULL;
        if (scr) {
            UWORD font_h = scr->Font ? scr->Font->ta_YSize : 8;
            UWORD w = 260;
            UWORD h = (UWORD)(scr->WBorTop + font_h + 1 + font_h + 8 + scr->WBorBottom);
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width  - w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - h) / 2) },
                { WA_Width,     (ULONG)w }, { WA_Height, (ULONG)h },
                { WA_Title,     (ULONG)GS(MSG_PV_PLEASE_WAIT) },
                { WA_PubScreen, (ULONG)scr },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_SIMPLE_REFRESH },
                { WA_IDCMP,     0 },
                { TAG_DONE,     0 }
            };
            rw = OpenWindowTagList(NULL, wt);
            if (rw) {
                const char *t = GS(MSG_PV_REBOOTING);
                SetAPen(rw->RPort, 1);
                Move(rw->RPort, (WORD)(rw->BorderLeft + 6),
                                (WORD)(rw->BorderTop + font_h));
                Text(rw->RPort, (CONST_STRPTR)t, (LONG)strlen(t));
            }
        }
        Delay(100);      /* ~2 seconds - let the system settle before reboot */
        ColdReboot();
        if (rw) CloseWindow(rw);   /* unreached unless ColdReboot is a no-op */
    }
}

static void build_part_list(struct RDBInfo *rdb, WORD sel)
{
    UWORD i;
    UWORD n = 0;   /* running count of list entries added */

    lv_rdb = rdb;
    lv_mbr = s_mbr;
    list_init(&part_list);
    memset(part_is_mbr,   0, sizeof(part_is_mbr));
    memset(part_mbr_slot, 0, sizeof(part_mbr_slot));

    /* RDB partitions */
    if (rdb && rdb->valid) {
        for (i = 0; i < rdb->num_parts && n < MAX_LIST_ENTRIES; i++, n++) {
            struct PartInfo *pi = &rdb->parts[i];
            char dt[16], sz[16], boot[12];
            ULONG cyls  = (pi->high_cyl >= pi->low_cyl)
                          ? pi->high_cyl - pi->low_cyl + 1 : 0;
            ULONG heads = pi->heads   > 0 ? pi->heads   : rdb->heads;
            ULONG secs  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
            ULONG bsz   = pi->block_size > 0 ? pi->block_size : 512;
            UQUAD bytes = (UQUAD)cyls * heads * secs * bsz;
            const char *nm = pi->drive_name[0] ? pi->drive_name : GS(MSG_PV_NONE);

            FriendlyDosType(pi->dos_type, dt);
            FormatSize(bytes, sz);
            DP_SNPRINTF(boot, "%s%ld", (pi->flags & 1) ? "*" : "", (long)pi->boot_pri);

            snprintf(part_strs[n], sizeof(part_strs[n]),
                    "%c %-7s %9lu %9lu  %-12s  %9s   %4s",
                    ((WORD)n == sel) ? '>' : ' ',
                    nm,
                    (unsigned long)pi->low_cyl,
                    (unsigned long)pi->high_cyl,
                    dt, sz, boot);

            part_is_mbr[n]    = FALSE;
            part_mbr_slot[n]  = 0;
            part_nodes[n].ln_Name = part_strs[n];
            part_nodes[n].ln_Type = NT_USER;
            part_nodes[n].ln_Pri  = 0;
            AddTail(&part_list, &part_nodes[n]);
        }
    }

    /* MBR partitions (if MBR is valid) */
    if (s_mbr && s_mbr->valid) {
        ULONG heads   = (rdb && rdb->valid && rdb->heads   > 0) ? rdb->heads   : 1;
        ULONG sectors = (rdb && rdb->valid && rdb->sectors > 0) ? rdb->sectors : 1;
        for (i = 0; i < MBR_MAX_PARTS && n < MAX_LIST_ENTRIES; i++) {
            const struct MBRPart *mp = &s_mbr->parts[i];
            char tn[12], sz[16], boot[4];
            ULONG lo_cyl, hi_cyl;
            UQUAD bytes;
            if (!mp->present) continue;

            lo_cyl = MBR_LBAToCyl(mp->lba_start, heads, sectors);
            hi_cyl = MBR_LBAToCyl(mp->lba_start + mp->lba_size - 1, heads, sectors);
            bytes  = (UQUAD)mp->lba_size * 512UL;
            MBR_TypeName(mp->type, tn);
            FormatSize(bytes, sz);
            boot[0] = mp->active ? '*' : ' '; boot[1] = '\0';

            snprintf(part_strs[n], sizeof(part_strs[n]),
                    "%c %-7s %9lu %9lu  %-12s  %9s   %s",
                    ((WORD)n == sel) ? '>' : ' ',
                    mp->name,
                    (unsigned long)lo_cyl,
                    (unsigned long)hi_cyl,
                    tn, sz, boot);

            part_is_mbr[n]   = TRUE;
            part_mbr_slot[n] = (UBYTE)i;
            part_nodes[n].ln_Name = part_strs[n];
            part_nodes[n].ln_Type = NT_USER;
            part_nodes[n].ln_Pri  = 0;
            AddTail(&part_list, &part_nodes[n]);
            n++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pen allocation                                                       */
/* ------------------------------------------------------------------ */

static LONG part_pens[NUM_PART_COLORS];
static LONG bg_pen;      /* dark navy background for the map  */
static LONG rdb_pen;     /* muted gray for RDB reserved area  */
static LONG mbr_pen;     /* warm orange/tan for MBR partitions */

/* Fallback pen indices used on graphics.library < V39, where ObtainBestPenA
 * does not exist (it was added in V39 / KS 3.0).  These are well-known
 * Workbench palette slots that exist on a default 4- or 8-colour screen. */
static const UBYTE FALLBACK_PART_PENS[NUM_PART_COLORS] =
    { 1, 2, 3, 4, 5, 6, 7, 1 };
#define FALLBACK_BG_PEN  0
#define FALLBACK_RDB_PEN 2
#define FALLBACK_MBR_PEN 6

static BOOL gfx_has_obtainbestpen(void)
{
    return (BOOL)(((struct Library *)GfxBase)->lib_Version >= 39);
}

static void alloc_pens(struct Screen *scr)
{
    UWORD i;
    if (gfx_has_obtainbestpen()) {
        struct ColorMap *cm = scr->ViewPort.ColorMap;
        struct TagItem   nt[] = { { TAG_DONE, 0 } };
        for (i = 0; i < NUM_PART_COLORS; i++)
            part_pens[i] = ObtainBestPenA(cm,
                C32(PART_R[i]), C32(PART_G[i]), C32(PART_B[i]), nt);
        bg_pen  = ObtainBestPenA(cm, C32(0x2a), C32(0x2a), C32(0x3a), nt);
        rdb_pen = ObtainBestPenA(cm, C32(0x55), C32(0x55), C32(0x66), nt);
        mbr_pen = ObtainBestPenA(cm, C32(0xC8), C32(0x82), C32(0x00), nt);
    } else {
        /* V37/V38 - use fixed Workbench palette pens. No allocation needed. */
        for (i = 0; i < NUM_PART_COLORS; i++)
            part_pens[i] = (LONG)FALLBACK_PART_PENS[i];
        bg_pen  = FALLBACK_BG_PEN;
        rdb_pen = FALLBACK_RDB_PEN;
        mbr_pen = FALLBACK_MBR_PEN;
    }
}

static void free_pens(struct Screen *scr)
{
    UWORD i;
    if (gfx_has_obtainbestpen()) {
        struct ColorMap *cm = scr->ViewPort.ColorMap;
        for (i = 0; i < NUM_PART_COLORS; i++)
            if (part_pens[i] >= 0) { ReleasePen(cm,(ULONG)part_pens[i]); part_pens[i]=-1; }
        if (bg_pen  >= 0) { ReleasePen(cm,(ULONG)bg_pen);  bg_pen  = -1; }
        if (rdb_pen >= 0) { ReleasePen(cm,(ULONG)rdb_pen); rdb_pen = -1; }
        if (mbr_pen >= 0) { ReleasePen(cm,(ULONG)mbr_pen); mbr_pen = -1; }
    } else {
        for (i = 0; i < NUM_PART_COLORS; i++) part_pens[i] = -1;
        bg_pen = rdb_pen = mbr_pen = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Disk map drawing (matches AmigaPart _draw_map style)                */
/* ------------------------------------------------------------------ */

static void draw_map(struct Window *win, struct RDBInfo *rdb, WORD sel,
                     WORD bx, WORD by, UWORD bw, UWORD bh)
{
    struct RastPort *rp  = win->RPort;
    WORD  fh = rp->TxHeight;
    WORD  fb = rp->TxBaseline;
    LONG  fill  = (bg_pen  >= 0) ? bg_pen  : 0;
    LONG  rfill = (rdb_pen >= 0) ? rdb_pen : 2;
    WORD  i;

    /* Map inner area - leave 1px border all round */
    WORD  mx  = bx + 1;
    WORD  my  = by + 1;
    UWORD mw  = bw - 2;
    UWORD mh  = bh - 2;

    /* Outer border */
    SetAPen(rp, 2);
    SetDrMd(rp, JAM1);
    Move(rp, bx,          by);     Draw(rp, bx+(WORD)bw, by);
    Draw(rp, bx+(WORD)bw, by+(WORD)bh);
    Draw(rp, bx,          by+(WORD)bh);
    Draw(rp, bx,          by);

    /* Background - free space */
    SetAPen(rp, fill);
    SetDrMd(rp, JAM2);
    RectFill(rp, mx, my, mx+(WORD)mw-1, my+(WORD)mh-1);

    if (!rdb || !rdb->valid) {
        const char *msg = GS(MSG_PV_MAP_NO_RDB);
        UWORD mlen = strlen(msg);
        WORD  tw   = rp->TxWidth ? (WORD)(mlen*(UWORD)rp->TxWidth):(WORD)(mlen*8);
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, bx + ((WORD)bw-(WORD)tw)/2, by+((WORD)bh-fh)/2+fb);
        Text(rp, msg, mlen);
        return;
    }

    {
        ULONG lo    = rdb->lo_cyl;
        ULONG hi    = rdb->hi_cyl;
        ULONG total = hi + 1;   /* full disk cylinder count (including RDB area) */

#define MAP_X(cyl) ((WORD)(mx + (WORD)((UQUAD)(cyl) * mw / total)))

        /* RDB reserved area (cylinder 0 .. lo_cyl-1) */
        if (lo > 0) {
            WORD rx2 = MAP_X(lo);
            if (rx2 > mx + 1) {
                SetAPen(rp, rfill);
                SetDrMd(rp, JAM2);
                RectFill(rp, mx, my, rx2-1, my+(WORD)mh-1);
                if (rx2 - mx > 24) {
                    SetAPen(rp, 1);
                    SetDrMd(rp, JAM1);
                    Move(rp, mx + (rx2-mx)/2 - 6, by+((WORD)bh-fh)/2+fb);
                    Text(rp, "RDB", 3);
                }
            }
        }

        /* Partition blocks */
        for (i = 0; i < (WORD)rdb->num_parts; i++) {
            struct PartInfo *pi  = &rdb->parts[i];
            WORD  map_right = mx + (WORD)mw;
            WORD  px1 = MAP_X(pi->low_cyl);
            WORD  px2 = MAP_X(pi->high_cyl + 1);
            LONG  pen;
            WORD  pw;

            /* Clip to map area - a partition with high_cyl > rdb->hi_cyl
               (e.g. after a buggy geometry update) must not draw past
               the right edge. */
            if (px1 < mx) px1 = mx;
            if (px1 > map_right) px1 = map_right;
            if (px2 < mx) px2 = mx;
            if (px2 > map_right) px2 = map_right;
            if (px2 < px1 + 2) px2 = (px1 + 2 > map_right) ? map_right : px1 + 2;
            pen = part_pens[i % NUM_PART_COLORS];
            if (pen < 0) pen = (i % 3) + 3;

            SetAPen(rp, pen);
            SetDrMd(rp, JAM2);
            RectFill(rp, px1, my, px2-1, my+(WORD)mh-1);

            /* Border */
            SetAPen(rp, 2);
            SetDrMd(rp, JAM1);
            Move(rp, px1, my);             Draw(rp, px2-1, my);
            Move(rp, px1, my+(WORD)mh-1);  Draw(rp, px2-1, my+(WORD)mh-1);
            Move(rp, px1, my);             Draw(rp, px1,   my+(WORD)mh-1);
            Move(rp, px2-1, my);           Draw(rp, px2-1, my+(WORD)mh-1);

            /* Drive name + size label */
            pw = px2 - px1;
            if (pw > 12) {
                char  sz[16];
                UWORD slen;
                ULONG cyls2  = (pi->high_cyl >= pi->low_cyl)
                               ? pi->high_cyl - pi->low_cyl + 1 : 0;
                ULONG heads2 = pi->heads   > 0 ? pi->heads   : rdb->heads;
                ULONG secs2  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
                ULONG bsz2   = pi->block_size > 0 ? pi->block_size : 512;
                UQUAD bytes2 = (UQUAD)cyls2 * heads2 * secs2 * bsz2;
                WORD  txw    = rp->TxWidth ? (WORD)rp->TxWidth : 8;
                WORD  max_c  = (pw - 4) / txw;
                char  *nm    = pi->drive_name[0] ? pi->drive_name : GS(MSG_PV_NONE);
                UWORD nlen   = strlen(nm);
                WORD  block_top; /* top of two-line text block */

                FormatSize(bytes2, sz);
                slen = strlen(sz);

                if ((WORD)nlen > max_c) nlen = (UWORD)max_c;
                if ((WORD)slen > max_c) slen = (UWORD)max_c;

                /* Centre the two-line block vertically inside the map bar */
                block_top = my + ((WORD)mh - (fh * 2 + 1)) / 2;

                SetAPen(rp, 1);
                SetDrMd(rp, JAM1);

                if (nlen > 0) {
                    WORD tw = (WORD)(nlen * (UWORD)txw);
                    Move(rp, px1 + (pw - tw) / 2, block_top + fb);
                    Text(rp, nm, nlen);
                }
                if (slen > 0 && (WORD)mh >= fh * 2 + 4) {
                    WORD tw = (WORD)(slen * (UWORD)txw);
                    Move(rp, px1 + (pw - tw) / 2, block_top + fh + 1 + fb);
                    Text(rp, sz, slen);
                }
            }
        }

        /* MBR partitions */
        if (s_mbr && s_mbr->valid) {
            ULONG mheads   = rdb->heads   > 0 ? rdb->heads   : 1;
            ULONG msectors = rdb->sectors > 0 ? rdb->sectors : 1;
            UWORD mi;
            LONG  mfill = (mbr_pen >= 0) ? mbr_pen : (LONG)FALLBACK_MBR_PEN;

            for (mi = 0; mi < MBR_MAX_PARTS; mi++) {
                const struct MBRPart *mp = &s_mbr->parts[mi];
                WORD map_right = mx + (WORD)mw;
                WORD px1, px2, pw;
                char tn[12];
                WORD txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
                WORD max_c, block_top;

                if (!mp->present) continue;

                {
                    ULONG lo_cyl = MBR_LBAToCyl(mp->lba_start, mheads, msectors);
                    ULONG hi_cyl = MBR_LBAToCyl(mp->lba_start + mp->lba_size - 1,
                                                 mheads, msectors);
                    px1 = MAP_X(lo_cyl);
                    px2 = MAP_X(hi_cyl + 1);
                }

                if (px1 < mx) px1 = mx;
                if (px1 > map_right) px1 = map_right;
                if (px2 < mx) px2 = mx;
                if (px2 > map_right) px2 = map_right;
                if (px2 < px1 + 2) px2 = (px1 + 2 > map_right) ? map_right : px1 + 2;

                /* Fill with MBR pen, then hatch every 2nd line with background */
                SetAPen(rp, mfill);
                SetDrMd(rp, JAM2);
                RectFill(rp, px1, my, px2-1, my+(WORD)mh-1);
                {
                    LONG hpen = (bg_pen >= 0) ? bg_pen : 0;
                    WORD yy;
                    SetAPen(rp, hpen);
                    SetDrMd(rp, JAM1);
                    for (yy = my; yy <= my+(WORD)mh-1; yy += 2) {
                        Move(rp, px1, yy);
                        Draw(rp, px2-1, yy);
                    }
                }

                /* Border */
                SetAPen(rp, 2);
                SetDrMd(rp, JAM1);
                Move(rp, px1, my);             Draw(rp, px2-1, my);
                Move(rp, px1, my+(WORD)mh-1);  Draw(rp, px2-1, my+(WORD)mh-1);
                Move(rp, px1, my);             Draw(rp, px1,   my+(WORD)mh-1);
                Move(rp, px2-1, my);           Draw(rp, px2-1, my+(WORD)mh-1);

                /* Type name label */
                pw = px2 - px1;
                if (pw > 12) {
                    MBR_TypeName(mp->type, tn);
                    max_c = (pw - 4) / txw;
                    {
                        UWORD nlen = strlen(tn);
                        if ((WORD)nlen > max_c) nlen = (UWORD)max_c;
                        block_top = my + ((WORD)mh - fh) / 2;
                        SetAPen(rp, 1);
                        SetDrMd(rp, JAM1);
                        if (nlen > 0) {
                            WORD tw = (WORD)(nlen * (UWORD)txw);
                            Move(rp, px1 + (pw - tw) / 2, block_top + fb);
                            Text(rp, tn, nlen);
                        }
                    }
                }
            }
        }

        /* Selection highlight: 3-px bright frame + dark shadow frame.
           Works for both RDB and MBR selections. */
        {
            WORD sx1 = -1, sx2 = -1;
            WORD bsz = 3;

            if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                struct PartInfo *sp = &rdb->parts[sel];
                sx1 = MAP_X(sp->low_cyl);
                sx2 = MAP_X(sp->high_cyl + 1);
            } else if (sel >= (WORD)rdb->num_parts && s_mbr && s_mbr->valid) {
                /* Find which MBR entry this list position corresponds to */
                WORD list_pos = (WORD)rdb->num_parts;
                UWORD mi;
                ULONG mheads   = rdb->heads   > 0 ? rdb->heads   : 1;
                ULONG msectors = rdb->sectors > 0 ? rdb->sectors : 1;
                for (mi = 0; mi < MBR_MAX_PARTS; mi++) {
                    if (!s_mbr->parts[mi].present) continue;
                    if (list_pos == sel) {
                        ULONG lo = MBR_LBAToCyl(s_mbr->parts[mi].lba_start,
                                                 mheads, msectors);
                        ULONG hi = MBR_LBAToCyl(s_mbr->parts[mi].lba_start +
                                                 s_mbr->parts[mi].lba_size - 1,
                                                 mheads, msectors);
                        sx1 = MAP_X(lo);
                        sx2 = MAP_X(hi + 1);
                        break;
                    }
                    list_pos++;
                }
            }

            if (sx1 >= 0 && sx2 > sx1) {
                if (sx2 < sx1 + 2) sx2 = sx1 + 2;
                SetAPen(rp, 2);
                SetDrMd(rp, JAM2);
                if (sx1 > mx) RectFill(rp, sx1-1, my, sx1-1, my+(WORD)mh-1);
                if (sx2 < mx+(WORD)mw) RectFill(rp, sx2, my, sx2, my+(WORD)mh-1);
                RectFill(rp, sx1, my-1 > my ? my-1 : my, sx2-1, my-1 > my ? my-1 : my);
                SetAPen(rp, 1);
                RectFill(rp, sx1,        my,              sx2-1,        my+bsz-1);
                RectFill(rp, sx1,        my+(WORD)mh-bsz, sx2-1,        my+(WORD)mh-1);
                RectFill(rp, sx1,        my+bsz,          sx1+bsz-1,    my+(WORD)mh-bsz-1);
                RectFill(rp, sx2-bsz,    my+bsz,          sx2-1,        my+(WORD)mh-bsz-1);
            }
        }

#undef MAP_X

        /* Axis labels - lo/hi cylinder only; free space is shown in info area */
        {
            char lo_str[24], hi_str[24];
            WORD label_y = by + (WORD)bh + 2 + fb;

            DP_SNPRINTF(lo_str, GS(MSG_PV_CYL_FMT), (unsigned long)lo);
            DP_SNPRINTF(hi_str, GS(MSG_PV_CYL_FMT), (unsigned long)hi);

            /* Erase the label strip before redrawing - prevents ghost text
               when the map is redrawn at a different position after resize. */
            SetAPen(rp, 0);
            SetDrMd(rp, JAM2);
            RectFill(rp, bx, by+(WORD)bh+1, bx+(WORD)bw, by+(WORD)bh+fh+4);

            SetAPen(rp, 1);
            SetDrMd(rp, JAM1);
            Move(rp, bx, label_y);
            Text(rp, lo_str, strlen(lo_str));

            {
                UWORD hlen = strlen(hi_str);
                WORD  txw  = rp->TxWidth ? (WORD)rp->TxWidth : 8;
                WORD  htw  = (WORD)(hlen * (UWORD)txw);
                Move(rp, bx+(WORD)bw-htw, label_y);
                Text(rp, hi_str, hlen);

                /* Centred usage hint - only if it fits between the Cyl labels */
                {
                    const char *hint = GS(MSG_PV_MAP_HINT);
                    UWORD hintlen = strlen(hint);
                    WORD  hinttw  = (WORD)TextLength(rp, hint, hintlen);
                    WORD  lo_end  = (WORD)TextLength(rp, lo_str, (UWORD)strlen(lo_str)) + 4;
                    WORD  avail   = (WORD)bw - lo_end - htw - 8;
                    if (hinttw <= avail) {
                        WORD cx = bx + lo_end + (avail - hinttw) / 2;
                        Move(rp, cx, label_y);
                        Text(rp, hint, hintlen);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Drag resize info - replaces axis labels during an active drag       */
/* Shows "DH0: Cyl 1 - 519  (250 MB)" centred below the map bar.     */
/* ------------------------------------------------------------------ */

static void draw_drag_info(struct Window *win, const struct RDBInfo *rdb,
                            WORD drag_part,
                            WORD bx, WORD by, UWORD bw, UWORD bh)
{
    struct RastPort *rp = win->RPort;
    WORD  fb  = rp->TxBaseline;
    WORD  fh  = rp->TxHeight;
    WORD  txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
    WORD  label_y = by + (WORD)bh + 2 + fb;
    char  info[64];
    UWORD ilen;
    const struct PartInfo *pi = &rdb->parts[drag_part];
    ULONG cyls  = pi->high_cyl >= pi->low_cyl
                  ? pi->high_cyl - pi->low_cyl + 1 : 0;
    ULONG di_heads = pi->heads   > 0 ? pi->heads   : rdb->heads;
    ULONG di_secs  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
    ULONG di_bsz   = pi->block_size > 0 ? pi->block_size : 512;
    UQUAD bytes = (UQUAD)cyls * di_heads * di_secs * di_bsz;
    char  sz[16];

    FormatSize(bytes, sz);
    snprintf(info, sizeof(info), GS(MSG_PV_DRAG_INFO_FMT),
            pi->drive_name[0] ? pi->drive_name : GS(MSG_PV_NONE),
            (unsigned long)pi->low_cyl,
            (unsigned long)pi->high_cyl,
            sz);

    /* Erase axis label strip */
    SetAPen(rp, 0);
    SetDrMd(rp, JAM2);
    RectFill(rp, bx, by+(WORD)bh+1,
             bx+(WORD)bw, by+(WORD)bh+fh+4);

    /* Draw centred info string */
    ilen = strlen(info);
    {
        WORD  iw2 = (WORD)(ilen * (UWORD)txw);
        WORD  cx  = bx + ((WORD)bw - iw2) / 2;
        if (cx < bx) cx = bx;
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, cx, label_y);
        Text(rp, info, ilen);
    }
}

/* ------------------------------------------------------------------ */
/* New-partition drag overlay - drawn on top of the map during drag    */
/* ------------------------------------------------------------------ */

static void draw_new_part_overlay(struct Window *win,
                                   ULONG lo, ULONG hi,
                                   const struct RDBInfo *rdb,
                                   WORD bx, WORD by, UWORD bw, UWORD bh)
{
    struct RastPort *rp  = win->RPort;
    WORD  mx    = bx + 1;
    UWORD mw    = bw - 2;
    WORD  my    = by + 1;
    UWORD mh    = bh - 2;
    ULONG total = rdb->hi_cyl + 1;
    WORD  px1, px2, pw;
    char  sz[16], info[64];
    UWORD ilen;
    WORD  fb  = rp->TxBaseline;
    WORD  fh  = rp->TxHeight;
    WORD  txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
    WORD  label_y = by + (WORD)bh + 2 + fb;
    ULONG cyls, bpc;
    UQUAD bytes;
    LONG  pen;

    if (total == 0) return;
    px1 = (WORD)(mx + (WORD)((UQUAD)lo       * mw / total));
    px2 = (WORD)(mx + (WORD)((UQUAD)(hi + 1) * mw / total));
    if (px2 < px1 + 2) px2 = px1 + 2;
    pw = px2 - px1;

    cyls  = (hi >= lo) ? (hi - lo + 1) : 1;
    bpc   = rdb->heads * rdb->sectors * ((rdb->blk_size > 0) ? rdb->blk_size : 512UL);
    bytes = (UQUAD)cyls * bpc;
    FormatSize(bytes, sz);

    /* Fill with the color this partition would get when added */
    pen = part_pens[rdb->num_parts % NUM_PART_COLORS];
    if (pen < 0) pen = (LONG)(rdb->num_parts % 3) + 3;
    SetAPen(rp, pen);
    SetDrMd(rp, JAM2);
    RectFill(rp, px1, my, px2-1, my+(WORD)mh-1);

    /* Bright double border */
    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, px1,   my);             Draw(rp, px2-1, my);
    Move(rp, px1,   my+(WORD)mh-1);  Draw(rp, px2-1, my+(WORD)mh-1);
    Move(rp, px1,   my);             Draw(rp, px1,   my+(WORD)mh-1);
    Move(rp, px2-1, my);             Draw(rp, px2-1, my+(WORD)mh-1);
    if (pw > 4 && (WORD)mh > 4) {
        Move(rp, px1+1,   my+1);           Draw(rp, px2-2, my+1);
        Move(rp, px1+1,   my+(WORD)mh-2);  Draw(rp, px2-2, my+(WORD)mh-2);
        Move(rp, px1+1,   my+1);           Draw(rp, px1+1, my+(WORD)mh-2);
        Move(rp, px2-2,   my+1);           Draw(rp, px2-2, my+(WORD)mh-2);
    }

    /* Size hint centred inside the box */
    {
        UWORD slen = strlen(sz);
        WORD  tw   = (WORD)(slen * (UWORD)txw);
        if (pw > tw + 4) {
            Move(rp, px1 + (pw - tw) / 2, my + ((WORD)mh - fh) / 2 + fb);
            Text(rp, sz, slen);
        }
    }

    /* Info strip below map */
    snprintf(info, sizeof(info), GS(MSG_PV_NEW_INFO_FMT),
            (unsigned long)lo, (unsigned long)hi, sz);
    SetAPen(rp, 0);
    SetDrMd(rp, JAM2);
    RectFill(rp, bx, by+(WORD)bh+1, bx+(WORD)bw, by+(WORD)bh+fh+4);
    ilen = strlen(info);
    {
        WORD iw2 = (WORD)(ilen * (UWORD)txw);
        WORD cx  = bx + ((WORD)bw - iw2) / 2;
        if (cx < bx) cx = bx;
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, cx, label_y);
        Text(rp, info, ilen);
    }
}

/* ------------------------------------------------------------------ */
/* Disk information section - drawn as text rows above the map         */
/* ------------------------------------------------------------------ */

static void draw_info(struct Window *win, const char *devname, ULONG unit,
                      struct RDBInfo *rdb, const char *brand,
                      WORD ix, WORD iy, UWORD iw)
{
    struct RastPort *rp = win->RPort;
    char   line1[120], line2[120], line3[120];
    char   sz[16];
    WORD   fb  = rp->TxBaseline;
    WORD   fh  = rp->TxHeight;
    WORD   txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
    /* Checkbox gadgets occupy the right side of line 3 - leave a gap there.
       Width formula must match the cbw in build_gadgets. */
    UWORD  cbw       = (UWORD)((UWORD)fh * 2 + 82);
    UWORD  cb_res    = (UWORD)(cbw * 2 + 16);  /* 2 checkboxes + gap + small margin */

    /* Erase the full info area (checkboxes on line 3 are redrawn by draw_static) */
    SetAPen(rp, 0);
    SetDrMd(rp, JAM2);
    RectFill(rp, ix, iy, ix+(WORD)iw-1, iy+(WORD)fh*3+8);

    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);

    /* Line 1: device / size / model */
    {
        char model[36];
        model[0] = '\0';
        if (brand && brand[0])
            strncpy(model, brand, 35);
        else if (rdb && (rdb->disk_vendor[0] || rdb->disk_product[0]))
            snprintf(model, sizeof(model), "%s %s",
                     rdb->disk_vendor, rdb->disk_product);
        model[35] = '\0';

        if (rdb && rdb->cylinders > 0) {
            ULONG bsz = (rdb->blk_size > 0) ? rdb->blk_size : 512;
            FormatSize((UQUAD)rdb->cylinders * rdb->heads * rdb->sectors * bsz, sz);
        } else {
            strncpy(sz, GS(MSG_PV_UNKNOWN), 15); sz[15] = '\0';
        }

        if (model[0])
            snprintf(line1, sizeof(line1), GS(MSG_PV_INFO_LINE1_MODEL),
                     devname, (unsigned long)unit, sz, model);
        else
            snprintf(line1, sizeof(line1), GS(MSG_PV_INFO_LINE1),
                     devname, (unsigned long)unit, sz);
    }

    /* Line 2: full geometry so large cylinder counts never clip */
    if (rdb && rdb->cylinders > 0)
        DP_SNPRINTF(line2, GS(MSG_PV_INFO_GEOMETRY_FMT),
                (unsigned long)rdb->cylinders,
                (unsigned long)rdb->heads,
                (unsigned long)rdb->sectors);
    else
        strncpy(line2, GS(MSG_PV_INFO_GEOM_UNKNOWN), 119);

    /* Line 3: RDB partition / free info (text clipped short; right side
       is occupied by the Last Disk / Last LUN checkbox gadgets) */
    if (rdb && rdb->valid) {
        char fsz[16];
        ULONG free_cyls = rdb->hi_cyl - rdb->lo_cyl + 1;
        UWORD fi;
        for (fi = 0; fi < rdb->num_parts; fi++) {
            ULONG used = rdb->parts[fi].high_cyl - rdb->parts[fi].low_cyl + 1;
            if (free_cyls >= used) free_cyls -= used;
        }
        { ULONG bsz = (rdb->blk_size > 0) ? rdb->blk_size : 512;
          FormatSize((UQUAD)free_cyls * rdb->heads * rdb->sectors * bsz, fsz); }
        DP_SNPRINTF(line3, GS(MSG_PV_INFO_RDB_FMT),
                (unsigned)rdb->num_parts,
                rdb->num_parts == 1 ? "" : "s", fsz);
    } else {
        strncpy(line3, GS(MSG_PV_INFO_RDB_NOTFOUND), 119);
    }
    line2[119] = line3[119] = '\0';

    {
        UWORD max_full = (UWORD)((iw - 4) / (UWORD)txw);
        UWORD max_l3   = (cb_res + 4 < iw) ? (UWORD)((iw - 4 - cb_res) / (UWORD)txw) : 0;
        UWORD l;

        l = (UWORD)strlen(line1); if (l > max_full) l = max_full;
        Move(rp, ix + 2, iy + fb);
        Text(rp, line1, l);

        l = (UWORD)strlen(line2); if (l > max_full) l = max_full;
        Move(rp, ix + 2, iy + (WORD)(fh + 2) + fb);
        Text(rp, line2, l);

        l = (UWORD)strlen(line3); if (l > max_l3) l = max_l3;
        Move(rp, ix + 2, iy + (WORD)(fh + 2) * 2 + fb);
        Text(rp, line3, l);
    }
}

/* ------------------------------------------------------------------ */
/* Column header - drawn just above the listview gadget                */
/* ------------------------------------------------------------------ */

static void draw_col_header(struct Window *win, WORD hx, WORD hy, UWORD hw)
{
    struct RastPort *rp  = win->RPort;
    WORD  fb  = rp->TxBaseline;
    WORD  fh  = rp->TxHeight;
    UWORD i;

    /* Background strip */
    SetAPen(rp, 2);
    SetDrMd(rp, JAM2);
    RectFill(rp, hx, hy, hx+(WORD)hw-1, hy+fh+1);

    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);

    /* Draw each column label at its computed pixel position */
    for (i = LVCOL_MARK + 1; i < LVCOL_COUNT; i++) {
        const char *label = GS(lv_hdr[i]);
        UWORD llen = strlen(label);
        WORD  lx   = hx + (WORD)lv_cols[i].x;
        /* Skip if column starts beyond the available width */
        if ((WORD)lv_cols[i].x >= (WORD)hw - 4) break;
        /* For right-aligned data columns, right-align the header label too */
        if (i == LVCOL_LOCYL || i == LVCOL_HICYL || i == LVCOL_SIZE) {
            WORD tw = (WORD)TextLength(rp, label, llen);
            lx += (WORD)lv_cols[i].w - tw;
        }
        Move(rp, lx, hy + fb);
        Text(rp, label, llen);
    }

    /* Divider lines - same positions as in lv_render, pen 1 on dark header */
    for (i = LVCOL_LOCYL; i < LVCOL_COUNT; i++) {
        WORD dx = hx + (WORD)lv_cols[i].x - 3;
        if (dx <= hx || dx >= hx + (WORD)hw - 1) continue;
        Move(rp, dx, hy);
        Draw(rp, dx, hy + fh + 1);
    }
}

/* ------------------------------------------------------------------ */
/* Draw all static text elements (called on open and on refresh)       */
/* ------------------------------------------------------------------ */

static void draw_static(struct Window *win, const char *devname, ULONG unit,
                         struct RDBInfo *rdb, const char *brand,
                         WORD ix, WORD iy, UWORD iw,   /* info section */
                         WORD bx, WORD by, UWORD bw, UWORD bh, /* map */
                         WORD hx, WORD hy, UWORD hw,   /* col header */
                         WORD sel,
                         struct Gadget *lastdisk_gad, struct Gadget *lastlun_gad)
{
    draw_info(win, devname, unit, rdb, brand, ix, iy, iw);
    /* draw_info erases the full info area including the checkbox slots -
       refresh those two gadgets so they reappear over the cleared background. */
    if (lastdisk_gad) RefreshGList(lastdisk_gad, win, NULL, lastlun_gad ? 2 : 1);
    draw_map (win, rdb, sel, bx, by, bw, bh);
    draw_col_header(win, hx, hy, hw);
}

/* ------------------------------------------------------------------ */
/* Listview refresh                                                    */
/* ------------------------------------------------------------------ */

static void refresh_listview(struct Window *win, struct Gadget *lv_gad,
                              struct RDBInfo *rdb, WORD sel)
{
    ULONG top = 0;
    struct TagItem get_top[]  = { { GTLV_Top,    (ULONG)&top       }, { TAG_DONE, 0 } };
    struct TagItem detach[]   = { { GTLV_Labels, ~0UL              }, { TAG_DONE, 0 } };
    struct TagItem reattach[] = { { GTLV_Labels, (ULONG)&part_list }, { TAG_DONE, 0 } };
    struct TagItem restore[]  = { { GTLV_Top,    0                 }, { TAG_DONE, 0 } };

    g_part_sel = sel;
    /* Detaching/reattaching GTLV_Labels (needed to force a redraw of a
       GTLV_CallBack listview - see gui_force_lv_redraw()) resets the
       scroll position, so save/restore it around the rebuild. Can't use
       gui_force_lv_redraw() directly here since build_part_list() needs to
       run between the detach and reattach. */
    GT_GetGadgetAttrsA(lv_gad, win, NULL, get_top);
    GT_SetGadgetAttrsA(lv_gad, win, NULL, detach);
    build_part_list(rdb, sel);
    GT_SetGadgetAttrsA(lv_gad, win, NULL, reattach);
    restore[0].ti_Data = top;
    GT_SetGadgetAttrsA(lv_gad, win, NULL, restore);
}

/* Lightweight version of refresh_listview() for a plain click: only moves
   the persistent highlight, without re-deriving/reformatting every row's
   display string (build_part_list() calls FriendlyDosType()/FormatSize()/
   snprintf() per row) - real, visible-on-real-hardware work on this
   project's m68000 target for a list that can hold up to MAX_LIST_ENTRIES
   rows. Use refresh_listview() instead whenever the underlying partition
   data actually changed (add/delete/resize/etc). */
static void mark_listview_selection(struct Window *win, struct Gadget *lv_gad, WORD sel)
{
    g_part_sel = sel;
    gui_force_lv_redraw(lv_gad, win, &part_list);
}

/* ------------------------------------------------------------------ */
/* Free cylinder range                                                 */
/* ------------------------------------------------------------------ */

/* Case-insensitive string compare (returns TRUE if equal). */
static BOOL name_eq(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return FALSE;
        if (ca == '\0') return TRUE;
    }
}

/* Find the lowest N such that "DH<N>" is not already used by any partition
   in this RDB *and* is not already present in the AmigaOS DosList.
   Checking the DosList avoids suggesting e.g. DH0 when another disk
   already has a DH0: device mounted. */
static void next_drive_name(const struct RDBInfo *rdb, char *buf)
{
    ULONG n;
    for (n = 0; n <= MAX_PARTITIONS; n++) {
        UWORD k;
        BOOL  taken = FALSE;
        char  cand[8];
        DP_SNPRINTF(cand, "DH%lu", n);

        /* Check partitions already in this RDB */
        for (k = 0; k < rdb->num_parts && !taken; k++)
            if (name_eq(cand, rdb->parts[k].drive_name))
                taken = TRUE;

        /* Check AmigaOS DosList (mounted devices from all other disks) */
        if (!taken) {
            struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
            while ((dl = NextDosEntry(dl, LDF_DEVICES)) != NULL) {
                /* dol_Name is a BSTR: BADDR gives ptr where byte 0 is
                   the length and bytes 1..len are the name. */
                const UBYTE *bs = (const UBYTE *)BADDR(dl->dol_Name);
                UBYTE        len = bs[0];
                char         tmp[32];
                UBYTE        i;
                if (len >= sizeof(tmp)) len = (UBYTE)(sizeof(tmp) - 1);
                for (i = 0; i < len; i++) tmp[i] = (char)bs[i + 1];
                tmp[len] = '\0';
                if (name_eq(cand, tmp)) { taken = TRUE; break; }
            }
            UnLockDosList(LDF_DEVICES | LDF_READ);
        }

        if (!taken) { strncpy(buf, cand, 31); buf[31] = '\0'; return; }
    }
    strncpy(buf, "DH0", 31);   /* fallback, shouldn't happen */
}

/* Recommend block_size / sectors_per_block / max_transfer / mask for a
 * brand-new partition. Logical FFS block size grows with partition size
 * (conservative FFS buckets); MaxTransfer/Mask default to safe IDE values
 * (0x1FE00 = 255 sectors, 0x7FFFFFFE).
 *   <=512 MB -> 512   (spb=1)
 *   <=2 GB   -> 1024  (spb=2)
 *   <=4 GB   -> 2048  (spb=4)
 *   > 4 GB   -> 4096  (spb=8)
 * The dialog displays block_size * sectors_per_block as the FFS block size. */
static void recommend_new_part_defaults(const struct RDBInfo *rdb,
                                        ULONG low_cyl, ULONG high_cyl,
                                        struct PartInfo *pi)
{
    ULONG heads   = rdb->heads   > 0 ? rdb->heads   : 1;
    ULONG sectors = rdb->sectors > 0 ? rdb->sectors : 1;
    ULONG cyls    = (high_cyl >= low_cyl) ? (high_cyl - low_cyl + 1) : 1;
    UQUAD bytes   = (UQUAD)cyls * heads * sectors * 512ULL;
    ULONG spb;

    if      (bytes <= (UQUAD)512 * 1024 * 1024)         spb = 1;
    else if (bytes <= (UQUAD)2   * 1024 * 1024 * 1024)  spb = 2;
    else if (bytes <= (UQUAD)4   * 1024 * 1024 * 1024)  spb = 4;
    else                                                spb = 8;

    pi->block_size        = 512;
    pi->sectors_per_block = spb;
    pi->max_transfer      = 0x0001FE00UL;
    pi->mask              = 0x7FFFFFFEUL;
}

static void find_free_range(const struct RDBInfo *rdb, ULONG *lo, ULONG *hi)
{
    /* Sort partition ranges by low_cyl (insertion sort - n is small),
       then scan for the first gap including holes left by deleted partitions.
       MBR partitions are also included so their cylinder range is not
       offered as free space for new RDB partitions. */
    ULONG  starts[MAX_PARTITIONS + MBR_MAX_PARTS];
    ULONG  ends  [MAX_PARTITIONS + MBR_MAX_PARTS];
    UWORD  n = rdb->num_parts;
    UWORD  i, j;
    ULONG  cursor;

    for (i = 0; i < rdb->num_parts; i++) {
        starts[i] = rdb->parts[i].low_cyl;
        ends[i]   = rdb->parts[i].high_cyl;
    }

    if (s_mbr && s_mbr->valid) {
        ULONG heads   = (rdb->heads   > 0) ? rdb->heads   : 1;
        ULONG sectors = (rdb->sectors > 0) ? rdb->sectors : 1;
        UWORD k;
        for (k = 0; k < MBR_MAX_PARTS; k++) {
            if (!s_mbr->parts[k].present) continue;
            starts[n] = MBR_LBAToCyl(s_mbr->parts[k].lba_start,
                                      heads, sectors);
            ends[n]   = MBR_LBAToCyl(s_mbr->parts[k].lba_start
                                      + s_mbr->parts[k].lba_size - 1,
                                      heads, sectors);
            n++;
        }
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (starts[j] < starts[i]) {
                ULONG t;
                t = starts[i]; starts[i] = starts[j]; starts[j] = t;
                t = ends[i];   ends[i]   = ends[j];   ends[j]   = t;
            }
        }
    }

    cursor = rdb->lo_cyl;
    for (i = 0; i < n; i++) {
        if (starts[i] > cursor) {
            *lo = cursor;
            *hi = starts[i] - 1;
            return;
        }
        if (ends[i] + 1 > cursor)
            cursor = ends[i] + 1;
    }
    *lo = cursor;
    *hi = rdb->hi_cyl;
}


/* ------------------------------------------------------------------ */
/* Hit-test: which partition block contains map x-coordinate           */
/* Returns partition index, or -1 if none.                             */
/* ------------------------------------------------------------------ */

static WORD hit_test_partition(const struct RDBInfo *rdb,
                                WORD mx, UWORD mw, ULONG total,
                                WORD mouse_x)
{
    UWORD i;
    for (i = 0; i < rdb->num_parts; i++) {
        WORD lx = (WORD)(mx + (WORD)((UQUAD)rdb->parts[i].low_cyl      * mw / total));
        WORD rx = (WORD)(mx + (WORD)((UQUAD)(rdb->parts[i].high_cyl+1) * mw / total));
        if (mouse_x >= lx && mouse_x < rx) return (WORD)i;
    }
    return -1;
}

/* Hit-test: which MBR partition bar contains map x-coordinate.
   Returns listview index (rdb->num_parts + ordinal-of-present-slot) or -1. */
static WORD hit_test_mbr_partition(const struct RDBInfo *rdb,
                                    WORD mx, UWORD mw, ULONG total,
                                    WORD mouse_x)
{
    UWORD i, n;
    if (!s_mbr || !s_mbr->valid || !rdb || rdb->heads == 0 || rdb->sectors == 0)
        return -1;
    n = rdb->num_parts;
    for (i = 0; i < MBR_MAX_PARTS; i++) {
        if (!s_mbr->parts[i].present) continue;
        {
            ULONG lo = MBR_LBAToCyl(s_mbr->parts[i].lba_start,
                                     rdb->heads, rdb->sectors);
            ULONG hi = MBR_LBAToCyl(s_mbr->parts[i].lba_start +
                                     s_mbr->parts[i].lba_size - 1,
                                     rdb->heads, rdb->sectors);
            WORD  lx = (WORD)(mx + (WORD)((UQUAD)lo       * mw / total));
            WORD  rx = (WORD)(mx + (WORD)((UQUAD)(hi + 1) * mw / total));
            if (mouse_x >= lx && mouse_x < rx) return (WORD)n;
        }
        n++;
    }
    return -1;
}

/* Hit-test: find which partition edge is at map x-coordinate          */
/*                                                                     */
/* Returns partition index and sets *edge_out (0=left, 1=right),      */
/* or -1 if no edge within tolerance.                                  */
/* mx = map inner left, mw = map inner width, total = hi_cyl+1        */
/* ------------------------------------------------------------------ */

#define DRAG_TOL 5   /* pixel tolerance for edge hit */

static WORD hit_test_edge(const struct RDBInfo *rdb,
                           WORD mx, UWORD mw, ULONG total,
                           WORD mouse_x, WORD *edge_out)
{
    UWORD i;
    for (i = 0; i < rdb->num_parts; i++) {
        WORD lx = (WORD)(mx + (WORD)((UQUAD)rdb->parts[i].low_cyl  * mw / total));
        WORD rx = (WORD)(mx + (WORD)((UQUAD)(rdb->parts[i].high_cyl+1) * mw / total));
        WORD dl = mouse_x - lx; if (dl < 0) dl = -dl;
        WORD dr = mouse_x - rx; if (dr < 0) dr = -dr;
        if (dl <= DRAG_TOL) { *edge_out = 0; return (WORD)i; }
        if (dr <= DRAG_TOL) { *edge_out = 1; return (WORD)i; }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Main window layout - extracted so it can be rebuilt on resize       */
/* ------------------------------------------------------------------ */

struct PartLayout {
    WORD  ix, iy; UWORD iw;
    WORD  bx, by; UWORD bw, bh;
    WORD  hx, hy; UWORD hw;
};

/*
 * Build (or rebuild) the main window gadget list from current window dimensions.
 * vi must remain valid for the gadgets' lifetime.
 * Returns TRUE on success; on failure, frees any partial gadget list internally.
 */
static BOOL build_gadgets(APTR vi,
                           UWORD win_w, UWORD win_h,
                           UWORD bor_l, UWORD bor_t,
                           UWORD bor_r, UWORD bor_b,
                           UWORD font_h,
                           struct TextAttr *font_ta,
                           ULONG rdb_flags,
                           struct Gadget **out_glist,
                           struct Gadget **out_lv_gad,
                           struct Gadget **out_lastdisk_gad,
                           struct Gadget **out_lastlun_gad,
                           struct PartLayout *lay)
{
    struct Gadget  *gctx = NULL, *glist = NULL, *lv = NULL, *prev;
    struct Gadget  *ldisk = NULL, *llun = NULL;
    struct NewGadget ng;
    struct TagItem   bt[] = { { TAG_DONE, 0 } };
    UWORD inner_w = win_w - bor_l - bor_r;
    UWORD pad     = 4;
    UWORD info_h  = font_h * 3 + 8;
    UWORD map_h   = 40;
    UWORD lbl_h   = font_h + 4;
    UWORD hdr_h   = font_h + 3;
    UWORD btn_h   = font_h + 6;
    UWORD row_h   = font_h + 2;
    /* Buttons anchored to the bottom; listview fills remaining space */
    UWORD btn_y   = win_h - bor_b - pad - btn_h;
    UWORD lv_top;
    UWORD lv_h;
    UWORD eighth_unused_; /* replaced by eighth inside button block */

    lay->ix = (WORD)(bor_l + pad);
    lay->iy = (WORD)(bor_t + pad);
    lay->iw = inner_w - pad * 2;
    lay->bx = (WORD)(bor_l + pad);
    lay->by = (WORD)(bor_t + pad + info_h + pad);
    lay->bw = inner_w - pad * 2;
    lay->bh = map_h;
    lay->hx = (WORD)(bor_l + pad);
    lay->hy = (WORD)(bor_t + pad + info_h + pad + map_h + lbl_h + pad);
    lay->hw = inner_w - pad * 2;

    lv_top = (UWORD)(lay->hy + (WORD)hdr_h);
    lv_h   = (btn_y > lv_top + pad + row_h * 2)
             ? (btn_y - pad - lv_top) : row_h * 2;
    lv_h   = (lv_h / row_h) * row_h;   /* snap to whole rows */

    /* Compute pixel column positions from the actual font metrics.
       Opens font_ta temporarily to measure text widths. */
    {
        struct TextFont *tf = OpenFont(font_ta);
        if (tf) {
            struct RastPort rp;
            UWORD gap = 6;  /* inter-column gap in pixels */
            UWORD cx  = 4;  /* left margin */
            /* Helper: max of two text widths */
#define MAXW(a,al,b,bl) \
    (TextLength(&rp,(a),(al)) > TextLength(&rp,(b),(bl)) \
     ? (UWORD)TextLength(&rp,(a),(al)) : (UWORD)TextLength(&rp,(b),(bl)))

            UWORD markw, drivew, locylw, hicylw, fsw, sizew, bootw;
            UWORD fixed, avail, drive_min;

            InitRastPort(&rp);
            SetFont(&rp, tf);

            markw  = (UWORD)TextLength(&rp, ">", 1);
            locylw = MAXW("9999999", 7, "Lo Cyl", 6);
            hicylw = MAXW("9999999", 7, "Hi Cyl", 6);
            fsw    = MAXW("FFS+IntlOFS ", 12, "FileSystem", 10);
            sizew  = MAXW("1000.0 MB", 9, "Size", 4);
            bootw  = MAXW("* -128", 6, "Boot", 4);

            /* The Drive (partition name) column fills the slack between the
               fixed-width columns and the right edge of the listview, so long
               names get as much room as the window allows and stop overlapping
               the cylinder columns.  build_gadgets() re-runs on IDCMP_NEWSIZE,
               so widening the window widens this column.  Clamped to a sensible
               minimum (old fixed width) when the window is narrow; lv_render()
               additionally truncates names that still don't fit. */
            drive_min = MAXW("DH10    ", 8, "Drive", 5);
            avail = (UWORD)(inner_w - pad * 2);     /* listview gadget width   */
            avail = (avail > 8) ? (UWORD)(avail - 8) : avail; /* frame margin  */
            fixed = (UWORD)(cx + markw + locylw + hicylw + fsw + sizew + bootw
                            + gap * 6 + 8);
            drivew = (avail > (UWORD)(fixed + drive_min))
                     ? (UWORD)(avail - fixed) : drive_min;

            lv_cols[LVCOL_MARK].x = cx;  lv_cols[LVCOL_MARK].w = markw;
            cx += markw + gap;

            lv_cols[LVCOL_DRIVE].x = cx; lv_cols[LVCOL_DRIVE].w = drivew;
            cx += drivew + gap;

            lv_cols[LVCOL_LOCYL].x = cx; lv_cols[LVCOL_LOCYL].w = locylw;
            cx += locylw + gap;

            lv_cols[LVCOL_HICYL].x = cx; lv_cols[LVCOL_HICYL].w = hicylw;
            cx += hicylw + gap;

            lv_cols[LVCOL_FS].x = cx;    lv_cols[LVCOL_FS].w = fsw;
            cx += fsw + gap;

            lv_cols[LVCOL_SIZE].x = cx;  lv_cols[LVCOL_SIZE].w = sizew;
            cx += sizew + gap + 8;

            lv_cols[LVCOL_BOOT].x = cx;  lv_cols[LVCOL_BOOT].w = bootw;

#undef MAXW
            CloseFont(tf);
        }
    }

    /* Set up the render hook - lv_render() captures a0/a1/a2 via
       register-variable declarations at function entry */
    lv_hook.h_Entry    = (HOOKFUNC)lv_render;
    lv_hook.h_SubEntry = NULL;
    lv_hook.h_Data     = NULL;

    gctx = CreateContext(&glist);
    if (!gctx) return FALSE;

    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr   = font_ta;

    /* Partition listview - render hook draws columns at computed pixel positions */
    {
        struct TagItem lt[] = {
            { GTLV_Labels,   (ULONG)&part_list  },
            { GTLV_CallBack, (ULONG)&lv_hook    },
            { TAG_DONE, 0 }
        };
        ng.ng_LeftEdge   = bor_l + pad;
        ng.ng_TopEdge    = (WORD)lv_top;
        ng.ng_Width      = inner_w - pad * 2;
        ng.ng_Height     = lv_h;
        ng.ng_GadgetText = NULL;
        ng.ng_GadgetID   = GID_PARTLIST;
        ng.ng_Flags      = 0;
        lv = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lt);
        if (!lv) { FreeGadgets(glist); return FALSE; }
    }
    prev = lv;

    /* Button row */
    {
    UWORD eighth = (inner_w - pad * 2 - pad * 7) / 8;
    ng.ng_TopEdge = (WORD)btn_y;
    ng.ng_Height  = btn_h;
    ng.ng_Width   = eighth;

#define MKBTN(lx,txt,gid) \
    ng.ng_LeftEdge=(lx); ng.ng_GadgetText=(txt); ng.ng_GadgetID=(gid); \
    prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); \
    if (!prev) { FreeGadgets(glist); return FALSE; }

    MKBTN(bor_l+pad,                     GS(MSG_PV_BTN_INITRDB), GID_INITRDB)
    MKBTN(bor_l+pad+(eighth+pad)*1,      GS(MSG_PV_BTN_ADD),     GID_ADD)
    MKBTN(bor_l+pad+(eighth+pad)*2,      GS(MSG_PV_BTN_EDIT),    GID_EDIT)
    MKBTN(bor_l+pad+(eighth+pad)*3,      GS(MSG_PV_BTN_DELETE),  GID_DELETE)
    MKBTN(bor_l+pad+(eighth+pad)*4,      GS(MSG_PV_BTN_MOVE),    GID_MOVE)
    MKBTN(bor_l+pad+(eighth+pad)*5,      GS(MSG_PV_BTN_FILESYS), GID_FILESYS)
    MKBTN(bor_l+pad+(eighth+pad)*6,      GS(MSG_PV_BTN_WRITE),   GID_WRITE)
    MKBTN(bor_l+pad+(eighth+pad)*7,      GS(MSG_PV_BTN_BACK),    GID_BACK)
#undef MKBTN
    }

    /* Last Disk / Last LUN checkboxes - right-aligned on info line 3 */
    {
        struct TagItem cbt[] = { { GTCB_Checked, 0 }, { TAG_DONE, 0 } };
        /* cbw must match the formula used in draw_info for the clip margin */
        UWORD cbw   = (UWORD)(font_h * 2 + 82);
        WORD  chk_y = (WORD)(bor_t + pad + (WORD)(font_h + 2) * 2);
        UWORD chk_h = (UWORD)(font_h + 2);
        WORD  cb_right = (WORD)(bor_l + inner_w - pad); /* right edge of iw */

        cbt[0].ti_Data = (rdb_flags & RDBFF_LAST) ? 1UL : 0UL;
        ng.ng_LeftEdge   = cb_right - (WORD)(cbw * 2 + pad * 3);
        ng.ng_TopEdge    = chk_y;
        ng.ng_Width      = cbw;
        ng.ng_Height     = chk_h;
        ng.ng_GadgetText = GS(MSG_PV_CHK_LASTDISK);
        ng.ng_GadgetID   = GID_LASTDISK;
        ng.ng_Flags      = PLACETEXT_RIGHT;
        ldisk = CreateGadgetA(CHECKBOX_KIND, prev, &ng, cbt);
        if (!ldisk) { FreeGadgets(glist); return FALSE; }
        prev = ldisk;

        cbt[0].ti_Data = (rdb_flags & RDBFF_LASTLUN) ? 1UL : 0UL;
        ng.ng_LeftEdge   = cb_right - (WORD)cbw;
        ng.ng_TopEdge    = chk_y;
        ng.ng_Width      = cbw;
        ng.ng_Height     = chk_h;
        ng.ng_GadgetText = GS(MSG_PV_CHK_LASTLUN);
        ng.ng_GadgetID   = GID_LASTLUN;
        ng.ng_Flags      = PLACETEXT_RIGHT;
        llun = CreateGadgetA(CHECKBOX_KIND, prev, &ng, cbt);
        if (!llun) { FreeGadgets(glist); return FALSE; }
    }

    *out_glist        = glist;
    *out_lv_gad       = lv;
    *out_lastdisk_gad = ldisk;
    *out_lastlun_gad  = llun;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* partview_run                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Advanced menu - Backup / Restore RDB block                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

static struct NewMenu partview_menu_def[] = {
    /* Menu 0 - application */
    { NM_TITLE, DISKPART_VERTITLE,        NULL,         0, 0, NULL },
    { NM_ITEM,  "About...",              NULL,         0, 0, NULL },  /* ITEM 0 */
    /* Menu 1 - Advanced: backup / restore operations */
    { NM_TITLE, "Advanced",              NULL,         0, 0, NULL },
    { NM_ITEM,  "Backup RDB Block",      NULL,         0, 0, NULL },  /* ITEM 0 */
    { NM_ITEM,  "Restore RDB Block",     NULL,         0, 0, NULL },  /* ITEM 1 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 2 */
    { NM_ITEM,  "Extended Backup...",    NULL,         0, 0, NULL },  /* ITEM 3 */
    { NM_ITEM,  "Extended Restore...",   NULL,         0, 0, NULL },  /* ITEM 4 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 5 */
    { NM_ITEM,  "Verify RDB Block...",   NULL,         0, 0, NULL },  /* ITEM 6 */
    { NM_ITEM,  "Verify Extended...",    NULL,         0, 0, NULL },  /* ITEM 7 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 8 */
    { NM_ITEM,  "RDB Integrity Check",   NULL,         0, 0, NULL },  /* ITEM 9 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 10 */
    { NM_ITEM,  "Dump Disk to Image...", NULL,         0, 0, NULL },  /* ITEM 11 */
    { NM_ITEM,  "Restore Image to Disk...",NULL,       0, 0, NULL },  /* ITEM 12 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 13 */
    { NM_ITEM,  "Export MountList...",   NULL,         0, 0, NULL },  /* ITEM 14 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 15 */
    { NM_ITEM,  "Zero Partition...",     NULL,         0, 0, NULL },  /* ITEM 16 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 17 */
    { NM_ITEM,  "Add MBR Partition...", NULL,         0, 0, NULL },  /* ITEM 18 */
    /* Menu 2 - Health: disk diagnostics */
    { NM_TITLE, "Health",                NULL,         0, 0, NULL },
    { NM_ITEM,  "SMART Status",          NULL,         0, 0, NULL },  /* ITEM 0 */
    { NM_ITEM,  "Bad Block Scan...",     NULL,         0, 0, NULL },  /* ITEM 1 */
    /* Menu 3 - Debug: low-level inspection tools */
    { NM_TITLE, "Debug",                 NULL,         0, 0, NULL },
    { NM_ITEM,  "View RDB Block",        NULL,         0, 0, NULL },  /* ITEM 0 */
    { NM_ITEM,  "Raw Block Scan...",     NULL,         0, 0, NULL },  /* ITEM 1 */
    { NM_ITEM,  "Hex Dump Blocks...",    NULL,         0, 0, NULL },  /* ITEM 2 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 3 */
    { NM_ITEM,  "Raw Disk Read...",      NULL,         0, 0, NULL },  /* ITEM 4 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 5 */
    { NM_ITEM,  "Check FFS Root...",     NULL,         0, 0, NULL },  /* ITEM 6 */
    { NM_END,   NULL,                    NULL,         0, 0, NULL },
};

/* Replace the built-in English nm_Label strings above with their localized
   forms before the menu is created.  NM_TITLE for menu 0 keeps DISKPART_VERTITLE
   (a version constant); NM_BARLABEL separators and the NM_END terminator are
   left alone.  The message ids are listed in the same order as the table. */
static void localize_partview_menu(void)
{
    static const LONG ids[] = {
        -1,                          /* DISKPART_VERTITLE (menu 0 title)   */
        MSG_PV_MENU_ABOUT,
        MSG_PV_MENU_ADVANCED,
        MSG_PV_MENU_BACKUP_RDB,
        MSG_PV_MENU_RESTORE_RDB,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_EXT_BACKUP,
        MSG_PV_MENU_EXT_RESTORE,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_VERIFY_RDB,
        MSG_PV_MENU_VERIFY_EXT,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_INTEGRITY,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_DUMP_IMAGE,
        MSG_PV_MENU_RESTORE_IMAGE,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_EXPORT_ML,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_ZERO_PART,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_MBR_ADD,
        MSG_PV_MENU_HEALTH,
        MSG_PV_MENU_SMART,
        MSG_PV_MENU_BADBLOCK,
        MSG_PV_MENU_DEBUG,
        MSG_PV_MENU_VIEW_RDB,
        MSG_PV_MENU_RAW_SCAN,
        MSG_PV_MENU_HEX_DUMP,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_RAW_READ,
        -1,                          /* NM_BARLABEL */
        MSG_PV_MENU_CHECK_FFS,
    };
    UWORD i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (ids[i] >= 0)
            partview_menu_def[i].nm_Label = GS(ids[i]);
}

/* ------------------------------------------------------------------ */
/* format_pending_partitions - after a successful RDB write, quick-format */
/* any newly created partition the user opted to format (want_format).    */
/* left mounted so no reboot is needed.  Reports the outcome once.        */
/* Returns TRUE if any pending partition could NOT be mounted live (format  */
/* skipped or failed) - the caller should then still require a reboot.      */
/* ------------------------------------------------------------------ */
static BOOL format_pending_partitions(struct Window *win, struct BlockDev *bd,
                                      struct RDBInfo *rdb)
{
    char  report[512];
    ULONG rlen = 0;
    UWORD i;
    int   any  = 0;
    BOOL  need_reboot = FALSE;

    report[0] = '\0';
    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi = &rdb->parts[i];
        char line[160];

        if (!pi->want_format || pi->volume_name[0] == '\0') continue;
        any = 1;

        if (!bd || bd->backend == BD_FILE) {
            DP_SNPRINTF(line, GS(MSG_PV_FMT_SKIPPED_IMAGE), pi->drive_name);
            need_reboot = TRUE;   /* exists in RDB but not mounted */
        } else {
            char err[80], mounted[40];
            err[0] = '\0';
            if (QuickFormat_Partition(bd, pi, mounted, err, sizeof(err))) {
                DP_SNPRINTF(line, GS(MSG_PV_FMT_FORMATTED),
                        mounted[0] ? mounted : pi->drive_name, pi->volume_name);
            } else {
                DP_SNPRINTF(line, GS(MSG_PV_FMT_FAILED), pi->drive_name, err);
                need_reboot = TRUE;   /* not mounted - reboot to pick it up */
            }
        }
        if (rlen + strlen(line) < sizeof(report) - 1)
            rlen += (ULONG)sprintf(report + rlen, "%s", line);
        pi->want_format = 0;   /* handled - don't retry on the next write */
    }

    if (any) {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
        es.es_TextFormat   = (UBYTE *)report;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
    return need_reboot;
}

/* Redraw all gadgets (incl. the bottom button row) - call after an operation
   opened sub-windows/requesters over the main window. */
static void refresh_all_gadgets(struct Window *win, struct Gadget *glist)
{
    if (glist) {
        RefreshGList(glist, win, NULL, -1);
        GT_RefreshWindow(win, NULL);
    }
}

/* Update the window titlebar to flag unsaved (pending-write) changes. */
static void set_title_dirty(struct Window *win, const char *devname, ULONG unit,
                            BOOL dirty)
{
    static char t[96];
    int n = DP_SNPRINTF(t, "%s", DISKPART_VERTITLE);
    sprintf(t + n, GS(MSG_PV_TITLE_UNIT_FMT),
            devname, (unsigned long)unit,
            dirty ? GS(MSG_PV_TITLE_UNSAVED) : "");
    SetWindowTitles(win, (UBYTE *)t, (UBYTE *)~0UL);  /* leave screen title */
}

/* RDB_Write progress hook: shows "Writing/Verifying block N/M" in the
   titlebar while a write is in progress. Installed for the lifetime of the
   window (see partview_run) so a long ADDFS write - which can touch well
   over a hundred LSEG blocks on real hardware - is visible rather than
   looking like a hang, and so the last block shown pinpoints a stall. */
static void write_progress_title(void *ud, ULONG done, ULONG total,
                                 const char *phase)
{
    struct Window *win = (struct Window *)ud;
    static char t[96];
    DP_SNPRINTF(t, "%s block %lu/%lu...", phase,
                (unsigned long)done, (unsigned long)total);
    SetWindowTitles(win, (UBYTE *)t, (UBYTE *)~0UL);
}

/* Case-insensitive equality for drive names. */
static BOOL name_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return FALSE;
    }
    return (*a == '\0' && *b == '\0');
}

/* ------------------------------------------------------------------ */
/* unmount_deleted_partitions - after a successful RDB write, unmount any   */
/* partition deleted this session so the change takes effect without a      */
/* reboot.  Skips names that were re-added.  Returns TRUE if any device     */
/* could not be unmounted (in use) - the caller should still require reboot.*/
/* ------------------------------------------------------------------ */
static BOOL unmount_deleted_partitions(struct Window *win, struct RDBInfo *rdb)
{
    char  report[512];
    ULONG rlen = 0;
    UWORD i, k;
    int   any = 0;
    BOOL  need_reboot = FALSE;

    report[0] = '\0';
    for (i = 0; i < s_unmount_count; i++) {
        const char *nm = s_unmount_names[i];
        char  line[128], err[80];
        BOOL  re_added = FALSE;

        if (!nm[0]) continue;
        for (k = 0; k < rdb->num_parts; k++)
            if (name_eq_ci(rdb->parts[k].drive_name, nm)) { re_added = TRUE; break; }
        if (re_added) continue;   /* name reused by a new partition - leave it */

        any = 1;
        err[0] = '\0';
        if (UnmountDevice(nm, err, sizeof(err))) {
            DP_SNPRINTF(line, GS(MSG_PV_UNMOUNTED), nm);
        } else {
            DP_SNPRINTF(line, GS(MSG_PV_STILL_MOUNTED), nm, err);
            need_reboot = TRUE;
        }
        if (rlen + strlen(line) < sizeof(report) - 1)
            rlen += (ULONG)sprintf(report + rlen, "%s", line);
    }
    s_unmount_count = 0;

    if (any) {
        struct EasyStruct es;
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
        es.es_TextFormat   = (UBYTE *)report;
        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
        EasyRequest(win, &es, NULL);
    }
    return need_reboot;
}

/* ------------------------------------------------------------------ */
/* load_window_geom - read a "WINDOW=left/top/width/height" tooltype   */
/* from the program's own Workbench icon, if launched from Workbench   */
/* and the tooltype is present and well-formed.  Read-only - the user  */
/* sets this by hand via the icon's Information window (same as        */
/* NOWARNING).  Returns FALSE (geometry untouched) on any failure.     */
/* ------------------------------------------------------------------ */
static BOOL load_window_geom(WORD *x, WORD *y, UWORD *w, UWORD *h)
{
    struct DiskObject *dobj;
    BPTR    prev_dir;
    STRPTR  val;
    char   *p, *end;
    long    vals[4];
    int     i;

    if (!IconBase || !DiskPart_WBStartup || DiskPart_WBStartup->sm_NumArgs < 1)
        return FALSE;

    {
        struct WBArg *wa = &DiskPart_WBStartup->sm_ArgList[0];
        prev_dir = CurrentDir(wa->wa_Lock);
        dobj = GetDiskObject((STRPTR)wa->wa_Name);
        CurrentDir(prev_dir);
    }
    if (!dobj) return FALSE;

    val = FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"WINDOW");
    if (!val) { FreeDiskObject(dobj); return FALSE; }

    p = (char *)val;
    for (i = 0; i < 4; i++) {
        vals[i] = strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        if (*p == '/') p++;
    }
    FreeDiskObject(dobj);
    if (i < 4) return FALSE;

    *x = (WORD)vals[0];
    *y = (WORD)vals[1];
    *w = (UWORD)vals[2];
    *h = (UWORD)vals[3];
    return TRUE;
}

/* quick_double_click() / LIST_DBLCLICK_MAX_US now live in guilv.h/guilv.c,
   shared with main.c's device-select and unit-select windows. */

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* check_ffs_root - show what FFS would find at the expected root      */
/* block position for the selected partition.  Useful post-reboot to   */
/* verify the grown filesystem structure is intact on disk.            */
/* ------------------------------------------------------------------ */
BOOL partview_run(const char *devname, ULONG unit)
{
    struct BlockDev  *bd       = NULL;
    struct RDBInfo   *rdb      = NULL;
    struct Screen    *scr      = NULL;
    APTR              vi       = NULL;
    struct Gadget    *glist         = NULL;
    struct Gadget    *lv_gad        = NULL;
    struct Gadget    *lastdisk_gad  = NULL;
    struct Gadget    *lastlun_gad   = NULL;
    struct Window    *win      = NULL;
    struct Menu      *menu     = NULL;
    WORD              sel      = -1;
    BOOL              dirty        = FALSE;  /* unsaved changes pending */
    BOOL              needs_reboot = FALSE;  /* partition layout changed */
    BOOL              exit_req     = FALSE;
    WORD              i;
    static struct MBRInfo mbr_store;        /* MBR data for this session */

    s_unmount_count = 0;     /* no deletes pending unmount yet this session */
    s_mbr = NULL;            /* no MBR loaded yet */
    /* g_part_sel is a static shared across partview_run() sessions (once
       per device/unit opened) - reset unconditionally, before any
       early-return path, so a selection from a previous disk never shows
       highlighted on a freshly-opened one. */
    g_part_sel = -1;
    static char       wfmt[512];            /* formatted write-fail message - static: off stack */
    static char       win_title[80];

    /* Custom pointer - chip RAM copy of ptr_resize_src, NULL if alloc failed */
    UWORD            *ptr_chip   = NULL;
    BOOL              ptr_custom = FALSE;   /* TRUE while SetPointer is active */

    /* Drag resize state */
    WORD  drag_part    = -1;   /* -1 = not dragging */
    WORD  drag_edge    = 0;    /* 0 = left (low_cyl), 1 = right (high_cyl) */
    ULONG drag_min     = 0;
    ULONG drag_max     = 0;
    ULONG drag_orig_lo = 0;   /* saved low_cyl  before drag */
    ULONG drag_orig_hi = 0;   /* saved high_cyl before drag */

    /* Double-click detection in map - RDB partitions */
    ULONG dbl_sec   = 0;
    ULONG dbl_mic   = 0;
    WORD  dbl_part  = -1;   /* partition clicked last time */

    /* Double-click detection in map - MBR partitions */
    ULONG dbl_mbr_sec = 0;
    ULONG dbl_mbr_mic = 0;
    WORD  dbl_mbr_idx = -1; /* listview index of last-clicked MBR bar */

    /* Double-click detection for the text listview - own item+timing check
       (quick_double_click), not Intuition's IEQUALIFIER_DOUBLECLICK. See
       quick_double_click() above for why. */
    WORD  dbl_list_sel = -1;
    ULONG dbl_list_sec  = 0;
    ULONG dbl_list_mic  = 0;

    /* Drag-move state (move whole partition by dragging its body) */
    WORD  drag_move_part   = -1;   /* -1 = not active */
    ULONG drag_move_orig_lo = 0;
    ULONG drag_move_orig_hi = 0;
    ULONG drag_move_width   = 0;   /* hi - lo, preserved during move */
    ULONG drag_move_min_lo  = 0;   /* minimum allowed lo_cyl */
    ULONG drag_move_max_lo  = 0;   /* maximum allowed lo_cyl */
    WORD  drag_move_anchor_x  = 0; /* pixel x where drag began */
    ULONG drag_move_anchor_cyl = 0; /* cylinder at anchor pixel */

    /* New-partition drag state */
    BOOL  drag_new       = FALSE;
    ULONG drag_new_lo    = 0;
    ULONG drag_new_hi    = 0;
    ULONG drag_new_start = 0;
    ULONG drag_new_min   = 0;   /* free-space left boundary */
    ULONG drag_new_max   = 0;   /* free-space right boundary */

    /* Layout coordinates filled in below */
    WORD  ix = 0, iy = 0;  UWORD iw = 0;           /* info section  */
    WORD  bx = 0, by = 0;  UWORD bw = 0, bh = 0;   /* map           */
    WORD  hx = 0, hy = 0;  UWORD hw = 0;            /* col header    */

    for (i = 0; i < NUM_PART_COLORS; i++) part_pens[i] = -1;
    bg_pen = rdb_pen = mbr_pen = -1;

    /* ---- Open device, read RDB, get geometry if needed ---- */
    bd = BlockDev_Open(devname, unit);

    if (bd && !BlockDev_IsHardDisk(bd)) {
        struct EasyStruct es;
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)DISKPART_VERTITLE;
        es.es_TextFormat=(UBYTE*)GS(MSG_PV_NOT_A_HARDDISK);
        es.es_GadgetFormat=(UBYTE*)GS(MSG_OK);
        EasyRequest(NULL, &es, NULL);
        goto cleanup;
    }

    rdb = (struct RDBInfo *)AllocVec(sizeof(*rdb), MEMF_PUBLIC | MEMF_CLEAR);
    if (!rdb) goto cleanup;

    if (bd) {
        RDB_Read(bd, rdb);
        /* nothing extra - names and DosTypes come from disk (PART/FSHD blocks) */
        if (!rdb->valid && bd) {
            ULONG cyls = 0, heads = 0, secs = 0;
            if (BlockDev_GetGeometry(bd, &cyls, &heads, &secs)) {
                rdb->cylinders = cyls;
                rdb->heads     = heads;
                rdb->sectors   = secs;
            }
        }
        /* Read MBR (non-fatal - just leaves mbr_store.valid = FALSE). */
        if (MBR_Read(bd, &mbr_store))
            s_mbr = &mbr_store;
    }

    build_part_list(rdb, sel);

    /* ---- Lock screen ---- */
    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;

    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto cleanup;

    alloc_pens(scr);

    /* ---- Open window first (no gadgets) to learn the actual border sizes.
            WFLG_SIZEBBOTTOM expands BorderBottom beyond scr->WBorBottom, so
            we cannot compute correct gadget positions until after open. ---- */
    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD pad     = 4;
        UWORD info_h  = font_h * 3 + 8;
        UWORD map_h   = 40;
        UWORD lbl_h   = font_h + 4;
        UWORD hdr_h   = font_h + 3;
        UWORD btn_h   = font_h + 6;
        UWORD row_h   = font_h + 2;
        UWORD win_w   = 560;
        /* Estimate bottom border generously: scr->WBorBottom + font height + a few
           pixels for the size gadget.  Overestimating just gives extra listview
           rows; underestimating would clip the buttons. */
        UWORD bor_b_est = (UWORD)scr->WBorBottom + font_h + 4;
        UWORD fixed_est = bor_t + pad + info_h + pad + map_h + lbl_h
                        + pad + hdr_h + pad + btn_h + pad + bor_b_est;
        UWORD win_h   = fixed_est + row_h * 8;
        UWORD min_w   = bor_l + bor_r + pad * 2 + 7 * (40 + pad) - pad;
        UWORD min_h   = fixed_est + row_h * 2;

        /* Saved tooltype geometry, if present, well-formed, and it actually
           fits the current screen - otherwise keep the centered default. */
        WORD  geom_x, geom_y;
        UWORD geom_w, geom_h;
        ULONG win_left = (ULONG)((scr->Width  - win_w) / 2);
        ULONG win_top  = (ULONG)((scr->Height - win_h) / 2);

        if (load_window_geom(&geom_x, &geom_y, &geom_w, &geom_h) &&
            geom_x >= 0 && geom_y >= 0 &&
            geom_w >= min_w && geom_h >= min_h &&
            (ULONG)geom_x + geom_w <= (ULONG)scr->Width &&
            (ULONG)geom_y + geom_h <= (ULONG)scr->Height) {
            win_left = (ULONG)geom_x;
            win_top  = (ULONG)geom_y;
            win_w    = geom_w;
            win_h    = geom_h;
        }

        {
            int n = DP_SNPRINTF(win_title, "%s", DISKPART_VERTITLE);
            sprintf(win_title + n, GS(MSG_PV_TITLE_UNIT_FMT),
                    devname, (unsigned long)unit, "");
        }

        {
            struct TagItem wt[] = {
                { WA_Left,      win_left },
                { WA_Top,       win_top },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)win_title },
                { WA_Gadgets,   NULL },          /* added after open, see below */
                { WA_PubScreen, (ULONG)scr },
                { WA_MinWidth,  min_w },
                { WA_MinHeight, min_h },
                { WA_MaxWidth,  (ULONG)scr->Width  },
                { WA_MaxHeight, (ULONG)scr->Height },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                IDCMP_GADGETDOWN  | IDCMP_REFRESHWINDOW |
                                IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE |
                                IDCMP_NEWSIZE | IDCMP_MENUPICK },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                WFLG_SIMPLE_REFRESH | WFLG_REPORTMOUSE |
                                WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto cleanup;
    RDB_SetWriteProgressHook(write_progress_title, win);

    /* ---- Build gadgets from the window's actual border sizes ---- */
    {
        struct PartLayout lay;
        UWORD fh = (UWORD)win->WScreen->Font->ta_YSize;

        if (!build_gadgets(vi,
                           (UWORD)win->Width,       (UWORD)win->Height,
                           (UWORD)win->BorderLeft,  (UWORD)win->BorderTop,
                           (UWORD)win->BorderRight, (UWORD)win->BorderBottom,
                           fh, win->WScreen->Font, rdb->flags,
                           &glist, &lv_gad, &lastdisk_gad, &lastlun_gad, &lay))
            goto cleanup;

        ix = lay.ix; iy = lay.iy; iw = lay.iw;
        bx = lay.bx; by = lay.by; bw = lay.bw; bh = lay.bh;
        hx = lay.hx; hy = lay.hy; hw = lay.hw;

        AddGList(win, glist, (UWORD)-1, -1, NULL);
        RefreshGList(glist, win, NULL, -1);

        /* Now that we know the real border sizes, set precise size limits.
           The estimate used for WA_MinHeight at open time may have been off
           because WFLG_SIZEBBOTTOM enlarges BorderBottom unpredictably. */
        {
            UWORD pad2     = 4;
            UWORD info_h2  = fh * 3 + 8;
            UWORD map_h2   = 40;
            UWORD lbl_h2   = fh + 4;
            UWORD hdr_h2   = fh + 3;
            UWORD btn_h2   = fh + 6;
            UWORD row_h2   = fh + 2;
            UWORD fixed2   = (UWORD)win->BorderTop
                           + pad2 + info_h2 + pad2 + map_h2 + lbl_h2
                           + pad2 + hdr_h2  + pad2 + btn_h2
                           + pad2 + (UWORD)win->BorderBottom;
            WORD  min_h2   = (WORD)(fixed2 + row_h2 * 2);
            WORD  min_w2   = (WORD)((UWORD)win->BorderLeft + (UWORD)win->BorderRight
                                    + pad2 * 2 + 7 * (40 + pad2) - pad2);
            WindowLimits(win, min_w2, (WORD)win->WScreen->Width,
                              min_h2, (WORD)win->WScreen->Height);
        }
    }

    {
        struct TagItem lt[] = { { TAG_DONE, 0 } };
        localize_partview_menu();
        menu = CreateMenusA(partview_menu_def, NULL);
        if (menu) {
            LayoutMenusA(menu, vi, lt);
            SetMenuStrip(win, menu);
        }
    }

    /* Allocate chip RAM copy of the resize pointer sprite */
    ptr_chip = (UWORD *)AllocVec(sizeof(ptr_resize_src), MEMF_CHIP);
    if (ptr_chip)
        CopyMem((APTR)ptr_resize_src, (APTR)ptr_chip, sizeof(ptr_resize_src));

    GT_RefreshWindow(win, NULL);
    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                ix, iy, iw, bx, by, bw, bh, hx, hy, hw, sel, lastdisk_gad, lastlun_gad);

    /* ---- Event loop ---- */
    {
        BOOL running = TRUE;
        BOOL last_dirty = FALSE;   /* tracks titlebar "unsaved changes" marker */
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG          iclass  = imsg->Class;
                UWORD          code    = imsg->Code;
                WORD           mouse_x = imsg->MouseX;
                WORD           mouse_y = imsg->MouseY;
                ULONG          ev_sec  = imsg->Seconds;
                ULONG          ev_mic  = imsg->Micros;
                struct Gadget *gad     = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (iclass) {

                case IDCMP_MENUPICK: {
                    UWORD mcode = code;
                    while (mcode != MENUNULL) {
                        struct MenuItem *it = ItemAddress(menu, mcode);
                        if (!it) break;
                        if (MENUNUM(mcode) == 0 && ITEMNUM(mcode) == 0)
                            show_about(win);
                        /* Advanced menu */
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 0)
                            rdb_backup_block(win, bd, rdb);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 1)
                            rdb_restore_block(win, bd);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 3)
                            rdb_backup_extended(win, bd, rdb);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 4)
                            rdb_restore_extended(win, bd);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 6)
                            rdb_verify_block(win, bd, rdb);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 7)
                            rdb_verify_extended(win, bd);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 9)
                            rdb_integrity_check(win, bd, rdb);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 11)
                            image_dump_disk(win, bd);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 12)
                            image_restore_disk(win, bd);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 14)
                            pv_export_mountlist(win, bd, rdb);
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 16) {
                            if (sel >= 0 && bd && rdb && sel < (WORD)rdb->num_parts)
                                offer_zero_partition(win, bd, rdb, &rdb->parts[sel]);
                            else if (sel >= (WORD)rdb->num_parts && bd && rdb &&
                                     s_mbr && s_mbr->valid)
                                offer_zero_mbr_part(win, bd, rdb, s_mbr,
                                                    part_mbr_slot[sel]);
                            else {
                                struct EasyStruct es;
                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)GS(MSG_ZERO_CONFIRM_TITLE);
                                es.es_TextFormat   = (UBYTE *)GS(MSG_ZERO_NO_PART_SEL);
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                EasyRequest(win, &es, NULL);
                            }
                        }
                        else if (MENUNUM(mcode) == 1 && ITEMNUM(mcode) == 18) {
                            offer_add_mbr_part(win, bd, s_mbr, rdb);
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                        }
                        /* Health menu */
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 0)
                            smart_status(win, bd);
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 1)
                            bad_block_scan(win, bd, rdb);
                        /* Debug menu */
                        else if (MENUNUM(mcode) == 3 && ITEMNUM(mcode) == 0)
                            rdb_view_block(win, bd, rdb);
                        else if (MENUNUM(mcode) == 3 && ITEMNUM(mcode) == 1)
                            rdb_raw_scan(win, bd);
                        else if (MENUNUM(mcode) == 3 && ITEMNUM(mcode) == 2)
                            raw_hex_dump(win, bd);
                        else if (MENUNUM(mcode) == 3 && ITEMNUM(mcode) == 4)
                            raw_disk_read(win, bd);
                        else if (MENUNUM(mcode) == 3 && ITEMNUM(mcode) == 6)
                            check_ffs_root(win, bd, rdb, sel);
                        mcode = it->NextSelect;
                    }
                    break;
                }

                case IDCMP_CLOSEWINDOW: {
                    struct EasyStruct es;
                    LONG r;
                    es.es_StructSize = sizeof(es);
                    es.es_Flags      = 0;
                    es.es_Title      = (UBYTE *)DISKPART_VERTITLE;
                    if (dirty) {
                        es.es_TextFormat   = (UBYTE *)GS(MSG_PV_UNSAVED_BODY);
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_WRITE_DISCARD_CANCEL);
                        r = EasyRequest(win, &es, NULL);
                        if (r == 0) break;           /* Cancel - stay */
                        if (r == 1 && bd) {          /* Write */
                            if (RDB_Write(bd, rdb)) {
                                dirty = FALSE;
                                if (needs_reboot) {
                                    offer_reboot(win, GS(MSG_PV_REBOOT_WRITTEN));
                                    needs_reboot = FALSE;
                                }
                            } else {
                                DP_SNPRINTF(wfmt, GS(MSG_PV_WRITE_FAILED_FMT),
                                        (int)bd->last_io_err);
                                es.es_TextFormat   = (UBYTE *)wfmt;
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                EasyRequest(win, &es, NULL);
                                break; /* stay open */
                            }
                        }
                        /* r == 2: Discard - fall through to exit */
                    } else if (needs_reboot) {
                        /* RDB was already written outside the dirty path
                           (e.g. by Partition Move, which writes RDB itself). */
                        offer_reboot(win, GS(MSG_PV_REBOOT_LAYOUT));
                        needs_reboot = FALSE;
                    } else {
                        es.es_TextFormat   = (UBYTE *)GS(MSG_PV_EXIT_BODY);
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_YES_NO);
                        if (EasyRequest(win, &es, NULL) != 1) break;
                    }
                    exit_req = TRUE;
                    running  = FALSE;
                    break;
                }

                case IDCMP_MOUSEBUTTONS:
                    if (code == SELECTDOWN) {
                        /* Hit-test partition edges/blocks in the map area */
                        if (rdb && rdb->valid &&
                            mouse_y >= by && mouse_y <= by + (WORD)bh)
                        {
                            WORD  mx2   = bx + 1;
                            UWORD mw2   = bw - 2;
                            ULONG total = rdb->hi_cyl + 1;

                            /* Decide intent. Priority:
                             *   click inside partition near right edge -> resize
                             *   click inside partition near left edge  -> "can't resize from start" dialog
                             *   click inside partition middle           -> move (or double-click -> edit)
                             *   click outside any partition near a right edge -> resize that partition
                             *   click in free space                     -> new-partition drag
                             * The inside-partition check uses an edge zone of max(DRAG_TOL, width/8)
                             * so wide partitions get a generously sized resize handle. */
                            WORD blk_in     = hit_test_partition(rdb, mx2, mw2, total, mouse_x);
                            WORD resize_part = -1;
                            WORD left_dlg_part = -1;
                            WORD move_blk   = -1;
                            WORD mbr_blk    = -1;

                            if (blk_in >= 0) {
                                WORD lx_p = (WORD)(mx2 + (WORD)((UQUAD)rdb->parts[blk_in].low_cyl       * mw2 / total));
                                WORD rx_p = (WORD)(mx2 + (WORD)((UQUAD)(rdb->parts[blk_in].high_cyl + 1) * mw2 / total));
                                WORD d_left  = (WORD)(mouse_x - lx_p);
                                WORD d_right = (WORD)((rx_p - 1) - mouse_x);
                                WORD ezone   = (WORD)((rx_p - lx_p) / 8);
                                if (d_left  < 0) d_left  = 0;
                                if (d_right < 0) d_right = 0;
                                if (ezone < DRAG_TOL) ezone = DRAG_TOL;
                                if (d_right <= ezone)      resize_part   = blk_in;
                                else if (d_left  <= ezone) left_dlg_part = blk_in;
                                else                       move_blk      = blk_in;
                            } else {
                                /* Outside any RDB partition - check RDB edge then MBR bar */
                                WORD ee = 0;
                                WORD ep = hit_test_edge(rdb, mx2, mw2, total, mouse_x, &ee);
                                if (ep >= 0 && ee == 1) resize_part = ep;
                                else mbr_blk = hit_test_mbr_partition(rdb, mx2, mw2,
                                                                       total, mouse_x);
                            }

                            if (resize_part >= 0) {
                                /* On the right edge - start resize drag, save originals */
                                WORD  part        = resize_part;
                                ULONG left_end    = rdb->lo_cyl;   /* first usable cyl */
                                ULONG right_start = rdb->hi_cyl + 1;
                                UWORD kk;

                                /* Find nearest neighbours by cylinder, not array index */
                                for (kk = 0; kk < rdb->num_parts; kk++) {
                                    if (kk == (UWORD)part) continue;
                                    if (rdb->parts[kk].high_cyl < rdb->parts[part].low_cyl) {
                                        if (rdb->parts[kk].high_cyl + 1 > left_end)
                                            left_end = rdb->parts[kk].high_cyl + 1;
                                    }
                                    if (rdb->parts[kk].low_cyl > rdb->parts[part].high_cyl) {
                                        if (rdb->parts[kk].low_cyl < right_start)
                                            right_start = rdb->parts[kk].low_cyl;
                                    }
                                }

                                drag_part      = part;
                                drag_edge      = 1;
                                drag_orig_lo   = rdb->parts[part].low_cyl;
                                drag_orig_hi   = rdb->parts[part].high_cyl;
                                drag_move_part = -1;
                                dbl_part       = -1;
                                drag_min = rdb->parts[part].low_cyl;
                                drag_max = right_start > 0 ? right_start - 1 : 0;
                            } else if (left_dlg_part >= 0) {
                                /* Left-edge drag: not supported - inform user */
                                struct EasyStruct es;
                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)GS(MSG_PV_NORESIZE_START_TITLE);
                                es.es_TextFormat   = (UBYTE *)GS(MSG_PV_NORESIZE_START_BODY);
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                EasyRequest(win, &es, NULL);
                            } else if (move_blk >= 0) {
                                /* Inside a partition block (middle) - check double-click */
                                WORD blk = move_blk;
                                if (blk == dbl_part &&
                                    DoubleClick(dbl_sec, dbl_mic,
                                                ev_sec,  ev_mic)) {
                                    /* Double-click: open Edit dialog */
                                    sel = blk;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    dbl_part = -1;
                                    {
                                        ULONG old_hi = rdb->parts[sel].high_cyl;
                                        if (partition_dialog(&rdb->parts[sel],
                                                             GS(MSG_PV_DLG_EDIT_PART), rdb, FALSE)) {
                                            {
                                            int g = offer_ffs_grow(win, bd, rdb,
                                                           &rdb->parts[sel], old_hi);
                                            if (g == GROW_NONE) g = offer_pfs_grow(win, bd, rdb,
                                                           &rdb->parts[sel], old_hi);
                                            if (g == GROW_NONE) g = offer_sfs_grow(win, bd, rdb,
                                                           &rdb->parts[sel], old_hi);
                                            dirty        = TRUE;
                                            /* Clean FFS grow remounts live; else reboot. */
                                            if (g != GROW_REMOUNTED && g != GROW_ABORTED) needs_reboot = TRUE;
                                            if (g != GROW_NONE) refresh_all_gadgets(win, glist);
                                            }
                                            refresh_listview(win, lv_gad, rdb, sel);
                                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                        ix, iy, iw, bx, by, bw, bh,
                                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                        }
                                    }
                                } else {
                                    /* Single click: select + start drag-move */
                                    ULONG left_end2   = rdb->lo_cyl;
                                    ULONG right_start2 = rdb->hi_cyl + 1;
                                    ULONG width2;
                                    UWORD kk2;
                                    sel      = blk;
                                    dbl_part = blk;
                                    dbl_sec  = ev_sec;
                                    dbl_mic  = ev_mic;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_map(win, rdb, sel, bx, by, bw, bh);

                                    /* Compute free space bounds for drag */
                                    for (kk2 = 0; kk2 < rdb->num_parts; kk2++) {
                                        if (kk2 == (UWORD)blk) continue;
                                        if (rdb->parts[kk2].high_cyl < rdb->parts[blk].low_cyl) {
                                            if (rdb->parts[kk2].high_cyl + 1 > left_end2)
                                                left_end2 = rdb->parts[kk2].high_cyl + 1;
                                        }
                                        if (rdb->parts[kk2].low_cyl > rdb->parts[blk].high_cyl) {
                                            if (rdb->parts[kk2].low_cyl < right_start2)
                                                right_start2 = rdb->parts[kk2].low_cyl;
                                        }
                                    }
                                    width2 = rdb->parts[blk].high_cyl - rdb->parts[blk].low_cyl;
                                    drag_move_part      = blk;
                                    drag_move_orig_lo   = rdb->parts[blk].low_cyl;
                                    drag_move_orig_hi   = rdb->parts[blk].high_cyl;
                                    drag_move_width     = width2;
                                    drag_move_min_lo    = left_end2;
                                    drag_move_max_lo    = (right_start2 > width2)
                                                          ? right_start2 - 1 - width2
                                                          : left_end2;
                                    drag_move_anchor_x   = mouse_x;
                                    drag_move_anchor_cyl = rdb->parts[blk].low_cyl;
                                }
                            } else if (mbr_blk >= 0 && s_mbr && s_mbr->valid) {
                                /* Click inside an MBR partition bar: select + double-click to edit */
                                sel = mbr_blk;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_map(win, rdb, sel, bx, by, bw, bh);
                                if (mbr_blk == dbl_mbr_idx &&
                                    DoubleClick(dbl_mbr_sec, dbl_mbr_mic, ev_sec, ev_mic)) {
                                    dbl_mbr_idx = -1;
                                    offer_edit_mbr_part(win, bd, s_mbr, rdb,
                                                        part_mbr_slot[mbr_blk]);
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb,
                                                (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else {
                                    dbl_mbr_idx = mbr_blk;
                                    dbl_mbr_sec = ev_sec;
                                    dbl_mbr_mic = ev_mic;
                                    dbl_part    = -1;
                                }
                            } else if (rdb->num_parts < MAX_PARTITIONS &&
                                       rdb->heads > 0 && rdb->sectors > 0) {
                                /* Empty space (not inside any RDB or MBR bar) -
                                   start new-partition drag */
                                LONG  dx = (LONG)(mouse_x - (WORD)mx2);
                                ULONG start_cyl;
                                UWORD kk;
                                if (dx < 0) dx = 0;
                                if (dx >= (LONG)mw2) dx = (LONG)(mw2 - 1);
                                start_cyl = (ULONG)((UQUAD)(ULONG)dx * total / (ULONG)mw2);
                                if (start_cyl < rdb->lo_cyl) start_cyl = rdb->lo_cyl;
                                if (start_cyl > rdb->hi_cyl) start_cyl = rdb->hi_cyl;
                                /* Find free gap containing start_cyl */
                                drag_new_min = rdb->lo_cyl;
                                drag_new_max = rdb->hi_cyl;
                                for (kk = 0; kk < rdb->num_parts; kk++) {
                                    if (rdb->parts[kk].high_cyl < start_cyl &&
                                        rdb->parts[kk].high_cyl + 1 > drag_new_min)
                                        drag_new_min = rdb->parts[kk].high_cyl + 1;
                                    if (rdb->parts[kk].low_cyl > start_cyl &&
                                        rdb->parts[kk].low_cyl - 1 < drag_new_max)
                                        drag_new_max = rdb->parts[kk].low_cyl - 1;
                                }
                                /* Exclude MBR cylinder ranges from drag bounds */
                                if (s_mbr && s_mbr->valid) {
                                    UWORD kmb;
                                    for (kmb = 0; kmb < MBR_MAX_PARTS; kmb++) {
                                        if (!s_mbr->parts[kmb].present) continue;
                                        {
                                            ULONG mlo = MBR_LBAToCyl(s_mbr->parts[kmb].lba_start,
                                                                       rdb->heads, rdb->sectors);
                                            ULONG mhi = MBR_LBAToCyl(s_mbr->parts[kmb].lba_start +
                                                                       s_mbr->parts[kmb].lba_size - 1,
                                                                       rdb->heads, rdb->sectors);
                                            if (mhi < start_cyl && mhi + 1 > drag_new_min)
                                                drag_new_min = mhi + 1;
                                            if (mlo > start_cyl && mlo - 1 < drag_new_max)
                                                drag_new_max = mlo - 1;
                                        }
                                    }
                                }
                                if (drag_new_min <= drag_new_max) {
                                    ULONG ini_hi = start_cyl;
                                    if (ini_hi < drag_new_min) ini_hi = drag_new_min;
                                    if (ini_hi > drag_new_max) ini_hi = drag_new_max;
                                    drag_new       = TRUE;
                                    drag_new_start = drag_new_min;  /* unused but keep tidy */
                                    drag_new_lo    = drag_new_min;
                                    drag_new_hi    = ini_hi;
                                    dbl_part       = -1;
                                    /* Show initial preview immediately */
                                    draw_map(win, rdb, sel, bx, by, bw, bh);
                                    draw_new_part_overlay(win, drag_new_lo, drag_new_hi,
                                                          rdb, bx, by, bw, bh);
                                }
                            }
                        }
                    } else if (code == SELECTUP) {
                        if (drag_part >= 0) {
                            WORD  confirmed_part = drag_part;
                            drag_part = -1;

                            /* Only ask if something actually changed */
                            if (rdb->parts[confirmed_part].low_cyl  != drag_orig_lo ||
                                rdb->parts[confirmed_part].high_cyl != drag_orig_hi)
                            {
                                struct EasyStruct es;
                                char msg[256];
                                ULONG cur_lo = rdb->parts[confirmed_part].low_cyl;
                                ULONG cur_hi = rdb->parts[confirmed_part].high_cyl;
                                /* low_cyl change = partition start moves on disk -> FS
                                   blocks are at wrong offsets -> data destroyed.
                                   high_cyl shrink = end of FS data truncated -> data lost.
                                   high_cyl grow only = safe; offer filesystem resize. */
                                BOOL is_grow = (cur_lo == drag_orig_lo) &&
                                               (cur_hi  > drag_orig_hi);

                                LONG r;
                                if (is_grow) {
                                    DP_SNPRINTF(msg, GS(MSG_PV_RESIZE_GROWN_BODY),
                                        rdb->parts[confirmed_part].drive_name);
                                    /* Button slots: 1=Resize, 2=Cancel, 0=Grow w/o resize.
                                       Extra spaces around the destructive option push it
                                       to the right edge and make accidental clicks harder. */
                                    es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_RESIZE_GROW_GADGETS);
                                } else {
                                    DP_SNPRINTF(msg, GS(MSG_PV_RESIZE_SHRUNK_BODY),
                                        rdb->parts[confirmed_part].drive_name);
                                    es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_RESIZE_SHRUNK_GADGETS);
                                }
                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)GS(MSG_PV_RESIZE_TITLE);
                                es.es_TextFormat   = (UBYTE *)msg;
                                r = EasyRequest(win, &es, NULL);
                                /* is_grow:  1=Resize, 2=Cancel, 0=Grow without resize
                                   shrink:   1=Yes destroy, 0=Cancel */
                                if (is_grow ? (r == 2) : (r == 0)) {
                                    /* Cancel: revert */
                                    rdb->parts[confirmed_part].low_cyl  = drag_orig_lo;
                                    rdb->parts[confirmed_part].high_cyl = drag_orig_hi;
                                } else {
                                    dirty = TRUE;
                                    if (is_grow && r == 1) {
                                        /* Resize: extend partition + grow FS */
                                        int g = offer_ffs_grow(win, bd, rdb,
                                                       &rdb->parts[confirmed_part],
                                                       drag_orig_hi);
                                        if (g == GROW_NONE) g = offer_pfs_grow(win, bd, rdb,
                                                       &rdb->parts[confirmed_part],
                                                       drag_orig_hi);
                                        if (g == GROW_NONE) g = offer_sfs_grow(win, bd, rdb,
                                                       &rdb->parts[confirmed_part],
                                                       drag_orig_hi);
                                        /* FFS remount avoids the reboot; else need it. */
                                        if (g != GROW_REMOUNTED && g != GROW_ABORTED) needs_reboot = TRUE;
                                            if (g != GROW_NONE) refresh_all_gadgets(win, glist);
                                    } else {
                                        /* cyl change committed without an FS grow
                                           (is_grow&&r==0, or shrink&&r==1) */
                                        needs_reboot = TRUE;
                                    }
                                }
                            }
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                        } else if (drag_new) {
                            drag_new = FALSE;
                            /* Open Add Partition dialog with dragged range */
                            {
                                struct PartInfo new_pi;
                                memset(&new_pi, 0, sizeof(new_pi));
                                next_drive_name(rdb, new_pi.drive_name);
                                new_pi.dos_type     = 0x444F5301UL;   /* FFS */
                                new_pi.low_cyl      = drag_new_lo;
                                new_pi.high_cyl     = drag_new_hi;
                                new_pi.heads        = rdb->heads;
                                new_pi.sectors      = rdb->sectors;
                                new_pi.boot_pri      = (rdb->num_parts == 0) ? 0 : -128;
                                /* First partition on the disk: bootable by default. */
                                if (rdb->num_parts == 0) new_pi.flags |= 1UL; /* PBFF_BOOTABLE */
                                new_pi.reserved_blks = 2;
                                new_pi.interleave    = 0;
                                recommend_new_part_defaults(rdb,
                                    new_pi.low_cyl, new_pi.high_cyl, &new_pi);
                                new_pi.num_buffer    = 30;
                                new_pi.buf_mem_type  = 0;
                                new_pi.boot_blocks   = 0;
                                new_pi.baud          = 0;
                                new_pi.control       = 0;
                                new_pi.dev_flags     = 0;
                                if (partition_dialog(&new_pi, GS(MSG_PV_DLG_ADD_PART), rdb, TRUE)) {
                                    rdb->parts[rdb->num_parts++] = new_pi;
                                    dirty = TRUE;
                                    /* A new partition needs a reboot to mount -
                                       unless it will be quick-formatted (mounted
                                       live).  format_pending_partitions() flags a
                                       reboot if that live mount doesn't happen. */
                                    if (!new_pi.want_format) needs_reboot = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                }
                            }
                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                        } else if (drag_move_part >= 0) {
                            WORD confirmed_move = drag_move_part;
                            ULONG dragged_lo    = rdb->parts[confirmed_move].low_cyl;
                            drag_move_part = -1;

                            /* Restore partition to original position first */
                            rdb->parts[confirmed_move].low_cyl  = drag_move_orig_lo;
                            rdb->parts[confirmed_move].high_cyl = drag_move_orig_hi;

                            if (dragged_lo != drag_move_orig_lo && bd) {
                                /* Partition was dragged - open move dialog pre-filled */
                                if (offer_move_partition(win, bd, rdb,
                                                         &rdb->parts[confirmed_move],
                                                         dragged_lo)) {
                                    needs_reboot = TRUE;
                                    sel = confirmed_move;
                                }
                            }
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                        }
                    }
                    break;

                case IDCMP_MOUSEMOVE:
                    if (drag_part >= 0 && rdb && rdb->valid) {
                        WORD  mx2   = bx + 1;
                        UWORD mw2   = bw - 2;
                        ULONG total = rdb->hi_cyl + 1;
                        LONG  dx    = (LONG)(mouse_x - (WORD)mx2);
                        ULONG new_cyl;

                        if (dx < 0)          dx = 0;
                        if (dx >= (LONG)mw2) dx = (LONG)(mw2 - 1);
                        new_cyl = (ULONG)((UQUAD)(ULONG)dx * total / (ULONG)mw2);
                        if (new_cyl < drag_min) new_cyl = drag_min;
                        if (new_cyl > drag_max) new_cyl = drag_max;

                        if (drag_edge == 0)
                            rdb->parts[drag_part].low_cyl  = new_cyl;
                        else
                            rdb->parts[drag_part].high_cyl = new_cyl;

                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        draw_drag_info(win, rdb, drag_part, bx, by, bw, bh);
                    } else if (drag_move_part >= 0 && rdb && rdb->valid) {
                        WORD  mx2   = bx + 1;
                        UWORD mw2   = bw - 2;
                        ULONG total = rdb->hi_cyl + 1;
                        LONG  dpx   = (LONG)(mouse_x - drag_move_anchor_x);
                        LONG  dcyl;
                        ULONG new_lo;

                        /* Convert pixel delta to cylinder delta */
                        if (mw2 > 0)
                            dcyl = (LONG)((UQUAD)(ULONG)(dpx < 0 ? -dpx : dpx)
                                          * total / (ULONG)mw2);
                        else
                            dcyl = 0;
                        if (dpx < 0) dcyl = -dcyl;

                        /* Compute and clamp new lo */
                        if (dcyl < 0 &&
                            (ULONG)(-dcyl) > drag_move_anchor_cyl)
                            new_lo = 0;
                        else
                            new_lo = (ULONG)((LONG)drag_move_anchor_cyl + dcyl);
                        if (new_lo < drag_move_min_lo) new_lo = drag_move_min_lo;
                        if (new_lo > drag_move_max_lo) new_lo = drag_move_max_lo;

                        rdb->parts[drag_move_part].low_cyl  = new_lo;
                        rdb->parts[drag_move_part].high_cyl = new_lo + drag_move_width;

                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        draw_drag_info(win, rdb, drag_move_part, bx, by, bw, bh);
                    } else if (drag_new && rdb && rdb->valid) {
                        WORD  mx2   = bx + 1;
                        UWORD mw2   = bw - 2;
                        ULONG total = rdb->hi_cyl + 1;
                        LONG  dx    = (LONG)(mouse_x - (WORD)mx2);
                        ULONG cyl;

                        if (dx < 0)          dx = 0;
                        if (dx >= (LONG)mw2) dx = (LONG)(mw2 - 1);
                        cyl = (ULONG)((UQUAD)(ULONG)dx * total / (ULONG)mw2);
                        if (cyl < drag_new_min) cyl = drag_new_min;
                        if (cyl > drag_new_max) cyl = drag_new_max;

                        /* lo is always anchored at the left of the free gap */
                        drag_new_lo = drag_new_min;
                        drag_new_hi = cyl;

                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        draw_new_part_overlay(win, drag_new_lo, drag_new_hi,
                                              rdb, bx, by, bw, bh);
                    } else {
                        /* Idle hover - show resize pointer only over a right-edge resize zone,
                         * matching the SELECTDOWN hit-test so the cursor advertises actual intent. */
                        if (ptr_chip) {
                            BOOL want_resize = FALSE;
                            if (rdb && rdb->valid &&
                                mouse_x >= bx && mouse_x < bx + (WORD)bw &&
                                mouse_y >= by && mouse_y < by + (WORD)bh)
                            {
                                WORD  mx2   = bx + 1;
                                UWORD mw2   = bw - 2;
                                ULONG total = rdb->hi_cyl + 1;
                                WORD  blk_in = hit_test_partition(rdb, mx2, mw2, total, mouse_x);
                                if (blk_in >= 0) {
                                    WORD lx_p = (WORD)(mx2 + (WORD)((UQUAD)rdb->parts[blk_in].low_cyl       * mw2 / total));
                                    WORD rx_p = (WORD)(mx2 + (WORD)((UQUAD)(rdb->parts[blk_in].high_cyl + 1) * mw2 / total));
                                    WORD d_right = (WORD)((rx_p - 1) - mouse_x);
                                    WORD ezone   = (WORD)((rx_p - lx_p) / 8);
                                    if (d_right < 0) d_right = 0;
                                    if (ezone < DRAG_TOL) ezone = DRAG_TOL;
                                    if (d_right <= ezone) want_resize = TRUE;
                                } else {
                                    WORD ee = 0;
                                    WORD ep = hit_test_edge(rdb, mx2, mw2, total, mouse_x, &ee);
                                    if (ep >= 0 && ee == 1) want_resize = TRUE;
                                }
                            }
                            if (want_resize && !ptr_custom) {
                                /* Negative offsets place the sprite up-and-left of
                                 * the mouse so the hot spot (col 7, row 3) lands on
                                 * the actual cursor position. */
                                SetPointer(win, ptr_chip, 7, 16, -7, -3);
                                ptr_custom = TRUE;
                            } else if (!want_resize && ptr_custom) {
                                ClearPointer(win);
                                ptr_custom = FALSE;
                            }
                        }
                    }
                    break;

                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == GID_PARTLIST) {
                        sel = (WORD)code;
                        draw_map(win, rdb, sel, bx, by, bw, bh);
                    }
                    break;

                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GID_PARTLIST:
                        sel = (WORD)code;
                        /* Persist the highlight past GADGETUP - see the
                           comment on lvdm_State in lv_render(). A plain
                           GT_SetGadgetAttrsA(GTLV_Selected,...) + RefreshGList
                           does NOT bring the row back once the mouse button
                           is released (confirmed on real KS3.1/3.2
                           hardware). Nothing about the partition data
                           changed here (just which row is highlighted), so
                           use the lightweight redraw instead of re-deriving
                           every row's display string via refresh_listview(). */
                        mark_listview_selection(win, lv_gad, sel);
                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        /* double-click -> open Edit dialog. See
                           quick_double_click() above for why this doesn't
                           use IEQUALIFIER_DOUBLECLICK. */
                        if (sel >= 0 && sel == dbl_list_sel &&
                            quick_double_click(dbl_list_sec, dbl_list_mic,
                                               ev_sec, ev_mic)) {
                            dbl_list_sel = -1;
                            if (sel < (WORD)rdb->num_parts && !part_is_mbr[sel]) {
                                ULONG old_hi = rdb->parts[sel].high_cyl;
                                if (partition_dialog(&rdb->parts[sel],
                                                     GS(MSG_PV_DLG_EDIT_PART), rdb, FALSE)) {
                                    {
                                    int g = offer_ffs_grow(win, bd, rdb,
                                                   &rdb->parts[sel], old_hi);
                                    if (g == GROW_NONE) g = offer_pfs_grow(win, bd, rdb,
                                                   &rdb->parts[sel], old_hi);
                                    if (g == GROW_NONE) g = offer_sfs_grow(win, bd, rdb,
                                                   &rdb->parts[sel], old_hi);
                                    dirty        = TRUE;
                                    if (g != GROW_REMOUNTED && g != GROW_ABORTED) needs_reboot = TRUE;
                                    if (g != GROW_NONE) refresh_all_gadgets(win, glist);
                                    }
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                }
                            } else if (sel < MAX_LIST_ENTRIES && part_is_mbr[sel] &&
                                       s_mbr && s_mbr->valid) {
                                UBYTE mslot = part_mbr_slot[sel];
                                offer_edit_mbr_part(win, bd, s_mbr, rdb, mslot);
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                            }
                        } else {
                            dbl_list_sel = sel;
                            dbl_list_sec = ev_sec;
                            dbl_list_mic = ev_mic;
                        }
                        break;

                    case GID_LASTDISK:
                        if (rdb) rdb->flags ^= RDBFF_LAST;
                        dirty = TRUE;
                        break;

                    case GID_LASTLUN:
                        if (rdb) rdb->flags ^= RDBFF_LASTLUN;
                        dirty = TRUE;
                        break;

                    case GID_INITRDB: {
                        struct EasyStruct es;
                        ULONG real_cyls = 0, real_heads = 0, real_secs = 0;
                        char  driver_warn[200];

                        driver_warn[0] = '\0';

                        if (bd) {
                            BlockDev_GetGeometry(bd, &real_cyls, &real_heads, &real_secs);

                            /* Warn when READ CAPACITY reports significantly more
                               capacity than TD_GETGEOMETRY - old driver limiting. */
                            if (bd->rc_total_blocks > 0 && bd->td_total_bytes > 0) {
                                UQUAD rc_bytes = (UQUAD)bd->rc_total_blocks *
                                                 bd->rc_block_size;
                                if (rc_bytes > bd->td_total_bytes +
                                               bd->td_total_bytes / 20) {
                                    char td_sz[16], rc_sz[16];
                                    FormatSize(bd->td_total_bytes, td_sz);
                                    FormatSize(rc_bytes, rc_sz);
                                    DP_SNPRINTF(driver_warn, GS(MSG_PV_INIT_DRIVER_WARN),
                                        rc_sz, td_sz);
                                }
                            }
                        }
                        /* Fall back to whatever we already know */
                        if (real_cyls == 0) real_cyls  = rdb->cylinders;
                        if (real_cyls == 0) {
                            /* Auto-detection failed - offer manual entry */
                            if (!geometry_dialog(0, 0, 0,
                                                 &real_cyls, &real_heads, &real_secs))
                                break;
                            /* geometry_dialog validated cyls/heads/secs > 0 */
                        }
                        if (real_heads == 0) real_heads = rdb->heads;
                        if (real_secs  == 0) real_secs  = rdb->sectors;

                        if (rdb->valid) {
                            /* Disk already has an RDB.
                               Loop so "Manual..." can update geometry and re-show dialog. */
                            BOOL geom_retry = TRUE;
                            while (geom_retry) {
                                LONG choice;
                                char msg[512];
                                geom_retry = FALSE;

                                DP_SNPRINTF(msg, GS(MSG_PV_INIT_HAS_RDB_BODY),
                                    (unsigned)rdb->num_parts,
                                    rdb->num_parts == 1 ? "" : "s",
                                    (unsigned long)real_cyls,
                                    (unsigned long)real_heads,
                                    (unsigned long)real_secs,
                                    (unsigned long)rdb->cylinders,
                                    (unsigned long)rdb->heads,
                                    (unsigned long)rdb->sectors,
                                    driver_warn);

                                { BOOL add_mbr = FALSE;
                                  choice = init_rdb_dialog(win, msg, TRUE, &add_mbr);
                                if (choice == 1) {
                                    /* Re-init */
                                    RDB_InitFresh(rdb, real_cyls, real_heads, real_secs);
                                    { struct TagItem st[]={{GTCB_Checked,0},{TAG_DONE,0}};
                                      if (lastdisk_gad) { st[0].ti_Data=(rdb->flags&RDBFF_LAST)?1:0;    GT_SetGadgetAttrsA(lastdisk_gad,win,NULL,st); }
                                      if (lastlun_gad)  { st[0].ti_Data=(rdb->flags&RDBFF_LASTLUN)?1:0; GT_SetGadgetAttrsA(lastlun_gad, win,NULL,st); } }
                                    if (add_mbr && bd) {
                                        rdb->rdb_block_lo = 1;
                                        rdb->block_num    = 1;
                                        memset(&mbr_store, 0, sizeof(mbr_store));
                                        if (MBR_WriteEmpty(bd)) {
                                            mbr_store.valid = TRUE;
                                            s_mbr = &mbr_store;
                                        }
                                    }
                                    sel   = -1;
                                    dirty = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else if (choice == 2) {
                                    /* Update Geometry (EXPERIMENTAL) - translate each
                                       partition's cyl boundaries to the new bytes-per-cyl
                                       so partitions stay on the same physical bytes and
                                       the in-memory view is self-consistent (matches what
                                       a reload-after-write would show). */
                                    ULONG bsz     = (rdb->blk_size > 0) ? rdb->blk_size : 512;
                                    UQUAD old_bpc = (UQUAD)rdb->heads * rdb->sectors * bsz;
                                    UQUAD new_bpc = (UQUAD)real_heads * real_secs    * bsz;
                                    if (old_bpc > 0 && new_bpc > 0) {
                                        UWORD k;
                                        for (k = 0; k < rdb->num_parts; k++) {
                                            struct PartInfo *p = &rdb->parts[k];
                                            UQUAD start = (UQUAD)p->low_cyl        * old_bpc;
                                            UQUAD end   = (UQUAD)(p->high_cyl + 1) * old_bpc;
                                            ULONG nlo   = (ULONG)(start / new_bpc);
                                            ULONG nhi_excl = (ULONG)(end / new_bpc);
                                            if (nhi_excl == 0) nhi_excl = 1;
                                            p->low_cyl  = nlo;
                                            p->high_cyl = nhi_excl - 1;
                                            if (p->high_cyl > real_cyls - 1)
                                                p->high_cyl = real_cyls - 1;
                                            if (p->low_cyl  > p->high_cyl)
                                                p->low_cyl  = p->high_cyl;
                                            p->heads   = real_heads;
                                            p->sectors = real_secs;
                                        }
                                    }
                                    rdb->cylinders = real_cyls;
                                    rdb->heads     = real_heads;
                                    rdb->sectors   = real_secs;
                                    rdb->hi_cyl    = real_cyls - 1;
                                    dirty = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else if (choice == 3) {
                                    /* Manual - re-enter geometry, then re-show dialog */
                                    if (geometry_dialog(real_cyls, real_heads, real_secs,
                                                        &real_cyls, &real_heads, &real_secs))
                                        geom_retry = TRUE;
                                }
                                } /* add_mbr scope */
                                /* choice == 0: Cancel - exit loop */
                            }
                        } else {
                            /* No RDB - confirm and create fresh */
                            BOOL geom_retry = TRUE;
                            while (geom_retry) {
                                LONG choice;
                                char msg_nordb[512];
                                geom_retry = FALSE;

                                DP_SNPRINTF(msg_nordb, GS(MSG_PV_INIT_NO_RDB_BODY),
                                    driver_warn);

                                { BOOL add_mbr2 = FALSE;
                                  choice = init_rdb_dialog(win, msg_nordb, FALSE, &add_mbr2);
                                if (choice == 1) {
                                    /* Yes */
                                    RDB_InitFresh(rdb, real_cyls, real_heads, real_secs);
                                    { struct TagItem st[]={{GTCB_Checked,0},{TAG_DONE,0}};
                                      if (lastdisk_gad) { st[0].ti_Data=(rdb->flags&RDBFF_LAST)?1:0;    GT_SetGadgetAttrsA(lastdisk_gad,win,NULL,st); }
                                      if (lastlun_gad)  { st[0].ti_Data=(rdb->flags&RDBFF_LASTLUN)?1:0; GT_SetGadgetAttrsA(lastlun_gad, win,NULL,st); } }
                                    if (add_mbr2 && bd) {
                                        rdb->rdb_block_lo = 1;
                                        rdb->block_num    = 1;
                                        memset(&mbr_store, 0, sizeof(mbr_store));
                                        if (MBR_WriteEmpty(bd)) {
                                            mbr_store.valid = TRUE;
                                            s_mbr = &mbr_store;
                                        }
                                    }
                                    sel   = -1;
                                    dirty = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else if (choice == 2) {
                                    /* Manual - re-enter geometry, then re-show dialog */
                                    if (geometry_dialog(real_cyls, real_heads, real_secs,
                                                        &real_cyls, &real_heads, &real_secs))
                                        geom_retry = TRUE;
                                }
                                } /* add_mbr2 scope */
                                /* choice == 0 (No): exit loop */
                            }
                        }
                        break;
                    }

                    case GID_ADD: {
                        struct PartInfo new_pi;
                        ULONG lo, hi;
                        if (!rdb->valid) {
                            if (rdb->cylinders == 0) break;
                            RDB_InitFresh(rdb, rdb->cylinders,
                                          rdb->heads, rdb->sectors);
                            { struct TagItem st[]={{GTCB_Checked,0},{TAG_DONE,0}};
                              if (lastdisk_gad) { st[0].ti_Data=(rdb->flags&RDBFF_LAST)?1:0;    GT_SetGadgetAttrsA(lastdisk_gad,win,NULL,st); }
                              if (lastlun_gad)  { st[0].ti_Data=(rdb->flags&RDBFF_LASTLUN)?1:0; GT_SetGadgetAttrsA(lastlun_gad, win,NULL,st); } }
                        }
                        if (rdb->num_parts >= MAX_PARTITIONS) break;
                        find_free_range(rdb, &lo, &hi);
                        if (lo > hi) break;

                        memset(&new_pi, 0, sizeof(new_pi));
                        next_drive_name(rdb, new_pi.drive_name);
                        new_pi.dos_type     = 0x444F5301UL;   /* FFS */
                        new_pi.low_cyl      = lo;
                        new_pi.high_cyl     = hi;
                        new_pi.heads        = rdb->heads;
                        new_pi.sectors      = rdb->sectors;
                        new_pi.boot_pri      = (rdb->num_parts == 0) ? 0 : -128;
                        /* First partition on the disk: bootable by default. */
                        if (rdb->num_parts == 0) new_pi.flags |= 1UL; /* PBFF_BOOTABLE */
                        new_pi.reserved_blks = 2;
                        new_pi.interleave    = 0;
                        recommend_new_part_defaults(rdb,
                            new_pi.low_cyl, new_pi.high_cyl, &new_pi);
                        new_pi.num_buffer    = 30;
                        new_pi.buf_mem_type  = 0;
                        new_pi.boot_blocks   = 0;
                        new_pi.baud          = 0;
                        new_pi.control       = 0;
                        new_pi.dev_flags     = 0;
                        /* flags: 0 = bootable */

                        if (partition_dialog(&new_pi, GS(MSG_PV_DLG_ADD_PART), rdb, TRUE)) {
                            rdb->parts[rdb->num_parts++] = new_pi;
                            dirty = TRUE;
                            /* New partition needs a reboot to mount unless it
                               will be quick-formatted (mounted live). */
                            if (!new_pi.want_format) needs_reboot = TRUE;
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                        }
                        break;
                    }

                    case GID_EDIT:
                        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                            ULONG old_hi = rdb->parts[sel].high_cyl;
                            if (partition_dialog(&rdb->parts[sel],
                                                 GS(MSG_PV_DLG_EDIT_PART), rdb, FALSE)) {
                                {
                                int g = offer_ffs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                if (g == GROW_NONE) g = offer_pfs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                if (g == GROW_NONE) g = offer_sfs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                dirty        = TRUE;
                                /* Clean FFS grow remounts live; else reboot. */
                                if (g != GROW_REMOUNTED && g != GROW_ABORTED) needs_reboot = TRUE;
                                            if (g != GROW_NONE) refresh_all_gadgets(win, glist);
                                }
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                            }
                        } else if (sel >= (WORD)rdb->num_parts && s_mbr && s_mbr->valid) {
                            /* Edit MBR partition */
                            UBYTE mslot = part_mbr_slot[sel];
                            offer_edit_mbr_part(win, bd, s_mbr, rdb, mslot);
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                        }
                        break;

                    case GID_DELETE:
                        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                            struct EasyStruct es;
                            char msg[128];
                            DP_SNPRINTF(msg, GS(MSG_PV_DELETE_BODY),
                                rdb->parts[sel].drive_name);
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)GS(MSG_PV_DELETE_TITLE);
                            es.es_TextFormat   = (UBYTE *)msg;
                            es.es_GadgetFormat = (UBYTE *)GS(MSG_YES_NO);
                            if (EasyRequest(win, &es, NULL) == 1) {
                                UWORD j;
                                /* Remember the name so we can unmount it after
                                   the RDB is written - no reboot needed unless
                                   the unmount fails (volume in use). */
                                if (s_unmount_count < MAX_PARTITIONS) {
                                    strncpy(s_unmount_names[s_unmount_count],
                                            rdb->parts[sel].drive_name, 31);
                                    s_unmount_names[s_unmount_count][31] = '\0';
                                    s_unmount_count++;
                                }
                                for (j=(UWORD)sel; j+1 < rdb->num_parts; j++)
                                    rdb->parts[j] = rdb->parts[j+1];
                                rdb->num_parts--;
                                dirty = TRUE;
                                if (sel >= (WORD)rdb->num_parts)
                                    sel = (WORD)rdb->num_parts - 1;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                            }
                        } else if (sel >= (WORD)rdb->num_parts && s_mbr && s_mbr->valid && bd) {
                            /* Delete MBR partition */
                            UBYTE mslot = part_mbr_slot[sel];
                            if (s_mbr->parts[mslot].present) {
                                struct EasyStruct es;
                                char msg[64];
                                sprintf(msg, GS(MSG_MBR_DEL_BODY_FMT),
                                        s_mbr->parts[mslot].name);
                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)GS(MSG_MBR_DEL_TITLE);
                                es.es_TextFormat   = (UBYTE *)msg;
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_YES_NO);
                                if (EasyRequest(win, &es, NULL) == 1) {
                                    s_mbr->parts[mslot].present = FALSE;
                                    if (MBR_Write(bd, s_mbr)) {
                                        sel = (WORD)rdb->num_parts - 1;
                                    } else {
                                        /* Roll back on write failure */
                                        s_mbr->parts[mslot].present = TRUE;
                                    }
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                }
                            }
                        }
                        break;

                    case GID_MOVE:
                        if (sel >= 0 && sel < (WORD)rdb->num_parts && bd) {
                            if (offer_move_partition(win, bd, rdb, &rdb->parts[sel], 0)) {
                                needs_reboot = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                            }
                        }
                        break;

                    case GID_FILESYS:
                        if (!rdb->valid) {
                            struct EasyStruct es;
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
                            es.es_TextFormat   = (UBYTE *)GS(MSG_PV_NO_RDB_INIT_FIRST);
                            es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                            EasyRequest(win, &es, NULL);
                        } else {
                            if (filesystem_manager_dialog(rdb))
                                dirty = TRUE;
                        }
                        break;

                    case GID_WRITE: {
                        struct EasyStruct es;
                        BOOL write_ok;
                        es.es_StructSize   = sizeof(es);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
                        es.es_TextFormat   = (UBYTE *)GS(MSG_PV_WRITE_CONFIRM_BODY);
                        es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_WRITE_CANCEL);
                        if (EasyRequest(win, &es, NULL) != 1) break;

                        write_ok = (bd != NULL) && RDB_Write(bd, rdb);

                        if (!write_ok && bd && bd->last_io_err == 0) {
                            /* Metadata overflow - try to offer a lo_cyl increase */
                            ULONG blks_per_cyl = rdb->heads * rdb->sectors;
                            ULONG new_lo       = (blks_per_cyl > 0)
                                ? (bd->last_overflow_need + blks_per_cyl - 1) / blks_per_cyl
                                : 0;
                            WORD  blk_part     = -1;
                            UWORD j;

                            for (j = 0; j < rdb->num_parts; j++) {
                                if (new_lo > 0 && rdb->parts[j].low_cyl < new_lo) {
                                    blk_part = (WORD)j; break;
                                }
                            }

                            if (new_lo > rdb->lo_cyl && blk_part < 0) {
                                /* Safe: no partition starts inside the new reserved area */
                                DP_SNPRINTF(wfmt, GS(MSG_PV_OVERFLOW_INCREASE_BODY),
                                    (unsigned long)bd->last_overflow_need,
                                    (unsigned long)bd->last_overflow_avail,
                                    (unsigned long)rdb->lo_cyl,
                                    (unsigned long)blks_per_cyl,
                                    (unsigned long)new_lo);
                                es.es_TextFormat   = (UBYTE *)wfmt;
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_OVERFLOW_INCREASE_GADGETS);
                                if (EasyRequest(win, &es, NULL) == 1) {
                                    rdb->lo_cyl = new_lo;
                                    dirty       = TRUE;
                                    write_ok    = RDB_Write(bd, rdb);
                                    if (!write_ok) {
                                        DP_SNPRINTF(wfmt, GS(MSG_PV_WRITE_FAILED_LOCYL),
                                            (int)bd->last_io_err);
                                        es.es_TextFormat   = (UBYTE *)wfmt;
                                        es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                        EasyRequest(win, &es, NULL);
                                    }
                                }
                            } else if (blk_part >= 0) {
                                /* A partition blocks the expansion */
                                DP_SNPRINTF(wfmt, GS(MSG_PV_OVERFLOW_BLOCKED),
                                    (unsigned long)new_lo,
                                    rdb->parts[blk_part].drive_name,
                                    (unsigned long)rdb->parts[blk_part].low_cyl,
                                    rdb->parts[blk_part].drive_name,
                                    (unsigned long)new_lo);
                                es.es_TextFormat   = (UBYTE *)wfmt;
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                EasyRequest(win, &es, NULL);
                            } else {
                                DP_SNPRINTF(wfmt, GS(MSG_PV_OVERFLOW_PLAIN),
                                    (unsigned long)bd->last_overflow_need,
                                    (unsigned long)bd->last_overflow_avail);
                                es.es_TextFormat   = (UBYTE *)wfmt;
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                EasyRequest(win, &es, NULL);
                            }
                        } else if (!write_ok) {
                            if (bd && bd->last_io_err == 1)
                                DP_SNPRINTF(wfmt, GS(MSG_PV_VERIFY_FAIL_FMT),
                                    (unsigned long)bd->last_verify_block,
                                    (unsigned long)bd->last_verify_off,
                                    bd->last_wrote[0], bd->last_wrote[1],
                                    bd->last_wrote[2], bd->last_wrote[3],
                                    bd->last_read[0],  bd->last_read[1],
                                    bd->last_read[2],  bd->last_read[3]);
                            else
                                DP_SNPRINTF(wfmt, GS(MSG_PV_WRITE_FAILED_FMT),
                                    bd ? (int)bd->last_io_err : 0);
                            es.es_TextFormat   = (UBYTE *)wfmt;
                            es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                            EasyRequest(win, &es, NULL);
                        }

                        if (write_ok) {
                            dirty = FALSE;
                            if (format_pending_partitions(win, bd, rdb))
                                needs_reboot = TRUE;
                            if (unmount_deleted_partitions(win, rdb))
                                needs_reboot = TRUE;
                            /* Only offer to erase an MBR that we didn't put there
                               ourselves (s_mbr->valid means we wrote/read it). */
                            if (!(s_mbr && s_mbr->valid) && BlockDev_HasMBR(bd)) {
                                es.es_TextFormat   = (UBYTE *)GS(MSG_PV_MBR_FOUND_BODY);
                                es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_MBR_ERASE_KEEP);
                                if (EasyRequest(win, &es, NULL) == 1)
                                    BlockDev_EraseMBR(bd);
                            }
                            if (needs_reboot) {
                                offer_reboot(win, GS(MSG_PV_REBOOT_WRITTEN));
                                needs_reboot = FALSE;
                            }
                        }
                        break;
                    }

                    case GID_BACK:
                        if (dirty) {
                            struct EasyStruct es;
                            LONG r;
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
                            es.es_TextFormat   = (UBYTE *)GS(MSG_PV_UNSAVED_BODY);
                            es.es_GadgetFormat = (UBYTE *)GS(MSG_PV_WRITE_DISCARD_CANCEL);
                            r = EasyRequest(win, &es, NULL);
                            if (r == 0) break;           /* Cancel - stay */
                            if (r == 1 && bd) {          /* Write */
                                if (RDB_Write(bd, rdb)) {
                                    dirty = FALSE;
                                    if (format_pending_partitions(win, bd, rdb))
                                        needs_reboot = TRUE;
                                    if (unmount_deleted_partitions(win, rdb))
                                        needs_reboot = TRUE;
                                    if (needs_reboot) {
                                        offer_reboot(win, GS(MSG_PV_REBOOT_WRITTEN));
                                        needs_reboot = FALSE;
                                    }
                                } else {
                                    DP_SNPRINTF(wfmt, GS(MSG_PV_WRITE_FAILED_FMT),
                                            (int)bd->last_io_err);
                                    es.es_TextFormat   = (UBYTE *)wfmt;
                                    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
                                    EasyRequest(win, &es, NULL);
                                    break; /* stay open */
                                }
                            }
                            /* r == 2: Discard - fall through to exit */
                        } else if (needs_reboot) {
                            /* RDB was already written outside the dirty path
                               (Partition Move writes RDB itself). */
                            offer_reboot(win, GS(MSG_PV_REBOOT_LAYOUT));
                            needs_reboot = FALSE;
                        }
                        running = FALSE; break;
                    }
                    break;

                case IDCMP_NEWSIZE: {
                    struct Gadget    *new_glist = NULL, *new_lv = NULL;
                    struct Gadget    *new_ldisk = NULL, *new_llun = NULL;
                    struct PartLayout new_lay;
                    UWORD fh = (UWORD)win->WScreen->Font->ta_YSize;

                    /* Cancel any in-progress drag - restore partition to pre-drag state */
                    if (drag_part >= 0) {
                        rdb->parts[drag_part].low_cyl  = drag_orig_lo;
                        rdb->parts[drag_part].high_cyl = drag_orig_hi;
                        drag_part = -1;
                    }

                    /* Reset pointer - map position changes after resize */
                    if (ptr_custom) { ClearPointer(win); ptr_custom = FALSE; }

                    RemoveGList(win, glist, -1);
                    FreeGadgets(glist);
                    glist = NULL; lv_gad = NULL;
                    lastdisk_gad = NULL; lastlun_gad = NULL;

                    /* Erase the window interior so stale gadget imagery
                       (buttons that moved, old Cyl labels, etc.) is gone
                       before the new layout is drawn. */
                    EraseRect(win->RPort,
                              win->BorderLeft,  win->BorderTop,
                              (WORD)win->Width  - (WORD)win->BorderRight  - 1,
                              (WORD)win->Height - (WORD)win->BorderBottom - 1);

                    if (build_gadgets(vi,
                                      (UWORD)win->Width,  (UWORD)win->Height,
                                      (UWORD)win->BorderLeft,  (UWORD)win->BorderTop,
                                      (UWORD)win->BorderRight, (UWORD)win->BorderBottom,
                                      fh, win->WScreen->Font, rdb->flags,
                                      &new_glist, &new_lv, &new_ldisk, &new_llun, &new_lay)) {
                        glist  = new_glist;
                        lv_gad = new_lv;
                        lastdisk_gad = new_ldisk;
                        lastlun_gad  = new_llun;
                        ix = new_lay.ix; iy = new_lay.iy; iw = new_lay.iw;
                        bx = new_lay.bx; by = new_lay.by;
                        bw = new_lay.bw; bh = new_lay.bh;
                        hx = new_lay.hx; hy = new_lay.hy; hw = new_lay.hw;

                        AddGList(win, glist, (UWORD)-1, -1, NULL);
                        RefreshGList(glist, win, NULL, -1);
                        GT_RefreshWindow(win, NULL);

                        /* Scroll the selected row back into view. Note:
                           GTLV_Selected is NOT set here - lv_render()'s
                           highlight is driven entirely by g_part_sel (see
                           its comment), which already equals sel and
                           doesn't need restoring; the gadget's own native
                           GTLV_Selected attribute is never read by the
                           render hook, so setting it here would be dead. */
                        if (sel >= 0) {
                            struct TagItem mv[] = {
                                { GTLV_MakeVisible, (ULONG)sel },
                                { TAG_DONE, 0 }
                            };
                            GT_SetGadgetAttrsA(lv_gad, win, NULL, mv);
                        }

                        draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                    ix, iy, iw, bx, by, bw, bh,
                                    hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                    }
                    break;
                }

                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                ix, iy, iw, bx, by, bw, bh,
                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                    /* Redraw every gadget (incl. the bottom button row) - GadTools
                       only auto-refreshes the damaged region, which can leave
                       buttons blank after sub-windows (grow progress / requesters)
                       overlapped them. */
                    if (glist) RefreshGList(glist, win, NULL, -1);
                    break;
                }
            }
            /* Flag/clear pending writes in the titlebar when the state changes. */
            if (dirty != last_dirty) {
                set_title_dirty(win, devname, unit, dirty);
                last_dirty = dirty;
            }
        }
    }

cleanup:
    RDB_SetWriteProgressHook(NULL, NULL);
    if (win) {
        if (ptr_custom) ClearPointer(win);
        ClearMenuStrip(win);
        if (glist) RemoveGList(win, glist, -1);
        CloseWindow(win);
    }
    if (ptr_chip)  FreeVec(ptr_chip);
    if (menu)      FreeMenus(menu);
    if (glist)     FreeGadgets(glist);
    if (vi)        FreeVisualInfo(vi);
    if (scr)       UnlockPubScreen(NULL, scr);
    {
        struct Screen *ws = LockPubScreen(NULL);
        if (ws) { free_pens(ws); UnlockPubScreen(NULL, ws); }
    }
    if (rdb)  { RDB_FreeCode(rdb); FreeVec(rdb); }
    if (bd)   BlockDev_Close(bd);
    return exit_req;
}
