/*
 * partview.c — Partition view window for DiskPart.
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
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "partview.h"
#include "version.h"
#include "ffsresize.h"
#include "pfsresize.h"
#include "sfsresize.h"
#include "partmove.h"
#include "partview_internal.h"


/* ------------------------------------------------------------------ */
/* External library bases (defined in main.c)                          */
/* ------------------------------------------------------------------ */

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct Library       *AslBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *GadToolsBase;

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
/* Partition colours — match AmigaPart COLORS list                     */
/* ------------------------------------------------------------------ */

#define NUM_PART_COLORS 8
static const UBYTE PART_R[NUM_PART_COLORS]={0x4A,0xE6,0x27,0x8E,0xE7,0x16,0xF3,0x29};
static const UBYTE PART_G[NUM_PART_COLORS]={0x90,0x7E,0xAE,0x44,0x4C,0xA0,0x9C,0x80};
static const UBYTE PART_B[NUM_PART_COLORS]={0xD9,0x22,0x60,0xAD,0x3C,0x85,0x12,0xB9};
#define C32(b) (((ULONG)(b)<<24)|((ULONG)(b)<<16)|((ULONG)(b)<<8)|(ULONG)(b))

/* ------------------------------------------------------------------ */
/* Partition listview — proportional-font column renderer              */
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

/* Column pixel layout — computed in build_gadgets from the actual font */
static struct {
    UWORD x;    /* left edge of column */
    UWORD w;    /* column width (for right-align) */
} lv_cols[LVCOL_COUNT];

/* Header labels — match order of LVCOL_* */
static const char * const lv_hdr[LVCOL_COUNT] = {
    "", "Drive", "Lo Cyl", "Hi Cyl", "FileSystem", "Size", "Boot"
};

/* Pointer to current RDB (set whenever rdb is live) — used by render hook */
static const struct RDBInfo *lv_rdb;

/* Forward declarations needed by lv_render (defined later in file) */
static char        part_strs[MAX_PARTITIONS][80];
static struct Node part_nodes[MAX_PARTITIONS];


/* Render hook — AmigaOS calls h_Entry with a0=hook, a1=msg, a2=node.
   Register variables capture those values before GCC can use the regs.
   a0/a1 are caller-saved so GCC never touches them in the prologue;
   a2 is callee-saved so GCC may push it — but PUSH doesn't change the
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
    if (!lv_rdb || idx < 0 || idx >= (WORD)lv_rdb->num_parts)
        return LVCB_OK;

    pi  = &lv_rdb->parts[idx];
    rp  = msg->lvdm_RastPort;
    b   = &msg->lvdm_Bounds;
    sel = (msg->lvdm_State == LVR_SELECTED ||
           msg->lvdm_State == LVR_SELECTEDDISABLED);

    bg_pen = sel ? (UWORD)msg->lvdm_DrawInfo->dri_Pens[FILLPEN]
                 : (UWORD)msg->lvdm_DrawInfo->dri_Pens[BACKGROUNDPEN];
    fg_pen = sel ? (UWORD)msg->lvdm_DrawInfo->dri_Pens[FILLTEXTPEN]
                 : (UWORD)msg->lvdm_DrawInfo->dri_Pens[TEXTPEN];

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

    /* Selection marker */
    if (sel) { tmp[0] = '>'; LV_TEXT(LVCOL_MARK, tmp, 1); }

    /* Drive name */
    {
        const char *nm = pi->drive_name[0] ? pi->drive_name : "(none)";
        LV_TEXT(LVCOL_DRIVE, nm, strlen(nm));
    }

    /* Lo Cyl */
    sprintf(tmp, "%lu", (unsigned long)pi->low_cyl);
    LV_RIGHT(LVCOL_LOCYL, tmp, strlen(tmp));

    /* Hi Cyl */
    sprintf(tmp, "%lu", (unsigned long)pi->high_cyl);
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

    /* Boot priority */
    sprintf(tmp, "%ld", (long)pi->boot_pri);
    LV_TEXT(LVCOL_BOOT, tmp, strlen(tmp));

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

static void build_part_list(struct RDBInfo *rdb, WORD sel)
{
    UWORD i;
    lv_rdb = rdb;   /* render hook needs access to partition data */
    list_init(&part_list);
    if (!rdb || !rdb->valid || rdb->num_parts == 0) return;

    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi = &rdb->parts[i];
        char dt[16], sz[16];
        ULONG cyls  = (pi->high_cyl >= pi->low_cyl)
                      ? pi->high_cyl - pi->low_cyl + 1 : 0;
        ULONG heads = pi->heads   > 0 ? pi->heads   : rdb->heads;
        ULONG secs  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
        ULONG bsz   = pi->block_size > 0 ? pi->block_size : 512;
        UQUAD bytes = (UQUAD)cyls * heads * secs * bsz;
        const char *nm = pi->drive_name[0] ? pi->drive_name : "(none)";

        FriendlyDosType(pi->dos_type, dt);
        FormatSize(bytes, sz);

        /* ">" marker for selected row, space otherwise */
        sprintf(part_strs[i], "%c %-7s %9lu %9lu  %-12s  %9s   %4ld",
                ((WORD)i == sel) ? '>' : ' ',
                nm,
                (unsigned long)pi->low_cyl,
                (unsigned long)pi->high_cyl,
                dt, sz,
                (long)pi->boot_pri);

        part_nodes[i].ln_Name = part_strs[i];
        part_nodes[i].ln_Type = NT_USER;
        part_nodes[i].ln_Pri  = 0;
        AddTail(&part_list, &part_nodes[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Pen allocation                                                       */
/* ------------------------------------------------------------------ */

static LONG part_pens[NUM_PART_COLORS];
static LONG bg_pen;      /* dark navy background for the map  */
static LONG rdb_pen;     /* muted gray for RDB reserved area  */

static void alloc_pens(struct Screen *scr)
{
    struct ColorMap *cm = scr->ViewPort.ColorMap;
    struct TagItem   nt[] = { { TAG_DONE, 0 } };
    UWORD i;
    for (i = 0; i < NUM_PART_COLORS; i++)
        part_pens[i] = ObtainBestPenA(cm,
            C32(PART_R[i]), C32(PART_G[i]), C32(PART_B[i]), nt);
    bg_pen  = ObtainBestPenA(cm, C32(0x2a), C32(0x2a), C32(0x3a), nt);
    rdb_pen = ObtainBestPenA(cm, C32(0x55), C32(0x55), C32(0x66), nt);
}

static void free_pens(struct Screen *scr)
{
    struct ColorMap *cm = scr->ViewPort.ColorMap;
    UWORD i;
    for (i = 0; i < NUM_PART_COLORS; i++)
        if (part_pens[i] >= 0) { ReleasePen(cm,(ULONG)part_pens[i]); part_pens[i]=-1; }
    if (bg_pen  >= 0) { ReleasePen(cm,(ULONG)bg_pen);  bg_pen  = -1; }
    if (rdb_pen >= 0) { ReleasePen(cm,(ULONG)rdb_pen); rdb_pen = -1; }
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

    /* Map inner area — leave 1px border all round */
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

    /* Background — free space */
    SetAPen(rp, fill);
    SetDrMd(rp, JAM2);
    RectFill(rp, mx, my, mx+(WORD)mw-1, my+(WORD)mh-1);

    if (!rdb || !rdb->valid) {
        const char *msg = "No RDB — use Init RDB to create partitions";
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
            WORD  px1 = MAP_X(pi->low_cyl);
            WORD  px2 = MAP_X(pi->high_cyl + 1);
            LONG  pen;
            WORD  pw;

            if (px2 < px1 + 2) px2 = px1 + 2;
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
                char  *nm    = pi->drive_name[0] ? pi->drive_name : "(none)";
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

        /* Selection highlight: 3-px bright frame + dark shadow frame */
        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
            struct PartInfo *sp  = &rdb->parts[sel];
            WORD sx1 = MAP_X(sp->low_cyl);
            WORD sx2 = MAP_X(sp->high_cyl + 1);
            WORD bsz = 3;   /* frame thickness in pixels */
            if (sx2 < sx1 + 2) sx2 = sx1 + 2;

            /* Dark shadow frame 1px outside the bright frame for contrast */
            SetAPen(rp, 2);
            SetDrMd(rp, JAM2);
            if (sx1 > mx) {
                RectFill(rp, sx1-1, my,           sx1-1, my+(WORD)mh-1);
            }
            if (sx2 < mx+(WORD)mw) {
                RectFill(rp, sx2,   my,           sx2,   my+(WORD)mh-1);
            }
            RectFill(rp, sx1, my-1 > my ? my-1 : my, sx2-1, my-1 > my ? my-1 : my);

            /* Bright (pen 1) 3-px thick inner frame via four strips */
            SetAPen(rp, 1);
            /* Top */
            RectFill(rp, sx1,        my,              sx2-1,        my+bsz-1);
            /* Bottom */
            RectFill(rp, sx1,        my+(WORD)mh-bsz, sx2-1,        my+(WORD)mh-1);
            /* Left */
            RectFill(rp, sx1,        my+bsz,          sx1+bsz-1,    my+(WORD)mh-bsz-1);
            /* Right */
            RectFill(rp, sx2-bsz,    my+bsz,          sx2-1,        my+(WORD)mh-bsz-1);
        }

#undef MAP_X

        /* Axis labels — lo/hi cylinder only; free space is shown in info area */
        {
            char lo_str[24], hi_str[24];
            WORD label_y = by + (WORD)bh + 2 + fb;

            sprintf(lo_str, "Cyl %lu", (unsigned long)lo);
            sprintf(hi_str, "Cyl %lu", (unsigned long)hi);

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
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Drag resize info — replaces axis labels during an active drag       */
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
    sprintf(info, "%s: Cyl %lu - %lu  (%s)",
            pi->drive_name[0] ? pi->drive_name : "(none)",
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
/* New-partition drag overlay — drawn on top of the map during drag    */
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
    bpc   = rdb->heads * rdb->sectors * 512UL;
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
    sprintf(info, "New: Cyl %lu - %lu  (%s)",
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
/* Disk information section — drawn as text rows above the map         */
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
    /* Checkbox gadgets occupy the right side of line 3 — leave a gap there.
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
            sprintf(model, "%s %s", rdb->disk_vendor, rdb->disk_product);
        model[35] = '\0';

        if (rdb && rdb->cylinders > 0) {
            FormatSize((UQUAD)rdb->cylinders * rdb->heads * rdb->sectors * 512, sz);
        } else {
            strncpy(sz, "unknown", 15); sz[15] = '\0';
        }

        if (model[0])
            sprintf(line1, "Device: %s/%lu    Size: %s    Model: %s",
                    devname, (unsigned long)unit, sz, model);
        else
            sprintf(line1, "Device: %s/%lu    Size: %s",
                    devname, (unsigned long)unit, sz);
    }

    /* Line 2: full geometry so large cylinder counts never clip */
    if (rdb && rdb->cylinders > 0)
        sprintf(line2, "Geometry: %lu x %lu x %lu  (CYL x HD x SEC)",
                (unsigned long)rdb->cylinders,
                (unsigned long)rdb->heads,
                (unsigned long)rdb->sectors);
    else
        strncpy(line2, "Geometry: unknown", 119);

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
        FormatSize((UQUAD)free_cyls * rdb->heads * rdb->sectors * 512UL, fsz);
        sprintf(line3, "RDB: %u partition%s         Free: %s",
                (unsigned)rdb->num_parts,
                rdb->num_parts == 1 ? "" : "s", fsz);
    } else {
        strncpy(line3, "RDB: Not found", 119);
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
/* Column header — drawn just above the listview gadget                */
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
        const char *label = lv_hdr[i];
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
    /* draw_info erases the full info area including the checkbox slots —
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
    struct TagItem detach[]   = { { GTLV_Labels, ~0UL              }, { TAG_DONE, 0 } };
    struct TagItem reattach[] = { { GTLV_Labels, (ULONG)&part_list }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(lv_gad, win, NULL, detach);
    build_part_list(rdb, sel);
    GT_SetGadgetAttrsA(lv_gad, win, NULL, reattach);
}

/* ------------------------------------------------------------------ */
/* Free cylinder range                                                 */
/* ------------------------------------------------------------------ */

/* Find the lowest N such that "DH<N>" is not already used by any partition
   in this RDB. Comparison is case-insensitive (AmigaOS names are uppercase
   by convention but guard against mixed-case entries). */
static void next_drive_name(const struct RDBInfo *rdb, char *buf)
{
    ULONG n;
    for (n = 0; n <= MAX_PARTITIONS; n++) {
        UWORD k;
        BOOL  taken = FALSE;
        char  cand[8];
        UWORD ci;
        sprintf(cand, "DH%lu", n);
        for (k = 0; k < rdb->num_parts; k++) {
            const char *ex = rdb->parts[k].drive_name;
            /* Case-insensitive compare */
            for (ci = 0; ; ci++) {
                char a = cand[ci], b = ex[ci];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                if (a != b) break;
                if (a == '\0') { taken = TRUE; break; }
            }
            if (taken) break;
        }
        if (!taken) { strncpy(buf, cand, 31); buf[31] = '\0'; return; }
    }
    strncpy(buf, "DH0", 31);   /* fallback, shouldn't happen */
}

static void find_free_range(const struct RDBInfo *rdb, ULONG *lo, ULONG *hi)
{
    /* Sort partition ranges by low_cyl (insertion sort — n is small),
       then scan for the first gap including holes left by deleted partitions. */
    ULONG  starts[MAX_PARTITIONS];
    ULONG  ends[MAX_PARTITIONS];
    UWORD  n = rdb->num_parts;
    UWORD  i, j;
    ULONG  cursor;

    for (i = 0; i < n; i++) {
        starts[i] = rdb->parts[i].low_cyl;
        ends[i]   = rdb->parts[i].high_cyl;
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
/* Main window layout — extracted so it can be rebuilt on resize       */
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

            InitRastPort(&rp);
            SetFont(&rp, tf);

            lv_cols[LVCOL_MARK].x = cx;
            lv_cols[LVCOL_MARK].w = (UWORD)TextLength(&rp, ">", 1);
            cx += lv_cols[LVCOL_MARK].w + gap;

            lv_cols[LVCOL_DRIVE].x = cx;
            lv_cols[LVCOL_DRIVE].w = MAXW("DH10    ", 8, "Drive", 5);
            cx += lv_cols[LVCOL_DRIVE].w + gap;

            lv_cols[LVCOL_LOCYL].x = cx;
            lv_cols[LVCOL_LOCYL].w = MAXW("9999999", 7, "Lo Cyl", 6);
            cx += lv_cols[LVCOL_LOCYL].w + gap;

            lv_cols[LVCOL_HICYL].x = cx;
            lv_cols[LVCOL_HICYL].w = MAXW("9999999", 7, "Hi Cyl", 6);
            cx += lv_cols[LVCOL_HICYL].w + gap;

            lv_cols[LVCOL_FS].x = cx;
            lv_cols[LVCOL_FS].w = MAXW("FFS+IntlOFS ", 12, "FileSystem", 10);
            cx += lv_cols[LVCOL_FS].w + gap;

            lv_cols[LVCOL_SIZE].x = cx;
            lv_cols[LVCOL_SIZE].w = MAXW("1000.0 MB", 9, "Size", 4);
            cx += lv_cols[LVCOL_SIZE].w + gap + 8;

            lv_cols[LVCOL_BOOT].x = cx;
            lv_cols[LVCOL_BOOT].w = MAXW("-128", 4, "Boot", 4);

#undef MAXW
            CloseFont(tf);
        }
    }

    /* Set up the render hook — lv_render() captures a0/a1/a2 via
       register-variable declarations at function entry */
    lv_hook.h_Entry    = (HOOKFUNC)lv_render;
    lv_hook.h_SubEntry = NULL;
    lv_hook.h_Data     = NULL;

    gctx = CreateContext(&glist);
    if (!gctx) return FALSE;

    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr   = font_ta;

    /* Partition listview — render hook draws columns at computed pixel positions */
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

    MKBTN(bor_l+pad,                     "Init RDB", GID_INITRDB)
    MKBTN(bor_l+pad+(eighth+pad)*1,      "Add",      GID_ADD)
    MKBTN(bor_l+pad+(eighth+pad)*2,      "Edit",     GID_EDIT)
    MKBTN(bor_l+pad+(eighth+pad)*3,      "Delete",   GID_DELETE)
    MKBTN(bor_l+pad+(eighth+pad)*4,      "Move",     GID_MOVE)
    MKBTN(bor_l+pad+(eighth+pad)*5,      "FileSys",  GID_FILESYS)
    MKBTN(bor_l+pad+(eighth+pad)*6,      "Write",    GID_WRITE)
    MKBTN(bor_l+pad+(eighth+pad)*7,      "Back",     GID_BACK)
#undef MKBTN
    }

    /* Last Disk / Last LUN checkboxes — right-aligned on info line 3 */
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
        ng.ng_GadgetText = "Last Disk";
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
        ng.ng_GadgetText = "Last LUN";
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
/* Advanced menu — Backup / Restore RDB block                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

static struct NewMenu partview_menu_def[] = {
    /* Menu 0 — application */
    { NM_TITLE, DISKPART_VERTITLE,        NULL,         0, 0, NULL },
    { NM_ITEM,  "About...",              NULL,         0, 0, NULL },  /* ITEM 0 */
    /* Menu 1 — Advanced: backup / restore operations */
    { NM_TITLE, "Advanced",              NULL,         0, 0, NULL },
    { NM_ITEM,  "Backup RDB Block",      NULL,         0, 0, NULL },  /* ITEM 0 */
    { NM_ITEM,  "Restore RDB Block",     NULL,         0, 0, NULL },  /* ITEM 1 */
    { NM_ITEM,  NM_BARLABEL,             NULL,         0, 0, NULL },  /* ITEM 2 */
    { NM_ITEM,  "Extended Backup...",    NULL,         0, 0, NULL },  /* ITEM 3 */
    { NM_ITEM,  "Extended Restore...",   NULL,         0, 0, NULL },  /* ITEM 4 */
    /* Menu 2 — Debug: low-level inspection tools */
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

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* check_ffs_root — show what FFS would find at the expected root      */
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
    static char       wfmt[128];            /* formatted write-fail message — static: off stack */
    static char       win_title[80];

    /* Drag resize state */
    WORD  drag_part    = -1;   /* -1 = not dragging */
    WORD  drag_edge    = 0;    /* 0 = left (low_cyl), 1 = right (high_cyl) */
    ULONG drag_min     = 0;
    ULONG drag_max     = 0;
    ULONG drag_orig_lo = 0;   /* saved low_cyl  before drag */
    ULONG drag_orig_hi = 0;   /* saved high_cyl before drag */

    /* Double-click detection in map */
    ULONG dbl_sec   = 0;
    ULONG dbl_mic   = 0;
    WORD  dbl_part  = -1;   /* partition clicked last time */

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
    bg_pen = rdb_pen = -1;

    /* ---- Open device, read RDB, get geometry if needed ---- */
    bd = BlockDev_Open(devname, unit);

    rdb = (struct RDBInfo *)AllocVec(sizeof(*rdb), MEMF_PUBLIC | MEMF_CLEAR);
    if (!rdb) goto cleanup;

    if (bd) {
        RDB_Read(bd, rdb);
        /* Fill in any missing partition names from the AmigaDOS DosList.
           Some disks have pb_DriveName[0]=0 (no BSTR name on disk);
           the OS names the partition at boot time and we can recover
           that name by matching device+unit+lo_cyl+hi_cyl in the list. */
        /* nothing extra — names and DosTypes come from disk (PART/FSHD blocks) */
        if (!rdb->valid) {
            struct DriveGeometry geom;
            memset(&geom, 0, sizeof(geom));
            bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
            bd->iotd.iotd_Req.io_Length  = sizeof(geom);
            bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
            bd->iotd.iotd_Req.io_Flags   = 0;
            if (DoIO((struct IORequest *)&bd->iotd) == 0 &&
                geom.dg_Heads > 0 && geom.dg_TrackSectors > 0) {
                ULONG cyls = geom.dg_Cylinders;
                /* dg_Cylinders can be CHS-limited (e.g. 4096 max).
                   Use dg_TotalSectors to compute the real cylinder count. */
                if (geom.dg_TotalSectors > 0) {
                    ULONG cyls_ts = geom.dg_TotalSectors /
                                    (geom.dg_Heads * geom.dg_TrackSectors);
                    if (cyls_ts > cyls) cyls = cyls_ts;
                }
                rdb->cylinders = cyls;
                rdb->heads     = geom.dg_Heads;
                rdb->sectors   = geom.dg_TrackSectors;
            }
        }
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

        sprintf(win_title, DISKPART_VERTITLE " - %s unit %lu", devname, (unsigned long)unit);

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
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
        menu = CreateMenusA(partview_menu_def, NULL);
        if (menu) {
            LayoutMenusA(menu, vi, lt);
            SetMenuStrip(win, menu);
        }
    }

    GT_RefreshWindow(win, NULL);
    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                ix, iy, iw, bx, by, bw, bh, hx, hy, hw, sel, lastdisk_gad, lastlun_gad);

    /* ---- Event loop ---- */
    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG          iclass  = imsg->Class;
                UWORD          code    = imsg->Code;
                UWORD          qual    = imsg->Qualifier;
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
                        /* Debug menu */
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 0)
                            rdb_view_block(win, bd, rdb);
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 1)
                            rdb_raw_scan(win, bd);
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 2)
                            raw_hex_dump(win, bd);
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 4)
                            raw_disk_read(win, bd);
                        else if (MENUNUM(mcode) == 2 && ITEMNUM(mcode) == 6)
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
                        es.es_TextFormat   = (UBYTE *)"You have unsaved changes.\nWrite partition table to disk?";
                        es.es_GadgetFormat = (UBYTE *)"Write|Discard|Cancel";
                        r = EasyRequest(win, &es, NULL);
                        if (r == 0) break;           /* Cancel — stay */
                        if (r == 1 && bd) {          /* Write */
                            if (RDB_Write(bd, rdb)) {
                                dirty = FALSE;
                                if (needs_reboot) {
                                    es.es_TextFormat   = (UBYTE *)"Partition table written.\nReboot now for changes to take effect.";
                                    es.es_GadgetFormat = (UBYTE *)"Reboot|Later";
                                    if (EasyRequest(win, &es, NULL) == 1)
                                        ColdReboot();
                                }
                            } else {
                                sprintf(wfmt, "Write failed (err %d)!\nCheck device and try again.",
                                        (int)bd->last_io_err);
                                es.es_TextFormat   = (UBYTE *)wfmt;
                                es.es_GadgetFormat = (UBYTE *)"OK";
                                EasyRequest(win, &es, NULL);
                                break; /* stay open */
                            }
                        }
                        /* r == 2: Discard — fall through to exit */
                    } else {
                        es.es_TextFormat   = (UBYTE *)"Exit DiskPart?";
                        es.es_GadgetFormat = (UBYTE *)"Yes|No";
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
                            WORD  edge  = 0;
                            WORD  part  = hit_test_edge(rdb, mx2, mw2, total,
                                                         mouse_x, &edge);
                            if (part >= 0) {
                                /* On an edge — start drag, save originals */
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
                                drag_edge      = edge;
                                drag_orig_lo   = rdb->parts[part].low_cyl;
                                drag_orig_hi   = rdb->parts[part].high_cyl;
                                drag_move_part = -1;
                                dbl_part       = -1;
                                if (edge == 0) {
                                    drag_min = left_end;
                                    drag_max = rdb->parts[part].high_cyl;
                                } else {
                                    drag_min = rdb->parts[part].low_cyl;
                                    drag_max = right_start > 0 ? right_start - 1 : 0;
                                }
                            } else {
                                /* Inside a partition block — check double-click */
                                WORD blk = hit_test_partition(rdb, mx2, mw2,
                                                               total, mouse_x);
                                if (blk >= 0) {
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
                                                                 "Edit Partition", rdb)) {
                                                offer_ffs_grow(win, bd, rdb,
                                                               &rdb->parts[sel], old_hi);
                                                offer_pfs_grow(win, bd, rdb,
                                                               &rdb->parts[sel], old_hi);
                                                offer_sfs_grow(win, bd, rdb,
                                                               &rdb->parts[sel], old_hi);
                                                dirty = TRUE;
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
                                } else if (rdb->num_parts < MAX_PARTITIONS &&
                                           rdb->heads > 0 && rdb->sectors > 0) {
                                    /* Empty space — start new-partition drag */
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
                                char msg[128];
                                sprintf(msg,
                                    "Partition %s has been resized.\n"
                                    "Existing data may be lost!\n"
                                    "Keep this change?",
                                    rdb->parts[confirmed_part].drive_name);
                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)"Resize Partition";
                                es.es_TextFormat   = (UBYTE *)msg;
                                es.es_GadgetFormat = (UBYTE *)"Yes|No";
                                if (EasyRequest(win, &es, NULL) == 1) {
                                    dirty = TRUE; needs_reboot = TRUE;
                                    offer_ffs_grow(win, bd, rdb,
                                                   &rdb->parts[confirmed_part],
                                                   drag_orig_hi);
                                    offer_pfs_grow(win, bd, rdb,
                                                   &rdb->parts[confirmed_part],
                                                   drag_orig_hi);
                                    offer_sfs_grow(win, bd, rdb,
                                                   &rdb->parts[confirmed_part],
                                                   drag_orig_hi);
                                } else {
                                    /* Revert */
                                    rdb->parts[confirmed_part].low_cyl  = drag_orig_lo;
                                    rdb->parts[confirmed_part].high_cyl = drag_orig_hi;
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
                                new_pi.block_size    = 512;
                                new_pi.boot_pri      = (rdb->num_parts == 0) ? 0 : -128;
                                new_pi.reserved_blks = 2;
                                new_pi.interleave    = 0;
                                new_pi.max_transfer  = 0x7FFFFFFFUL;
                                new_pi.mask          = 0x7FFFFFFCUL;
                                new_pi.num_buffer    = 30;
                                new_pi.buf_mem_type  = 0;
                                new_pi.boot_blocks   = 0;
                                new_pi.baud          = 0;
                                new_pi.control       = 0;
                                new_pi.dev_flags     = 0;
                                if (partition_dialog(&new_pi, "Add Partition", rdb)) {
                                    rdb->parts[rdb->num_parts++] = new_pi;
                                    dirty = TRUE; needs_reboot = TRUE;
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
                                /* Partition was dragged — open move dialog pre-filled */
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
                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        /* double-click → open Edit dialog */
                        if ((qual & IEQUALIFIER_DOUBLECLICK) &&
                            sel >= 0 && sel < (WORD)rdb->num_parts) {
                            ULONG old_hi = rdb->parts[sel].high_cyl;
                            if (partition_dialog(&rdb->parts[sel],
                                                 "Edit Partition", rdb)) {
                                offer_ffs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                offer_pfs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                offer_sfs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                dirty = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                            }
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
                        struct DriveGeometry geom;
                        ULONG real_cyls = 0, real_heads = 0, real_secs = 0;

                        /* Always fetch actual geometry from the device */
                        if (bd) {
                            memset(&geom, 0, sizeof(geom));
                            bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
                            bd->iotd.iotd_Req.io_Length  = sizeof(geom);
                            bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
                            bd->iotd.iotd_Req.io_Flags   = 0;
                            if (DoIO((struct IORequest *)&bd->iotd) == 0 &&
                                geom.dg_Heads > 0 && geom.dg_TrackSectors > 0) {
                                real_cyls  = geom.dg_Cylinders;
                                real_heads = geom.dg_Heads;
                                real_secs  = geom.dg_TrackSectors;
                                /* dg_Cylinders can be CHS-limited (e.g. 4096 max).
                                   Use dg_TotalSectors for the real cylinder count. */
                                if (geom.dg_TotalSectors > 0) {
                                    ULONG cyls_ts = geom.dg_TotalSectors /
                                                    (real_heads * real_secs);
                                    if (cyls_ts > real_cyls) real_cyls = cyls_ts;
                                }
                            }
                        }
                        /* Fall back to whatever we already know */
                        if (real_cyls == 0) real_cyls  = rdb->cylinders;
                        if (real_cyls == 0) {
                            /* Auto-detection failed — offer manual entry */
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

                                sprintf(msg,
                                    "Disk already has an RDB with %u partition%s.\n\n"
                                    "Device geometry: %lu cyl x %lu hd x %lu sec\n"
                                    "RDB geometry:    %lu cyl x %lu hd x %lu sec\n\n"
                                    "Re-init: wipe all partitions, create fresh RDB.\n"
                                    "Update Geometry (EXPERIMENTAL): keep partitions,\n"
                                    "  update RDB to match device size.\n"
                                    "Manual...: enter geometry by hand.",
                                    (unsigned)rdb->num_parts,
                                    rdb->num_parts == 1 ? "" : "s",
                                    (unsigned long)real_cyls,
                                    (unsigned long)real_heads,
                                    (unsigned long)real_secs,
                                    (unsigned long)rdb->cylinders,
                                    (unsigned long)rdb->heads,
                                    (unsigned long)rdb->sectors);

                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)"Init RDB";
                                es.es_TextFormat   = (UBYTE *)msg;
                                es.es_GadgetFormat =
                                    (UBYTE *)"Re-init|Update Geometry|Manual...|Cancel";
                                choice = EasyRequest(win, &es, NULL);

                                if (choice == 1) {
                                    /* Re-init */
                                    RDB_InitFresh(rdb, real_cyls, real_heads, real_secs);
                                    { struct TagItem st[]={{GTCB_Checked,0},{TAG_DONE,0}};
                                      if (lastdisk_gad) { st[0].ti_Data=(rdb->flags&RDBFF_LAST)?1:0;    GT_SetGadgetAttrsA(lastdisk_gad,win,NULL,st); }
                                      if (lastlun_gad)  { st[0].ti_Data=(rdb->flags&RDBFF_LASTLUN)?1:0; GT_SetGadgetAttrsA(lastlun_gad, win,NULL,st); } }
                                    sel   = -1;
                                    dirty = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else if (choice == 2) {
                                    /* Update Geometry (EXPERIMENTAL) */
                                    rdb->cylinders = real_cyls;
                                    rdb->heads     = real_heads;
                                    rdb->sectors   = real_secs;
                                    rdb->hi_cyl    = real_cyls - 1;
                                    dirty = TRUE;
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else if (choice == 3) {
                                    /* Manual — re-enter geometry, then re-show dialog */
                                    if (geometry_dialog(real_cyls, real_heads, real_secs,
                                                        &real_cyls, &real_heads, &real_secs))
                                        geom_retry = TRUE;
                                }
                                /* choice == 0: Cancel — exit loop */
                            }
                        } else {
                            /* No RDB — confirm and create fresh */
                            BOOL geom_retry = TRUE;
                            while (geom_retry) {
                                LONG choice;
                                geom_retry = FALSE;

                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)"Init RDB";
                                es.es_TextFormat   =
                                    (UBYTE *)"Create a new RDB on this disk?\n"
                                             "All existing data will be lost.\n\n"
                                             "Manual...: enter geometry by hand.";
                                es.es_GadgetFormat = (UBYTE *)"Yes|Manual...|No";
                                choice = EasyRequest(win, &es, NULL);

                                if (choice == 1) {
                                    /* Yes */
                                    RDB_InitFresh(rdb, real_cyls, real_heads, real_secs);
                                    { struct TagItem st[]={{GTCB_Checked,0},{TAG_DONE,0}};
                                      if (lastdisk_gad) { st[0].ti_Data=(rdb->flags&RDBFF_LAST)?1:0;    GT_SetGadgetAttrsA(lastdisk_gad,win,NULL,st); }
                                      if (lastlun_gad)  { st[0].ti_Data=(rdb->flags&RDBFF_LASTLUN)?1:0; GT_SetGadgetAttrsA(lastlun_gad, win,NULL,st); } }
                                    sel   = -1;
                                    dirty = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                    draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                ix, iy, iw, bx, by, bw, bh,
                                                hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                } else if (choice == 2) {
                                    /* Manual — re-enter geometry, then re-show dialog */
                                    if (geometry_dialog(real_cyls, real_heads, real_secs,
                                                        &real_cyls, &real_heads, &real_secs))
                                        geom_retry = TRUE;
                                }
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
                        new_pi.block_size    = 512;
                        new_pi.boot_pri      = (rdb->num_parts == 0) ? 0 : -128;
                        new_pi.reserved_blks = 2;
                        new_pi.interleave    = 0;
                        new_pi.max_transfer  = 0x7FFFFFFFUL;
                        new_pi.mask          = 0x7FFFFFFCUL;
                        new_pi.num_buffer    = 30;
                        new_pi.buf_mem_type  = 0;
                        new_pi.boot_blocks   = 0;
                        new_pi.baud          = 0;
                        new_pi.control       = 0;
                        new_pi.dev_flags     = 0;
                        /* flags: 0 = bootable */

                        if (partition_dialog(&new_pi, "Add Partition", rdb)) {
                            rdb->parts[rdb->num_parts++] = new_pi;
                            dirty = TRUE; needs_reboot = TRUE;
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
                                                 "Edit Partition", rdb)) {
                                offer_ffs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                offer_pfs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                offer_sfs_grow(win, bd, rdb,
                                               &rdb->parts[sel], old_hi);
                                dirty = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                            }
                        }
                        break;

                    case GID_DELETE:
                        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                            struct EasyStruct es;
                            char msg[128];
                            sprintf(msg,
                                "Delete partition %s?\n"
                                "All data on this partition will be lost!",
                                rdb->parts[sel].drive_name);
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)"Delete Partition";
                            es.es_TextFormat   = (UBYTE *)msg;
                            es.es_GadgetFormat = (UBYTE *)"Yes|No";
                            if (EasyRequest(win, &es, NULL) == 1) {
                                UWORD j;
                                for (j=(UWORD)sel; j+1 < rdb->num_parts; j++)
                                    rdb->parts[j] = rdb->parts[j+1];
                                rdb->num_parts--;
                                dirty = TRUE; needs_reboot = TRUE;
                                if (sel >= (WORD)rdb->num_parts)
                                    sel = (WORD)rdb->num_parts - 1;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
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
                            es.es_TextFormat   = (UBYTE *)"No RDB found.\nInit RDB first.";
                            es.es_GadgetFormat = (UBYTE *)"OK";
                            EasyRequest(win, &es, NULL);
                        } else {
                            if (filesystem_manager_dialog(rdb))
                                dirty = TRUE;
                        }
                        break;

                    case GID_WRITE: {
                        struct EasyStruct es;
                        es.es_StructSize   = sizeof(es);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)DISKPART_VERTITLE;
                        es.es_TextFormat   = (UBYTE *)"Write partition table to disk?\nAll existing data may be lost!";
                        es.es_GadgetFormat = (UBYTE *)"Write|Cancel";
                        if (EasyRequest(win, &es, NULL) == 1) {
                            if (!bd || !RDB_Write(bd, rdb)) {
                                if (bd && bd->last_io_err == 1)
                                    sprintf(wfmt,
                                        "Verify fail blk %lu off %lu\n"
                                        "W:%02X%02X%02X%02X R:%02X%02X%02X%02X",
                                        (unsigned long)bd->last_verify_block,
                                        (unsigned long)bd->last_verify_off,
                                        bd->last_wrote[0], bd->last_wrote[1],
                                        bd->last_wrote[2], bd->last_wrote[3],
                                        bd->last_read[0],  bd->last_read[1],
                                        bd->last_read[2],  bd->last_read[3]);
                                else if (bd && bd->last_io_err == 0)
                                    sprintf(wfmt,
                                        "Write rejected: RDB metadata (%lu blocks)\n"
                                        "would overflow the reserved area.\n"
                                        "Remove filesystem drivers to free space.",
                                        (unsigned long)rdb->num_fs);
                                else
                                    sprintf(wfmt, "Write failed (err %d)!\nCheck device and try again.",
                                        bd ? (int)bd->last_io_err : 0);
                                es.es_TextFormat   = (UBYTE *)wfmt;
                                es.es_GadgetFormat = (UBYTE *)"OK";
                                EasyRequest(win, &es, NULL);
                            } else {
                                dirty = FALSE;
                                if (BlockDev_HasMBR(bd)) {
                                    es.es_TextFormat   = (UBYTE *)"PC partition table (MBR) found on block 0.\nErase it?";
                                    es.es_GadgetFormat = (UBYTE *)"Erase|Keep";
                                    if (EasyRequest(win, &es, NULL) == 1)
                                        BlockDev_EraseMBR(bd);
                                }
                                if (needs_reboot) {
                                    es.es_TextFormat   = (UBYTE *)"Partition table written.\nReboot now for changes to take effect.";
                                    es.es_GadgetFormat = (UBYTE *)"Reboot|Later";
                                    if (EasyRequest(win, &es, NULL) == 1)
                                        ColdReboot();
                                    else
                                        needs_reboot = FALSE;
                                }
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
                            es.es_TextFormat   = (UBYTE *)"You have unsaved changes.\nWrite partition table to disk?";
                            es.es_GadgetFormat = (UBYTE *)"Write|Discard|Cancel";
                            r = EasyRequest(win, &es, NULL);
                            if (r == 0) break;           /* Cancel — stay */
                            if (r == 1 && bd) {          /* Write */
                                if (RDB_Write(bd, rdb)) {
                                    dirty = FALSE;
                                    if (needs_reboot) {
                                        es.es_TextFormat   = (UBYTE *)"Partition table written.\nReboot now for changes to take effect.";
                                        es.es_GadgetFormat = (UBYTE *)"Reboot|Later";
                                        if (EasyRequest(win, &es, NULL) == 1)
                                            ColdReboot();
                                    }
                                } else {
                                    sprintf(wfmt, "Write failed (err %d)!\nCheck device and try again.",
                                            (int)bd->last_io_err);
                                    es.es_TextFormat   = (UBYTE *)wfmt;
                                    es.es_GadgetFormat = (UBYTE *)"OK";
                                    EasyRequest(win, &es, NULL);
                                    break; /* stay open */
                                }
                            }
                            /* r == 2: Discard — fall through to exit */
                        }
                        running = FALSE; break;
                    }
                    break;

                case IDCMP_NEWSIZE: {
                    struct Gadget    *new_glist = NULL, *new_lv = NULL;
                    struct Gadget    *new_ldisk = NULL, *new_llun = NULL;
                    struct PartLayout new_lay;
                    UWORD fh = (UWORD)win->WScreen->Font->ta_YSize;

                    /* Cancel any in-progress drag — restore partition to pre-drag state */
                    if (drag_part >= 0) {
                        rdb->parts[drag_part].low_cyl  = drag_orig_lo;
                        rdb->parts[drag_part].high_cyl = drag_orig_hi;
                        drag_part = -1;
                    }

                    RemoveGList(win, glist, -1);
                    FreeGadgets(glist);
                    glist = NULL; lv_gad = NULL;
                    lastdisk_gad = NULL; lastlun_gad = NULL;

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

                        /* Restore listview selection */
                        if (sel >= 0) {
                            struct TagItem st[] = {
                                { GTLV_Selected,    (ULONG)sel },
                                { GTLV_MakeVisible, (ULONG)sel },
                                { TAG_DONE, 0 }
                            };
                            GT_SetGadgetAttrsA(lv_gad, win, NULL, st);
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
                    break;
                }
            }
        }
    }

cleanup:
    if (win)   { ClearMenuStrip(win); if (glist) RemoveGList(win, glist, -1); CloseWindow(win); }
    if (menu)    FreeMenus(menu);
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    {
        struct Screen *ws = LockPubScreen(NULL);
        if (ws) { free_pens(ws); UnlockPubScreen(NULL, ws); }
    }
    if (rdb)  { RDB_FreeCode(rdb); FreeVec(rdb); }
    if (bd)   BlockDev_Close(bd);
    return exit_req;
}
