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
static void FriendlyDosType(ULONG dostype, char *buf);
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
                UQUAD bytes2 = (UQUAD)cyls2 * heads2 * secs2 * 512UL;
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
    UQUAD bytes = (UQUAD)cyls * rdb->heads * rdb->sectors * 512UL;
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
/* Built-in (ROM) filesystem types — always available                  */
/* ------------------------------------------------------------------ */

static const struct { const char *name; ULONG dostype; } builtin_fs[] = {
    { "OFS",      0x444F5300UL },
    { "FFS",      0x444F5301UL },
    { "FFS+Intl", 0x444F5303UL },
};
#define NUM_BUILTIN_FS 3

/* Returns a human-readable filesystem name: built-in friendly name if known,
   otherwise falls back to FormatDosType (e.g. "PFS\3").  buf >= 16 bytes. */
static void FriendlyDosType(ULONG dostype, char *buf)
{
    UWORD i;
    for (i = 0; i < NUM_BUILTIN_FS; i++) {
        if (builtin_fs[i].dostype == dostype) {
            strncpy(buf, builtin_fs[i].name, 15);
            buf[15] = '\0';
            return;
        }
    }
    FormatDosType(dostype, buf);
}

/* Block size — maps cycle index ↔ bytes value */
static const char * const blocksize_labels[] = {
    "512", "1024", "2048", "4096", "8192", "16384", "32768", NULL
};
static const ULONG blocksize_values[] = {
    512UL, 1024UL, 2048UL, 4096UL, 8192UL, 16384UL, 32768UL
};
#define NUM_BLOCKSIZES 7

static UWORD blocksize_index(ULONG bsz)
{
    UWORD i;
    for (i = 0; i < NUM_BLOCKSIZES; i++)
        if (blocksize_values[i] == bsz) return i;
    return 0;   /* default 512 */
}

/* BufMemType — maps cycle index ↔ MEMF_* value */
static const char * const bufmem_labels[] = {
    "Any", "Public", "Chip", "Fast", "24-bit DMA", NULL
};
static const ULONG bufmem_values[] = { 0UL, 1UL, 2UL, 4UL, 8UL };
#define NUM_BUFMEM_TYPES 5

static UWORD bufmem_index(ULONG val)
{
    UWORD i;
    for (i = 0; i < NUM_BUFMEM_TYPES; i++)
        if (bufmem_values[i] == val) return i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Numeric parsing helpers                                             */
/* ------------------------------------------------------------------ */

/* Parse decimal or 0x/0X-prefixed hex string to ULONG */
static ULONG parse_num(const char *s)
{
    ULONG val = 0;
    int   hex = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; hex = 1; }
    while (*s) {
        char  c = *s++;
        ULONG digit;
        if (hex) {
            if      (c >= '0' && c <= '9') digit = (ULONG)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (ULONG)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (ULONG)(c - 'A' + 10);
            else break;
            if (val > (0xFFFFFFFFUL - digit) / 16UL) return 0xFFFFFFFFUL;
            val = val * 16UL + digit;
        } else {
            if (c >= '0' && c <= '9') digit = (ULONG)(c - '0');
            else break;
            if (val > (0xFFFFFFFFUL - digit) / 10UL) return 0xFFFFFFFFUL;
            val = val * 10UL + digit;
        }
    }
    return val;
}

/* Parse signed decimal (handles leading '-') */
static LONG parse_long(const char *s)
{
    while (*s == ' ') s++;
    if (*s == '-') return -(LONG)parse_num(s + 1);
    return (LONG)parse_num(s);
}

/*
 * Parse a DosType from either hex ("0x50465303") or a 4-char string ("PFS\3").
 * String rules: up to 4 chars packed big-endian into a ULONG.
 *   \N  (backslash + single decimal digit 0-9) → byte value N (e.g. \3 → 0x03).
 *   Any other char → its ASCII value.
 * Examples: "PFS\3" → 0x50465303, "DOS\0" → 0x444F5300, "DOS\1" → 0x444F5301.
 */
static ULONG parse_dostype(const char *s)
{
    ULONG val = 0;
    UBYTE bytes[4];
    UBYTE i, nb;

    while (*s == ' ') s++;

    /* Hex if starts with 0x/0X or is all hex digits (8 chars) */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_num(s);

    /* String encoding: parse up to 4 characters */
    nb = 0;
    while (*s && nb < 4) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '9') {
            bytes[nb++] = (UBYTE)(s[1] - '0');
            s += 2;
        } else {
            bytes[nb++] = (UBYTE)*s++;
        }
    }
    /* Pad remaining bytes with 0 */
    for (i = nb; i < 4; i++) bytes[i] = 0;

    val = ((ULONG)bytes[0] << 24) | ((ULONG)bytes[1] << 16) |
          ((ULONG)bytes[2] <<  8) |  (ULONG)bytes[3];
    return val;
}

/* ------------------------------------------------------------------ */
/* Partition add / edit dialog                                          */
/*                                                                     */
/* Fields (single-column layout):                                      */
/*   Name, Lo Cylinder, Hi Cylinder, Filesystem, Boot Priority,        */
/*   Bootable (checkbox), Buffers, MaxTransfer, Mask                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Advanced dialog gadget IDs                                          */
/* ------------------------------------------------------------------ */
#define ADLG_BUFFERS     1
#define ADLG_BUFMEMTYPE  2
#define ADLG_BOOTBLOCKS  3
#define ADLG_MAXTRANSFER 4
#define ADLG_MASK        5
#define ADLG_OK          6
#define ADLG_CANCEL      7
#define ADLG_RESERVED    8
#define ADLG_INTERLEAVE  9
#define ADLG_CONTROL     10
#define ADLG_DEVFLAGS    11
#define ADLG_ROWS        9

/* ------------------------------------------------------------------ */
/* Main dialog gadget IDs                                              */
/* ------------------------------------------------------------------ */
#define PDLG_NAME        1
#define PDLG_SIZEMB      3
#define PDLG_TYPE        4
#define PDLG_BLOCKSIZE   13
#define PDLG_BOOTPRI     5
#define PDLG_BOOTABLE    6
#define PDLG_DIRSCSI     7
#define PDLG_OK          8
#define PDLG_ADVANCED    9
#define PDLG_CANCEL      10
#define PDLG_SYNCSCSI    11
#define PDLG_AUTOMOUNT   12

/* Rows: Name, LoCyl, SizeMB, FS, BlockSize, BootPri, Bootable+Automount, DirSCSI+SyncSCSI */
#define PDLG_ROWS 8

static void partition_advanced_dialog(struct PartInfo *pi)
{
    struct Screen  *scr          = NULL;
    APTR            vi           = NULL;
    struct Gadget  *glist        = NULL;
    struct Gadget  *gctx         = NULL;
    struct Gadget  *buffers_gad  = NULL;
    struct Gadget  *bootblks_gad = NULL;
    struct Gadget  *maxtrans_gad = NULL;
    struct Gadget  *mask_gad     = NULL;
    struct Gadget  *reserved_gad = NULL;
    struct Gadget  *interleave_gad = NULL;
    struct Gadget  *control_gad  = NULL;
    struct Gadget  *devflags_gad = NULL;
    struct Window  *win          = NULL;
    UWORD           cur_bufmem   = bufmem_index(pi->buf_mem_type);

    char buffers_str[16], bootblks_str[16], maxtrans_str[16], mask_str[16];
    char reserved_str[16], interleave_str[16], control_str[16], devflags_str[16];
    sprintf(buffers_str,    "%lu",     (unsigned long)(pi->num_buffer  > 0 ? pi->num_buffer  : 30));
    sprintf(bootblks_str,   "%lu",     (unsigned long)(pi->boot_blocks > 0 ? pi->boot_blocks :  2));
    sprintf(maxtrans_str,   "0x%08lX", (unsigned long)pi->max_transfer);
    sprintf(mask_str,       "0x%08lX", (unsigned long)pi->mask);
    sprintf(reserved_str,   "%lu",     (unsigned long)(pi->reserved_blks > 0 ? pi->reserved_blks : 2));
    sprintf(interleave_str, "%lu",     (unsigned long)pi->interleave);
    sprintf(control_str,    "0x%08lX", (unsigned long)pi->control);
    sprintf(devflags_str,   "0x%08lX", (unsigned long)pi->dev_flags);

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
        UWORD win_w   = 380;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 3;
        UWORD row_h   = font_h + 4;
        UWORD lbl_w   = 100;
        UWORD gad_x   = bor_l + lbl_w;
        UWORD gad_w   = inner_w - lbl_w - pad;
        UWORD win_h   = bor_t + pad
                      + (UWORD)ADLG_ROWS * (row_h + pad)
                      + row_h + pad + bor_b;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            struct Gadget *prev = gctx;
            UWORD row = 0;
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

#define ROW_Y(r) ((WORD)(bor_t + pad + (r) * (row_h + pad)))
#define STR_GAD(gid, lbl, initstr, maxch, pgad) \
    ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row); \
    ng.ng_Width=gad_w; ng.ng_Height=row_h; \
    ng.ng_GadgetText=(lbl); ng.ng_GadgetID=(gid); ng.ng_Flags=PLACETEXT_LEFT; \
    { struct TagItem st_[]={{GTST_String,(ULONG)(initstr)}, \
                            {GTST_MaxChars,(maxch)},{TAG_DONE,0}}; \
      *(pgad)=CreateGadgetA(STRING_KIND,prev,&ng,st_); \
      if (!*(pgad)) goto cleanup; prev=*(pgad); } row++;

            STR_GAD(ADLG_RESERVED,    "Reserved Blks", reserved_str,   6,  &reserved_gad)
            STR_GAD(ADLG_INTERLEAVE,  "Interleave",    interleave_str, 6,  &interleave_gad)
            STR_GAD(ADLG_BUFFERS,     "Buffers",       buffers_str,    6,  &buffers_gad)

            /* BufMemType cycle */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Buf Mem Type"; ng.ng_GadgetID=ADLG_BUFMEMTYPE;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem bmt[]={{GTCY_Labels,(ULONG)bufmem_labels},
                                    {GTCY_Active,(ULONG)cur_bufmem},{TAG_DONE,0}};
              prev=CreateGadgetA(CYCLE_KIND,prev,&ng,bmt);
              if (!prev) goto cleanup; }
            row++;

            STR_GAD(ADLG_BOOTBLOCKS,  "Boot Blocks",  bootblks_str,  6,  &bootblks_gad)
            STR_GAD(ADLG_MAXTRANSFER, "MaxTransfer",  maxtrans_str, 12, &maxtrans_gad)
            STR_GAD(ADLG_MASK,        "Mask",         mask_str,     12, &mask_gad)
            STR_GAD(ADLG_CONTROL,     "Control",      control_str,  12, &control_gad)
            STR_GAD(ADLG_DEVFLAGS,    "Dev Flags",    devflags_str, 12, &devflags_gad)

#undef STR_GAD
#undef ROW_Y

            /* OK / Cancel */
            {
                UWORD btn_y  = bor_t + pad + (UWORD)ADLG_ROWS * (row_h + pad);
                UWORD half_w = (inner_w - pad * 2 - pad) / 2;
                struct TagItem bt[] = { { TAG_DONE, 0 } };
                ng.ng_TopEdge=btn_y; ng.ng_Height=row_h;
                ng.ng_Width=half_w; ng.ng_Flags=PLACETEXT_IN;
                ng.ng_LeftEdge=bor_l+pad; ng.ng_GadgetText="OK";
                ng.ng_GadgetID=ADLG_OK;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
                if (!prev) goto cleanup;
                ng.ng_LeftEdge=bor_l+pad+half_w+pad;
                ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=ADLG_CANCEL;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
                if (!prev) goto cleanup;
            }
        }

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width -win_w)/2) },
                { WA_Top,       (ULONG)((scr->Height-win_h)/2) },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)"Advanced Partition Settings" },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                                WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto cleanup;
    GT_RefreshWindow(win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code   = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case ADLG_BUFMEMTYPE: cur_bufmem = (UWORD)code; break;
                    case ADLG_OK: {
                        struct StringInfo *si;
                        si = (struct StringInfo *)reserved_gad->SpecialInfo;
                        pi->reserved_blks = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)interleave_gad->SpecialInfo;
                        pi->interleave   = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)buffers_gad->SpecialInfo;
                        pi->num_buffer   = parse_num((char *)si->Buffer);
                        pi->buf_mem_type = bufmem_values[cur_bufmem];
                        si = (struct StringInfo *)bootblks_gad->SpecialInfo;
                        pi->boot_blocks  = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)maxtrans_gad->SpecialInfo;
                        pi->max_transfer = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)mask_gad->SpecialInfo;
                        pi->mask         = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)control_gad->SpecialInfo;
                        pi->control      = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)devflags_gad->SpecialInfo;
                        pi->dev_flags    = parse_num((char *)si->Buffer);
                        running = FALSE; break;
                    }
                    case ADLG_CANCEL: running = FALSE; break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
}

/* ------------------------------------------------------------------ */
/* Main partition add / edit dialog                                    */
/* ------------------------------------------------------------------ */

static BOOL partition_dialog(struct PartInfo *pi, const char *title,
                             const struct RDBInfo *rdb)
{
    struct Screen  *scr          = NULL;
    APTR            vi           = NULL;
    struct Gadget  *glist        = NULL;
    struct Gadget  *gctx         = NULL;
    struct Gadget  *name_gad     = NULL;
    struct Gadget  *sizemb_gad   = NULL;
    struct Gadget  *bootpri_gad  = NULL;
    struct Gadget  *boot_gad      = NULL;
    struct Gadget  *automount_gad = NULL;
    struct Gadget  *dirscsi_gad   = NULL;
    struct Gadget  *syncscsi_gad  = NULL;
    struct Window  *win          = NULL;
    BOOL            result       = FALSE;
    UWORD           cur_fs       = 1;   /* default FFS */
    UWORD           cur_bsz      = 0;   /* default 512 */

    /* Dynamic filesystem list: built-ins + whatever is in the RDB FSHD list */
#define MAX_DLG_FS (NUM_BUILTIN_FS + MAX_FILESYSTEMS + 1)
    char       dlg_fs_names[MAX_DLG_FS][16];
    const char *dlg_fs_labels[MAX_DLG_FS + 1];
    ULONG       dlg_fs_dostypes[MAX_DLG_FS];
    UWORD       dlg_num_fs = 0;
    {
        UWORD bi, fi, k;
        /* Built-in filesystems */
        for (bi = 0; bi < NUM_BUILTIN_FS; bi++) {
            dlg_fs_labels[dlg_num_fs]   = builtin_fs[bi].name;
            dlg_fs_dostypes[dlg_num_fs] = builtin_fs[bi].dostype;
            dlg_num_fs++;
        }
        /* Add filesystems present in the RDB FSHD list */
        for (fi = 0; fi < rdb->num_fs; fi++) {
            ULONG dt = rdb->filesystems[fi].dos_type;
            BOOL dup = FALSE;
            for (k = 0; k < dlg_num_fs; k++)
                if (dlg_fs_dostypes[k] == dt) { dup = TRUE; break; }
            if (!dup && dlg_num_fs < MAX_DLG_FS - 1) {
                FriendlyDosType(dt, dlg_fs_names[dlg_num_fs]);
                dlg_fs_labels[dlg_num_fs]   = dlg_fs_names[dlg_num_fs];
                dlg_fs_dostypes[dlg_num_fs] = dt;
                dlg_num_fs++;
            }
        }
        dlg_fs_labels[dlg_num_fs] = NULL;
        /* Find index matching current dos_type */
        for (k = 0; k < dlg_num_fs; k++)
            if (dlg_fs_dostypes[k] == pi->dos_type) { cur_fs = k; break; }
    }
    cur_bsz = blocksize_index(pi->block_size > 0 ? pi->block_size : 512);

    char locyl_str[16], sizemb_str[16], bootpri_str[16];
    {
        ULONG eff_heads = pi->heads   > 0 ? pi->heads   : rdb->heads;
        ULONG eff_secs  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
        ULONG eff_bsz   = pi->block_size > 0 ? pi->block_size : 512;
        ULONG bytes_per_cyl = eff_heads * eff_secs * eff_bsz;
        ULONG cyl_count     = (pi->high_cyl >= pi->low_cyl)
                              ? (pi->high_cyl - pi->low_cyl + 1) : 1;
        UQUAD total_bytes   = (bytes_per_cyl > 0)
                              ? (UQUAD)cyl_count * bytes_per_cyl : 0;
        ULONG size_mb       = (ULONG)(total_bytes / (1024ULL * 1024ULL));
        if (size_mb == 0 && total_bytes > 0) size_mb = 1;
        sprintf(sizemb_str, "%lu", (unsigned long)size_mb);
    }
    sprintf(locyl_str,   "Lo: %lu", (unsigned long)pi->low_cyl);
    sprintf(bootpri_str, "%ld", (long)pi->boot_pri);

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
        UWORD win_w   = 380;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 3;
        UWORD row_h   = font_h + 4;
        UWORD lbl_w   = 100;
        UWORD gad_x   = bor_l + lbl_w;
        UWORD gad_w   = inner_w - lbl_w - pad;
        UWORD win_h   = bor_t + pad
                      + (UWORD)PDLG_ROWS * (row_h + pad)
                      + row_h + pad
                      + bor_b;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            struct Gadget *prev;
            UWORD row = 0;
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

#define ROW_Y(r) ((WORD)(bor_t + pad + (r) * (row_h + pad)))
#define STR_GAD(gid, lbl, initstr, maxch, pgad) \
    ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row); \
    ng.ng_Width=gad_w;    ng.ng_Height=row_h; \
    ng.ng_GadgetText=(lbl); ng.ng_GadgetID=(gid); ng.ng_Flags=PLACETEXT_LEFT; \
    { struct TagItem st_[]={ {GTST_String,(ULONG)(initstr)}, \
                             {GTST_MaxChars,(maxch)}, {TAG_DONE,0} }; \
      *(pgad)=CreateGadgetA(STRING_KIND, prev, &ng, st_); \
      if (!*(pgad)) goto cleanup; prev=*(pgad); } row++;

            /* Row 0: Name */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Name"; ng.ng_GadgetID=PDLG_NAME;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem st[]={{GTST_String,(ULONG)pi->drive_name},
                                   {GTST_MaxChars,30},{TAG_DONE,0}};
              name_gad=CreateGadgetA(STRING_KIND,gctx,&ng,st);
              if (!name_gad) goto cleanup; prev=name_gad; }
            row++;

            /* Lo Cylinder — reference display only, not editable */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Cylinder"; ng.ng_GadgetID=0; ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem tt[]={{GTTX_Text,(ULONG)locyl_str},{TAG_DONE,0}};
              prev=CreateGadgetA(TEXT_KIND,prev,&ng,tt);
              if (!prev) goto cleanup; }
            row++;

            STR_GAD(PDLG_SIZEMB,  "Size (MB)",    sizemb_str,  10, &sizemb_gad)

            /* Filesystem (cycle) */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="FileSystem"; ng.ng_GadgetID=PDLG_TYPE;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem ct[]={{GTCY_Labels,(ULONG)dlg_fs_labels},
                                   {GTCY_Active,(ULONG)cur_fs},{TAG_DONE,0}};
              prev=CreateGadgetA(CYCLE_KIND,prev,&ng,ct);
              if (!prev) goto cleanup; }
            row++;

            /* Block Size (cycle) */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Block Size"; ng.ng_GadgetID=PDLG_BLOCKSIZE;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem bs[]={{GTCY_Labels,(ULONG)blocksize_labels},
                                   {GTCY_Active,(ULONG)cur_bsz},{TAG_DONE,0}};
              prev=CreateGadgetA(CYCLE_KIND,prev,&ng,bs);
              if (!prev) goto cleanup; }
            row++;

            STR_GAD(PDLG_BOOTPRI, "Boot Priority", bootpri_str, 8, &bootpri_gad)

            /* Row 5: Bootable [x]   Automount [x] */
            {
                BOOL is_bootable  = (BOOL)((pi->flags & 1) != 0);  /* PBFF_BOOTABLE */
                BOOL is_automount = (BOOL)((pi->flags & 2) == 0);  /* !PBFF_NOMOUNT */
                UWORD half = (inner_w - pad * 3) / 2;
                struct TagItem cbt[] = { { GTCB_Checked, 0 }, { TAG_DONE, 0 } };

                cbt[0].ti_Data = (ULONG)is_bootable;
                ng.ng_LeftEdge=bor_l+pad; ng.ng_TopEdge=ROW_Y(row);
                ng.ng_Width=half; ng.ng_Height=row_h;
                ng.ng_GadgetText="Bootable"; ng.ng_GadgetID=PDLG_BOOTABLE;
                ng.ng_Flags=PLACETEXT_RIGHT;
                boot_gad=CreateGadgetA(CHECKBOX_KIND,prev,&ng,cbt);
                if (!boot_gad) goto cleanup; prev=boot_gad;

                cbt[0].ti_Data=(ULONG)is_automount;
                ng.ng_LeftEdge=bor_l+pad+half+pad; ng.ng_TopEdge=ROW_Y(row);
                ng.ng_Width=half; ng.ng_Height=row_h;
                ng.ng_GadgetText="Automount"; ng.ng_GadgetID=PDLG_AUTOMOUNT;
                ng.ng_Flags=PLACETEXT_RIGHT;
                automount_gad=CreateGadgetA(CHECKBOX_KIND,prev,&ng,cbt);
                if (!automount_gad) goto cleanup; prev=automount_gad;
            }
            row++;

            /* Row 6: Direct SCSI [x]   Sync SCSI [x] */
            {
                BOOL is_dirscsi  = (BOOL)((pi->flags & 4) != 0);
                BOOL is_syncscsi = (BOOL)((pi->flags & 8) != 0);
                UWORD half = (inner_w - pad * 3) / 2;
                struct TagItem cbt[] = { { GTCB_Checked, 0 }, { TAG_DONE, 0 } };

                cbt[0].ti_Data=(ULONG)is_dirscsi;
                ng.ng_LeftEdge=bor_l+pad; ng.ng_TopEdge=ROW_Y(row);
                ng.ng_Width=half; ng.ng_Height=row_h;
                ng.ng_GadgetText="Direct SCSI"; ng.ng_GadgetID=PDLG_DIRSCSI;
                ng.ng_Flags=PLACETEXT_RIGHT;
                dirscsi_gad=CreateGadgetA(CHECKBOX_KIND,prev,&ng,cbt);
                if (!dirscsi_gad) goto cleanup; prev=dirscsi_gad;

                cbt[0].ti_Data=(ULONG)is_syncscsi;
                ng.ng_LeftEdge=bor_l+pad+half+pad; ng.ng_TopEdge=ROW_Y(row);
                ng.ng_Width=half; ng.ng_Height=row_h;
                ng.ng_GadgetText="Sync SCSI"; ng.ng_GadgetID=PDLG_SYNCSCSI;
                ng.ng_Flags=PLACETEXT_RIGHT;
                syncscsi_gad=CreateGadgetA(CHECKBOX_KIND,prev,&ng,cbt);
                if (!syncscsi_gad) goto cleanup; prev=syncscsi_gad;
            }
            row++;

#undef STR_GAD
#undef ROW_Y

            /* Three buttons: OK / Advanced... / Cancel */
            {
                UWORD btn_y  = bor_t + pad + (UWORD)PDLG_ROWS * (row_h + pad);
                UWORD third  = (inner_w - pad * 2 - pad * 2) / 3;
                struct TagItem bt[] = { { TAG_DONE, 0 } };
                ng.ng_TopEdge=btn_y; ng.ng_Height=row_h;
                ng.ng_Width=third; ng.ng_Flags=PLACETEXT_IN;
                ng.ng_LeftEdge=bor_l+pad;
                ng.ng_GadgetText="OK"; ng.ng_GadgetID=PDLG_OK;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if (!prev) goto cleanup;
                ng.ng_LeftEdge=bor_l+pad+third+pad;
                ng.ng_GadgetText="Advanced..."; ng.ng_GadgetID=PDLG_ADVANCED;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if (!prev) goto cleanup;
                ng.ng_LeftEdge=bor_l+pad+(third+pad)*2;
                ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=PDLG_CANCEL;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if (!prev) goto cleanup;
            }
        }

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width -win_w)/2) },
                { WA_Top,       (ULONG)((scr->Height-win_h)/2) },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)title },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                                WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto cleanup;

    GT_RefreshWindow(win, NULL);
    ActivateGadget(name_gad, win, NULL);

    {
        BOOL running    = TRUE;
        BOOL need_reboot = FALSE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code   = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case PDLG_TYPE:      cur_fs  = (UWORD)code; break;
                    case PDLG_BLOCKSIZE: cur_bsz = (UWORD)code; break;
                    case PDLG_ADVANCED:
                        partition_advanced_dialog(pi);
                        break;
                    case PDLG_OK: {
                        ULONG old_dos_type   = pi->dos_type;
                        ULONG old_block_size = pi->block_size > 0 ? pi->block_size : 512;
                        ULONG new_dos_type   = dlg_fs_dostypes[cur_fs];
                        ULONG new_block_size = blocksize_values[cur_bsz];
                        BOOL  destructive    = (new_dos_type   != old_dos_type ||
                                                new_block_size != old_block_size);
                        if (destructive) {
                            struct EasyStruct es = {
                                sizeof(struct EasyStruct), 0,
                                DISKPART_VERTITLE " - Warning",
                                "Changing the filesystem or block size\n"
                                "will DESTROY ALL DATA on %s.\n\n"
                                "Continue?",
                                "Yes, destroy data|Cancel"
                            };
                            if (!EasyRequest(win, &es, NULL,
                                             (ULONG)pi->drive_name, TAG_DONE))
                                break; /* user cancelled — stay in dialog */
                        }
                        {
                        struct StringInfo *si;
                        si = (struct StringInfo *)name_gad->SpecialInfo;
                        strncpy(pi->drive_name, (char *)si->Buffer,
                                sizeof(pi->drive_name)-1);
                        pi->block_size = new_block_size;
                        /* Convert Size (MB) to high_cyl */
                        {
                            ULONG eff_heads = pi->heads   > 0 ? pi->heads   : rdb->heads;
                            ULONG eff_secs  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
                            ULONG eff_bsz   = pi->block_size > 0 ? pi->block_size : 512;
                            ULONG bytes_per_cyl = eff_heads * eff_secs * eff_bsz;
                            ULONG max_hi = rdb->hi_cyl;
                            UWORD k;
                            /* clamp to nearest occupied partition above lo_cyl */
                            for (k = 0; k < rdb->num_parts; k++) {
                                if (&rdb->parts[k] == pi) continue;
                                if (rdb->parts[k].low_cyl > pi->low_cyl &&
                                    rdb->parts[k].low_cyl - 1 < max_hi)
                                    max_hi = rdb->parts[k].low_cyl - 1;
                            }
                            si = (struct StringInfo *)sizemb_gad->SpecialInfo;
                            if (bytes_per_cyl > 0) {
                                ULONG size_mb    = parse_num((char *)si->Buffer);
                                UQUAD bytes_need = (UQUAD)size_mb * 1024ULL * 1024ULL;
                                ULONG cyls = (ULONG)((bytes_need + bytes_per_cyl - 1)
                                                     / bytes_per_cyl);
                                if (cyls == 0) cyls = 1;
                                pi->high_cyl = pi->low_cyl + cyls - 1;
                                if (pi->high_cyl > max_hi) pi->high_cyl = max_hi;
                            }
                        }
                        si = (struct StringInfo *)bootpri_gad->SpecialInfo;
                        pi->boot_pri = parse_long((char *)si->Buffer);
                        pi->flags = 0;
                        if (  boot_gad->Flags      & GFLG_SELECTED) pi->flags |= 1UL; /* PBFF_BOOTABLE */
                        if (!(automount_gad->Flags & GFLG_SELECTED)) pi->flags |= 2UL; /* PBFF_NOMOUNT */
                        if (  dirscsi_gad->Flags   & GFLG_SELECTED) pi->flags |= 4UL;
                        if (  syncscsi_gad->Flags  & GFLG_SELECTED) pi->flags |= 8UL;
                        pi->dos_type = new_dos_type;
                        if (destructive) need_reboot = TRUE;
                        result = TRUE; running = FALSE;
                        }
                        break;
                    }
                    case PDLG_CANCEL: running = FALSE; break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
        if (need_reboot) {
            struct EasyStruct es = {
                sizeof(struct EasyStruct), 0,
                DISKPART_VERTITLE,
                "Filesystem or block size changed.\n"
                "A reboot is required for this\n"
                "partition to be recognised correctly.",
                "OK"
            };
            EasyRequest(win, &es, NULL, TAG_DONE);
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
/* Filesystem manager dialog                                           */
/* ------------------------------------------------------------------ */

/* Gadget IDs for the filesystem manager window */
#define FSDLG_LIST    1
#define FSDLG_SEL     2   /* GTLV_ShowSelected target — display only */
#define FSDLG_ADD     3
#define FSDLG_EDIT    4
#define FSDLG_DELETE  5
#define FSDLG_DONE    6

/* Gadget IDs for the add-FS sub-dialog */
#define AFSDLG_DOSTYPE  1
#define AFSDLG_FILE     2
#define AFSDLG_BROWSE   3
#define AFSDLG_OK       4
#define AFSDLG_CANCEL   5
#define AFSDLG_HEXDISP  6

static char        fs_strs[MAX_FILESYSTEMS][64];
static struct Node fs_nodes[MAX_FILESYSTEMS];
static struct List fs_list_gad;

static void build_fs_list(const struct RDBInfo *rdb)
{
    UWORD i;
    fs_list_gad.lh_Head     = (struct Node *)&fs_list_gad.lh_Tail;
    fs_list_gad.lh_Tail     = NULL;
    fs_list_gad.lh_TailPred = (struct Node *)&fs_list_gad.lh_Head;

    if (!rdb || !rdb->valid) return;
    for (i = 0; i < rdb->num_fs; i++) {
        const struct FSInfo *fi = &rdb->filesystems[i];
        char dt[16], ver[12], codesz[16];

        FormatDosType(fi->dos_type, dt);

        if (fi->version)
            sprintf(ver, "%lu.%lu",
                    (unsigned long)(fi->version >> 16),
                    (unsigned long)(fi->version & 0xFFFFUL));
        else
            sprintf(ver, "----");

        if (fi->code && fi->code_size > 0)
            FormatSize((UQUAD)fi->code_size, codesz);
        else
            sprintf(codesz, "No code");

        sprintf(fs_strs[i], "%-12s  %-8s  %s", dt, ver, codesz);

        fs_nodes[i].ln_Name = fs_strs[i];
        fs_nodes[i].ln_Type = NT_USER;
        fs_nodes[i].ln_Pri  = 0;
        AddTail(&fs_list_gad, &fs_nodes[i]);
    }
}

/* Load a file path into *fi->code/code_size. Shows error on failure.
   Returns TRUE on success (or if path is empty = no-code entry).     */
static BOOL fs_load_file(struct Window *win, const char *path, struct FSInfo *fi)
{
    struct EasyStruct es;
    BPTR fh;
    LONG fsize;
    UBYTE *code;

    if (path[0] == '\0') return TRUE;   /* no file = register DosType only */

    fh = Open((UBYTE *)path, MODE_OLDFILE);
    if (!fh) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Error";
        es.es_TextFormat=(UBYTE*)"Cannot open file.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    if (fsize <= 0 || fsize >= (LONG)(1024L * 1024L)) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Error";
        es.es_TextFormat=(UBYTE*)"File is empty or too large\n(limit: 1 MB).";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    code = (UBYTE *)AllocVec((ULONG)fsize, MEMF_PUBLIC | MEMF_CLEAR);
    if (!code) { Close(fh); return FALSE; }
    if (Read(fh, code, fsize) != fsize) {
        Close(fh); FreeVec(code);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Error";
        es.es_TextFormat=(UBYTE*)"File read error.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    Close(fh);
    if (fi->code) FreeVec(fi->code);
    fi->code      = code;
    fi->code_size = (ULONG)fsize;
    /* Record the source path in fhb_FileSysName (e.g. "L:pfs3aio").
       Some boot ROMs and expansion firmwares use this field to identify
       or fall back to loading the handler.  Always preserve it. */
    {
        ULONG len = strlen(path);
        if (len > 83) len = 83;
        memcpy(fi->fs_name, path, len);
        fi->fs_name[len] = '\0';
    }
    /* Extract version from Resident struct (RT_MATCHWORD = 0x4AFC).
       AmigaOS convention: fhb_Version = (major << 16) | minor.
       Scan entire binary for RT_MATCHWORD and read RT_Version at +11. */
    {
        ULONG i;
        UBYTE major = 0, minor = 0;
        for (i = 0; i + 12 < (ULONG)fsize; i += 2) {
            if (code[i] == 0x4A && code[i+1] == 0xFC) {
                major = code[i + 11];
                /* look for $VER: string to get minor — fall back to 0 */
                {
                    ULONG j;
                    for (j = 0; j + 5 < (ULONG)fsize; j++) {
                        if (code[j]   == '$' && code[j+1] == 'V' &&
                            code[j+2] == 'E' && code[j+3] == 'R' &&
                            code[j+4] == ':') {
                            /* skip "$VER: name maj.min" — advance past ": " and name */
                            ULONG k = j + 5;
                            while (k < (ULONG)fsize && code[k] == ' ') k++;
                            /* skip name token */
                            while (k < (ULONG)fsize && code[k] != ' ' && code[k] != '\0') k++;
                            while (k < (ULONG)fsize && code[k] == ' ') k++;
                            /* skip major digits */
                            while (k < (ULONG)fsize && code[k] >= '0' && code[k] <= '9') k++;
                            if (k < (ULONG)fsize && code[k] == '.') {
                                k++;
                                minor = 0;
                                while (k < (ULONG)fsize && code[k] >= '0' && code[k] <= '9') {
                                    minor = (UBYTE)(minor * 10 + (code[k] - '0'));
                                    k++;
                                }
                            }
                            break;
                        }
                    }
                }
                break;
            }
        }
        fi->version = ((ULONG)major << 16) | (ULONG)minor;
    }
    return TRUE;
}

/* Opens a sub-dialog to enter/edit DosType + optional file path.
   is_edit=FALSE: Add (default drawer L:), is_edit=TRUE: Edit.
   Returns TRUE if user confirmed, filling in *fi.               */
static BOOL fs_addedit_dialog(struct FSInfo *fi, BOOL is_edit)
{
    struct Screen *scr         = NULL;
    APTR           vi          = NULL;
    struct Gadget *glist       = NULL;
    struct Gadget *gctx        = NULL;
    struct Gadget *dostype_gad = NULL;
    struct Gadget *file_gad    = NULL;
    struct Window *win         = NULL;
    struct Gadget *hex_gad     = NULL;
    BOOL           result      = FALSE;
    char           dt_str[20];
    char           hex_str[16];
    static char    file_str[256];   /* static so ASL path update persists */

    FormatDosType(fi->dos_type, dt_str);
    sprintf(hex_str, "0x%08lX", fi->dos_type);
    if (is_edit) {
        file_str[0] = '\0';   /* empty = keep existing code */
    } else {
        strncpy(file_str, "L:", sizeof(file_str) - 1);
        file_str[sizeof(file_str) - 1] = '\0';
    }

    scr = LockPubScreen(NULL);
    if (!scr) goto fs_add_cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto fs_add_cleanup;

    {
        UWORD font_h   = scr->Font->ta_YSize;
        UWORD bor_l    = (UWORD)scr->WBorLeft;
        UWORD bor_t    = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r    = (UWORD)scr->WBorRight;
        UWORD bor_b    = (UWORD)scr->WBorBottom;
        UWORD win_w    = 460;
        UWORD inner_w  = win_w - bor_l - bor_r;
        UWORD pad      = 3;
        UWORD row_h    = font_h + 4;
        UWORD lbl_w    = 90;
        UWORD browse_w = 70;
        UWORD gad_x    = bor_l + lbl_w;
        UWORD file_w   = inner_w - lbl_w - browse_w - pad * 2;
        UWORD dt_str_w = 110;                          /* DosType input: "DOS\1" or "0x444F5301" */
        UWORD dt_hex_w = inner_w - lbl_w - dt_str_w - pad * 2; /* hex readout to the right */
        UWORD gad_w    = inner_w - lbl_w - pad;
        UWORD win_h    = bor_t + pad + row_h + pad + row_h + pad + row_h + pad + bor_b;
        struct NewGadget ng;
        struct Gadget *prev;

        gctx = CreateContext(&glist);
        if (!gctx) goto fs_add_cleanup;

        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;

        /* DosType string — narrowed; accepts "DOS\1" or "0x444F5301" */
        ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=(WORD)(bor_t+pad);
        ng.ng_Width=dt_str_w; ng.ng_Height=row_h;
        ng.ng_GadgetText="DosType"; ng.ng_GadgetID=AFSDLG_DOSTYPE;
        ng.ng_Flags=PLACETEXT_LEFT;
        { struct TagItem st[]={{GTST_String,(ULONG)dt_str},{GTST_MaxChars,18},{TAG_DONE,0}};
          dostype_gad=CreateGadgetA(STRING_KIND,gctx,&ng,st);
          if (!dostype_gad) goto fs_add_cleanup; prev=dostype_gad; }

        /* Hex readout — read-only display to the right of DosType */
        ng.ng_LeftEdge=gad_x+dt_str_w+pad; ng.ng_TopEdge=(WORD)(bor_t+pad);
        ng.ng_Width=dt_hex_w; ng.ng_Height=row_h;
        ng.ng_GadgetText=NULL; ng.ng_GadgetID=AFSDLG_HEXDISP;
        ng.ng_Flags=0;
        { struct TagItem tt[]={{GTTX_Text,(ULONG)hex_str},{GTTX_Border,TRUE},{TAG_DONE,0}};
          hex_gad=CreateGadgetA(TEXT_KIND,prev,&ng,tt);
          if (!hex_gad) goto fs_add_cleanup; prev=hex_gad; }

        /* File string (narrower to leave room for Browse button) */
        ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=(WORD)(bor_t+pad+row_h+pad);
        ng.ng_Width=file_w; ng.ng_Height=row_h;
        ng.ng_GadgetText="File"; ng.ng_GadgetID=AFSDLG_FILE;
        ng.ng_Flags=PLACETEXT_LEFT;
        { struct TagItem st[]={{GTST_String,(ULONG)file_str},{GTST_MaxChars,255},{TAG_DONE,0}};
          file_gad=CreateGadgetA(STRING_KIND,prev,&ng,st);
          if (!file_gad) goto fs_add_cleanup; prev=file_gad; }

        /* Browse button — right of File string */
        ng.ng_LeftEdge=gad_x+(WORD)file_w+pad; ng.ng_TopEdge=(WORD)(bor_t+pad+row_h+pad);
        ng.ng_Width=browse_w; ng.ng_Height=row_h;
        ng.ng_GadgetText="Browse..."; ng.ng_GadgetID=AFSDLG_BROWSE;
        ng.ng_Flags=PLACETEXT_IN;
        { struct TagItem bt[]={{TAG_DONE,0}};
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
          if (!prev) goto fs_add_cleanup; }

        /* OK / Cancel */
        { UWORD btn_y = bor_t+pad+row_h+pad+row_h+pad;
          UWORD half  = (inner_w-pad*2-pad)/2;
          struct TagItem bt[]={{TAG_DONE,0}};
          ng.ng_TopEdge=btn_y; ng.ng_Height=row_h; ng.ng_Width=half;
          ng.ng_Flags=PLACETEXT_IN;
          ng.ng_LeftEdge=bor_l+pad; ng.ng_GadgetText="OK";
          ng.ng_GadgetID=AFSDLG_OK;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_add_cleanup;
          ng.ng_LeftEdge=bor_l+pad+half+pad;
          ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=AFSDLG_CANCEL;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_add_cleanup; }

        { struct TagItem wt[]={
              {WA_Left,(ULONG)((scr->Width-win_w)/2)},
              {WA_Top,(ULONG)((scr->Height-win_h)/2)},
              {WA_Width,win_w},{WA_Height,win_h},
              {WA_Title,(ULONG)(is_edit ? "Edit FileSystem Driver" : "Add FileSystem Driver")},
              {WA_Gadgets,(ULONG)glist},{WA_PubScreen,(ULONG)scr},
              {WA_IDCMP,IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_REFRESHWINDOW},
              {WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_CLOSEGADGET|
                        WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH},
              {TAG_DONE,0}};
          win=OpenWindowTagList(NULL,wt); }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto fs_add_cleanup;
    GT_RefreshWindow(win, NULL);
    ActivateGadget(dostype_gad, win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case AFSDLG_CANCEL: running = FALSE; break;

                    case AFSDLG_DOSTYPE: {
                        /* Update hex readout when user presses Return in DosType field */
                        struct StringInfo *si = (struct StringInfo *)dostype_gad->SpecialInfo;
                        ULONG dt = parse_dostype((char *)si->Buffer);
                        sprintf(hex_str, "0x%08lX", dt);
                        { struct TagItem tt[]={{GTTX_Text,(ULONG)hex_str},{TAG_DONE,0}};
                          GT_SetGadgetAttrsA(hex_gad, win, NULL, tt); }
                        break;
                    }

                    case AFSDLG_BROWSE:
                        if (AslBase) {
                            struct FileRequester *fr;
                            struct StringInfo *si = (struct StringInfo *)file_gad->SpecialInfo;
                            { struct TagItem asl_tags[] = {
                                  { ASLFR_TitleText,    (ULONG)"Select FileSystem Driver" },
                                  { ASLFR_InitialDrawer,(ULONG)"L:" },
                                  { ASLFR_InitialFile,  (ULONG)si->Buffer },
                                  { TAG_DONE, 0 } };
                              fr = (struct FileRequester *)AllocAslRequest(
                                  ASL_FileRequest, asl_tags); }
                            if (fr) {
                                if (AslRequest(fr, NULL)) {
                                    /* Build full path: drawer + file */
                                    strncpy(file_str, fr->fr_Drawer, sizeof(file_str)-1);
                                    file_str[sizeof(file_str)-1] = '\0';
                                    AddPart((UBYTE *)file_str, (UBYTE *)fr->fr_File,
                                            sizeof(file_str));
                                    /* Update the string gadget */
                                    { struct TagItem ut[]={{GTST_String,(ULONG)file_str},{TAG_DONE,0}};
                                      GT_SetGadgetAttrsA(file_gad, win, NULL, ut); }
                                }
                                FreeAslRequest(fr);
                            }
                        }
                        break;

                    case AFSDLG_OK: {
                        struct StringInfo *si;
                        si = (struct StringInfo *)dostype_gad->SpecialInfo;
                        fi->dos_type = parse_dostype((char *)si->Buffer);
                        si = (struct StringInfo *)file_gad->SpecialInfo;
                        if (fs_load_file(win, (char *)si->Buffer, fi))
                            result = TRUE;
                        if (result) running = FALSE;
                        break;
                    }
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

fs_add_cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return result;
}

/* Opens the filesystem manager window.
   Returns TRUE if any changes were made.                         */
static BOOL filesystem_manager_dialog(struct RDBInfo *rdb)
{
    struct Screen *scr    = NULL;
    APTR           vi     = NULL;
    struct Gadget *glist  = NULL;
    struct Gadget *gctx   = NULL;
    struct Gadget *lv_gad = NULL;
    struct Window *win    = NULL;
    BOOL           dirty  = FALSE;
    WORD           sel    = -1;

    build_fs_list(rdb);

    scr = LockPubScreen(NULL);
    if (!scr) goto fs_mgr_cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto fs_mgr_cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 480;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 4;
        UWORD btn_h   = font_h + 6;
        UWORD hdr_h   = font_h + 3;
        UWORD lv_h    = (UWORD)(font_h + 2) * 6;
        UWORD sel_h   = font_h + 4;   /* GTLV_ShowSelected string gadget */
        UWORD win_h   = bor_t + pad + hdr_h + lv_h + pad + sel_h + pad + btn_h + pad + bor_b;
        struct Gadget *sel_gad = NULL;
        struct NewGadget ng;
        struct Gadget *prev;

        gctx = CreateContext(&glist);
        if (!gctx) goto fs_mgr_cleanup;

        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;

        /* GTLV_ShowSelected target — must be created BEFORE the listview.
           With this set, GadTools gives the listview persistent selection
           highlighting and GADGETUP Code becomes the reliable item ordinal. */
        { UWORD sel_y = bor_t + pad + hdr_h + lv_h + pad;
          struct TagItem st[] = {{GTST_String,(ULONG)""},{GTST_MaxChars,63},{TAG_DONE,0}};
          ng.ng_LeftEdge  = bor_l + pad;
          ng.ng_TopEdge   = (WORD)sel_y;
          ng.ng_Width     = inner_w - pad * 2;
          ng.ng_Height    = sel_h;
          ng.ng_GadgetText= NULL;
          ng.ng_GadgetID  = FSDLG_SEL;
          ng.ng_Flags     = 0;
          sel_gad = CreateGadgetA(STRING_KIND, gctx, &ng, st);
          if (!sel_gad) goto fs_mgr_cleanup;
          prev = sel_gad; }

        /* Listview — GTLV_ShowSelected links it to sel_gad above */
        ng.ng_LeftEdge  = bor_l + pad;
        ng.ng_TopEdge   = (WORD)(bor_t + pad + hdr_h);
        ng.ng_Width     = inner_w - pad * 2;
        ng.ng_Height    = lv_h;
        ng.ng_GadgetText= NULL;
        ng.ng_GadgetID  = FSDLG_LIST;
        ng.ng_Flags     = 0;
        { struct TagItem lt[] = {{GTLV_Labels,    (ULONG)&fs_list_gad},
                                  {GTLV_ShowSelected,(ULONG)sel_gad},
                                  {TAG_DONE,0}};
          lv_gad = CreateGadgetA(LISTVIEW_KIND, prev, &ng, lt);
          if (!lv_gad) goto fs_mgr_cleanup;
          prev = lv_gad; }

        { UWORD btn_y    = bor_t + pad + hdr_h + lv_h + pad + sel_h + pad;
          UWORD quarter  = (inner_w - pad * 2 - pad * 3) / 4;
          struct TagItem bt[] = {{TAG_DONE,0}};
          ng.ng_TopEdge=btn_y; ng.ng_Height=btn_h;
          ng.ng_Width=quarter; ng.ng_Flags=PLACETEXT_IN;
          ng.ng_LeftEdge=bor_l+pad; ng.ng_GadgetText="Add";
          ng.ng_GadgetID=FSDLG_ADD;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup;
          ng.ng_LeftEdge=bor_l+pad+quarter+pad; ng.ng_GadgetText="Edit";
          ng.ng_GadgetID=FSDLG_EDIT;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup;
          ng.ng_LeftEdge=bor_l+pad+(quarter+pad)*2; ng.ng_GadgetText="Delete";
          ng.ng_GadgetID=FSDLG_DELETE;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup;
          ng.ng_LeftEdge=bor_l+pad+(quarter+pad)*3; ng.ng_GadgetText="Done";
          ng.ng_GadgetID=FSDLG_DONE;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup; }

        { struct TagItem wt[]={
              {WA_Left,(ULONG)((scr->Width-win_w)/2)},
              {WA_Top,(ULONG)((scr->Height-win_h)/2)},
              {WA_Width,win_w},{WA_Height,win_h},
              {WA_Title,(ULONG)"FileSystem Drivers"},
              {WA_Gadgets,(ULONG)glist},{WA_PubScreen,(ULONG)scr},
              {WA_IDCMP,IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                        IDCMP_GADGETDOWN|IDCMP_REFRESHWINDOW},
              {WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_CLOSEGADGET|
                        WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH},
              {TAG_DONE,0}};
          win=OpenWindowTagList(NULL,wt); }

        /* Column header drawn just above listview */
        if (win) {
            struct RastPort *rp = win->RPort;
            const char *hdr = "DosType       Version   Code";
            WORD fb = rp->TxBaseline;
            SetAPen(rp, 2); SetDrMd(rp, JAM2);
            RectFill(rp, bor_l+pad, bor_t+pad,
                     bor_l+pad+(WORD)(inner_w-pad*2)-1, (WORD)(bor_t+pad+hdr_h)-1);
            SetAPen(rp, 1); SetDrMd(rp, JAM1);
            Move(rp, bor_l+pad+4, (WORD)(bor_t+pad)+fb);
            Text(rp, hdr, strlen(hdr));
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto fs_mgr_cleanup;
    GT_RefreshWindow(win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code   = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETDOWN:
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case FSDLG_LIST:
                        /* With GTLV_ShowSelected set, Code is the reliable item ordinal */
                        sel = (WORD)code;
                        break;
                    case FSDLG_SEL:
                        break; /* show-selected string gadget — ignore */
                    case FSDLG_DONE: running = FALSE; break;

                    case FSDLG_ADD:
                        if (rdb->num_fs < MAX_FILESYSTEMS) {
                            struct FSInfo new_fi;
                            memset(&new_fi, 0, sizeof(new_fi));
                            new_fi.dos_type    = 0x444F5301UL;  /* FFS default */
                            new_fi.version     = 0;
                            new_fi.priority    = 0;
                            new_fi.global_vec  = -1L;
                            new_fi.patch_flags = 0x180UL; /* patch SegList + GlobalVec */
                            if (fs_addedit_dialog(&new_fi, FALSE)) {
                                rdb->filesystems[rdb->num_fs++] = new_fi;
                                dirty = TRUE;
                                { struct TagItem dt[]={{GTLV_Labels,TAG_IGNORE},{TAG_DONE,0}};
                                  struct TagItem rt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, dt);
                                  build_fs_list(rdb);
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, rt); }
                                sel = (WORD)(rdb->num_fs - 1);
                            }
                        }
                        break;

                    case FSDLG_EDIT:
                        if (sel >= 0 && sel < (WORD)rdb->num_fs) {
                            /* Work on a copy so Cancel leaves original intact.
                               The copy starts with the existing code pointer; if
                               the user loads a new file fs_addedit_dialog frees
                               the old allocation via FreeVec before setting a new
                               one, so we must NOT free it again on cancel. */
                            struct FSInfo edit_fi = rdb->filesystems[sel];
                            if (fs_addedit_dialog(&edit_fi, TRUE)) {
                                rdb->filesystems[sel] = edit_fi;
                                dirty = TRUE;
                                { struct TagItem dt[]={{GTLV_Labels,TAG_IGNORE},{TAG_DONE,0}};
                                  struct TagItem rt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, dt);
                                  build_fs_list(rdb);
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, rt); }
                            } else {
                                /* Cancelled: if fs_addedit_dialog loaded a new
                                   code buffer it stored it in edit_fi.code.
                                   Since we're not applying, free it if it differs
                                   from the original. */
                                if (edit_fi.code &&
                                    edit_fi.code != rdb->filesystems[sel].code)
                                    FreeVec(edit_fi.code);
                            }
                        }
                        break;

                    case FSDLG_DELETE:
                        if (sel >= 0 && sel < (WORD)rdb->num_fs) {
                            struct EasyStruct es;
                            char msg[256];
                            char dt[16];
                            char users[128];
                            ULONG del_dt = rdb->filesystems[sel].dos_type;
                            UWORD k, num_users = 0;
                            char *up = users;
                            users[0] = '\0';

                            /* Find which partitions use this filesystem */
                            for (k = 0; k < rdb->num_parts; k++) {
                                if (rdb->parts[k].dos_type == del_dt) {
                                    ULONG rem = (ULONG)(sizeof(users) - (ULONG)(up - users) - 1);
                                    if (num_users > 0 && rem > 2)
                                        { *up++=','; *up++=' '; *up='\0'; rem-=2; }
                                    if (rem > 1) {
                                        strncpy(up, rdb->parts[k].drive_name, rem);
                                        up[rem] = '\0';
                                        while (*up) up++;
                                    }
                                    num_users++;
                                }
                            }

                            FriendlyDosType(del_dt, dt);
                            if (num_users > 0) {
                                sprintf(msg,
                                    "Filesystem %s is in use by:\n"
                                    "%s\n\n"
                                    "Affected partition(s) will be\n"
                                    "changed to FFS. Delete anyway?",
                                    dt, users);
                            } else {
                                sprintf(msg, "Delete filesystem driver %s?", dt);
                            }

                            es.es_StructSize  = sizeof(es);
                            es.es_Flags       = 0;
                            es.es_Title       = (UBYTE*)"Delete FS Driver";
                            es.es_TextFormat  = (UBYTE*)msg;
                            es.es_GadgetFormat = (UBYTE*)"Yes|No";
                            if (EasyRequest(win, &es, NULL) == 1) {
                                UWORD j;
                                /* Reset affected partitions to FFS */
                                for (k = 0; k < rdb->num_parts; k++) {
                                    if (rdb->parts[k].dos_type == del_dt)
                                        rdb->parts[k].dos_type = 0x444F5301UL;
                                }
                                /* Remove filesystem entry */
                                if (rdb->filesystems[sel].code)
                                    FreeVec(rdb->filesystems[sel].code);
                                for (j=(UWORD)sel; j+1 < rdb->num_fs; j++)
                                    rdb->filesystems[j] = rdb->filesystems[j+1];
                                rdb->num_fs--;
                                dirty = TRUE;
                                if (sel >= (WORD)rdb->num_fs)
                                    sel = (WORD)rdb->num_fs - 1;
                                { struct TagItem dt2[]={{GTLV_Labels,TAG_IGNORE},{TAG_DONE,0}};
                                  struct TagItem rt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, dt2);
                                  build_fs_list(rdb);
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, rt); }
                            }
                        }
                        break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

fs_mgr_cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return dirty;
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
    UWORD seventh;

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
    seventh = (inner_w - pad * 2 - pad * 6) / 7;
    ng.ng_TopEdge = (WORD)btn_y;
    ng.ng_Height  = btn_h;
    ng.ng_Width   = seventh;

#define MKBTN(lx,txt,gid) \
    ng.ng_LeftEdge=(lx); ng.ng_GadgetText=(txt); ng.ng_GadgetID=(gid); \
    prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); \
    if (!prev) { FreeGadgets(glist); return FALSE; }

    MKBTN(bor_l+pad,                      "Init RDB", GID_INITRDB)
    MKBTN(bor_l+pad+(seventh+pad)*1,      "Add",      GID_ADD)
    MKBTN(bor_l+pad+(seventh+pad)*2,      "Edit",     GID_EDIT)
    MKBTN(bor_l+pad+(seventh+pad)*3,      "Delete",   GID_DELETE)
    MKBTN(bor_l+pad+(seventh+pad)*4,      "FileSys",  GID_FILESYS)
    MKBTN(bor_l+pad+(seventh+pad)*5,      "Write",    GID_WRITE)
    MKBTN(bor_l+pad+(seventh+pad)*6,      "Back",     GID_BACK)
#undef MKBTN

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

static void rdb_backup_block(struct Window *win, struct BlockDev *bd,
                              struct RDBInfo *rdb)
{
    struct EasyStruct es;
    UBYTE *buf;
    static char save_path[256];

    if (!rdb->valid) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Backup RDB Block";
        es.es_TextFormat=(UBYTE*)"No RDB found on this disk.\nNothing to backup.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }
    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Backup RDB Block";
        es.es_TextFormat=(UBYTE*)"Device is not accessible.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    if (!BlockDev_ReadBlock(bd, rdb->block_num, buf)) {
        FreeVec(buf);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Backup RDB Block";
        es.es_TextFormat=(UBYTE*)"Failed to read RDB block from disk.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    if (!AslBase) {
        FreeVec(buf);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Backup RDB Block";
        es.es_TextFormat=(UBYTE*)"asl.library not available.\nCannot open file requester.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    {
        struct FileRequester *fr;
        BOOL chosen = FALSE;
        { struct TagItem asl_tags[] = {
              { ASLFR_TitleText,    (ULONG)"Save RDB Block Backup" },
              { ASLFR_DoSaveMode,   TRUE },
              { ASLFR_InitialDrawer,(ULONG)"RAM:" },
              { ASLFR_InitialFile,  (ULONG)"RDB.backup" },
              { TAG_DONE, 0 } };
          fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, asl_tags); }
        if (fr) {
            if (AslRequest(fr, NULL)) {
                strncpy(save_path, fr->fr_Drawer, sizeof(save_path)-1);
                save_path[sizeof(save_path)-1] = '\0';
                AddPart((UBYTE *)save_path, (UBYTE *)fr->fr_File, sizeof(save_path));
                chosen = TRUE;
            }
            FreeAslRequest(fr);
        }
        if (!chosen) { FreeVec(buf); return; }
    }

    {
        BPTR fh = Open((UBYTE *)save_path, MODE_NEWFILE);
        if (!fh) {
            es.es_StructSize=sizeof(es); es.es_Flags=0;
            es.es_Title=(UBYTE*)"Backup RDB Block";
            es.es_TextFormat=(UBYTE*)"Cannot create backup file.";
            es.es_GadgetFormat=(UBYTE*)"OK";
            EasyRequest(win, &es, NULL);
        } else {
            Write(fh, buf, (LONG)bd->block_size);
            Close(fh);
            es.es_StructSize=sizeof(es); es.es_Flags=0;
            es.es_Title=(UBYTE*)"Backup RDB Block";
            es.es_TextFormat=(UBYTE*)"RDB block saved successfully.";
            es.es_GadgetFormat=(UBYTE*)"OK";
            EasyRequest(win, &es, NULL);
        }
    }
    FreeVec(buf);
}

static void rdb_restore_block(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct es;
    UBYTE *buf;
    static char load_path[256];
    BPTR fh;
    LONG fsize;

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"Device is not accessible.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    /* Prominent warning before doing anything else */
    es.es_StructSize=sizeof(es); es.es_Flags=0;
    es.es_Title=(UBYTE*)"Restore RDB Block - WARNING";
    es.es_TextFormat=(UBYTE*)
        "WARNING: This will OVERWRITE block 0\n"
        "on the disk with data from the backup file!\n\n"
        "Restoring an incorrect or mismatched backup\n"
        "WILL cause permanent data loss.\n\n"
        "Ensure the backup matches THIS disk.\n\n"
        "Are you absolutely sure?";
    es.es_GadgetFormat=(UBYTE*)"Yes, restore|Cancel";
    if (EasyRequest(win, &es, NULL) != 1) return;

    if (!AslBase) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"asl.library not available.\nCannot open file requester.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    {
        struct FileRequester *fr;
        BOOL chosen = FALSE;
        { struct TagItem asl_tags[] = {
              { ASLFR_TitleText,    (ULONG)"Select RDB Block Backup File" },
              { ASLFR_InitialDrawer,(ULONG)"RAM:" },
              { ASLFR_InitialFile,  (ULONG)"RDB.backup" },
              { TAG_DONE, 0 } };
          fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, asl_tags); }
        if (fr) {
            if (AslRequest(fr, NULL)) {
                strncpy(load_path, fr->fr_Drawer, sizeof(load_path)-1);
                load_path[sizeof(load_path)-1] = '\0';
                AddPart((UBYTE *)load_path, (UBYTE *)fr->fr_File, sizeof(load_path));
                chosen = TRUE;
            }
            FreeAslRequest(fr);
        }
        if (!chosen) return;
    }

    fh = Open((UBYTE *)load_path, MODE_OLDFILE);
    if (!fh) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"Cannot open backup file.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize != (LONG)bd->block_size) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"Backup file size does not match\nthe device block size. Aborted.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { Close(fh); return; }

    if (Read(fh, buf, fsize) != fsize) {
        Close(fh); FreeVec(buf);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"File read error.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }
    Close(fh);

    /* Second confirmation — shown after the file is chosen, names the device */
    { char msg[160];
      sprintf(msg,
          "LAST CHANCE\n\n"
          "Write backup to block 0 of\n"
          "%s unit %lu ?\n\n"
          "This CANNOT be undone.",
          bd->devname, (unsigned long)bd->unit);
      es.es_StructSize=sizeof(es); es.es_Flags=0;
      es.es_Title=(UBYTE*)"Restore RDB Block - FINAL WARNING";
      es.es_TextFormat=(UBYTE*)msg;
      es.es_GadgetFormat=(UBYTE*)"Write it|Cancel";
      if (EasyRequest(win, &es, NULL) != 1) { FreeVec(buf); return; } }

    if (!BlockDev_WriteBlock(bd, 0, buf)) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"Failed to write block to disk.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
    } else {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Restore RDB Block";
        es.es_TextFormat=(UBYTE*)"RDB block restored to block 0.\nPlease reboot for changes to take effect.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
    }
    FreeVec(buf);
}

/* ------------------------------------------------------------------ */
/* Extended Backup / Restore — all blocks rdb_block_lo..HighRDSKBlock */
/* File format: 32-byte header + raw blocks                           */
/*   hdr[0] = 'ERDB' magic                                            */
/*   hdr[1] = version 1                                               */
/*   hdr[2] = block_lo                                                */
/*   hdr[3] = block_size                                              */
/*   hdr[4] = num_blocks                                              */
/*   hdr[5..7] = reserved 0                                           */
/* ------------------------------------------------------------------ */

#define ERDB_MAGIC   0x45524442UL   /* 'ERDB' */
#define ERDB_VERSION 1UL
#define ERDB_HDR_SZ  32             /* 8 longwords */

static void rdb_backup_extended(struct Window *win, struct BlockDev *bd,
                                  struct RDBInfo *rdb)
{
    struct EasyStruct es;
    UBYTE *buf;
    static char save_path[256];
    ULONG block_lo, block_hi, num_blocks, blk;
    ULONG hdr[8];

    if (!rdb->valid) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Backup";
        es.es_TextFormat=(UBYTE*)"No RDB found on this disk.\nNothing to backup.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }
    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Backup";
        es.es_TextFormat=(UBYTE*)"Device is not accessible.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    /* Read RDSK block to get rdb_HighRDSKBlock */
    if (!BlockDev_ReadBlock(bd, rdb->block_num, buf)) {
        FreeVec(buf);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Backup";
        es.es_TextFormat=(UBYTE*)"Failed to read RDB block from disk.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }
    { struct RigidDiskBlock *r = (struct RigidDiskBlock *)buf;
      block_lo = rdb->rdb_block_lo;
      block_hi = r->rdb_HighRDSKBlock;
      if (block_hi == RDB_END_MARK || block_hi < block_lo)
          block_hi = block_lo;
    }
    num_blocks = block_hi - block_lo + 1;

    if (!AslBase) {
        FreeVec(buf);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Backup";
        es.es_TextFormat=(UBYTE*)"asl.library not available.\nCannot open file requester.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    /* Build suggested filename from disk product name.
       Spaces → '_', non-alphanumeric/dash → '_', trailing '_' trimmed.
       Falls back to "disk" if the product string is empty. */
    { char init_file[64];
      { char name[32]; char *d = name; UWORD ci;
        for (ci = 0; ci < 16 && rdb->disk_product[ci]; ci++) {
            char c = rdb->disk_product[ci];
            if (c == ' ') *d++ = '_';
            else if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')
                *d++ = c;
            else *d++ = '_';
        }
        /* Trim trailing underscores */
        while (d > name && *(d-1) == '_') d--;
        *d = '\0';
        if (name[0]) sprintf(init_file, "RDB_extended_%s.backup", name);
        else         sprintf(init_file, "RDB_extended_disk.backup");
      }

    { struct FileRequester *fr; BOOL chosen = FALSE;
      { struct TagItem at[] = {
            { ASLFR_TitleText,    (ULONG)"Save Extended RDB Backup" },
            { ASLFR_DoSaveMode,   TRUE },
            { ASLFR_InitialDrawer,(ULONG)"RAM:" },
            { ASLFR_InitialFile,  (ULONG)init_file },
            { TAG_DONE, 0 } };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at); }
      if (fr) {
          if (AslRequest(fr, NULL)) {
              strncpy(save_path, fr->fr_Drawer, sizeof(save_path)-1);
              save_path[sizeof(save_path)-1] = '\0';
              AddPart((UBYTE *)save_path, (UBYTE *)fr->fr_File, sizeof(save_path));
              chosen = TRUE;
          }
          FreeAslRequest(fr);
      }
      if (!chosen) { FreeVec(buf); return; }
    }} /* end FileRequester + init_file blocks */

    { BPTR fh = Open((UBYTE *)save_path, MODE_NEWFILE);
      if (!fh) {
          FreeVec(buf);
          es.es_StructSize=sizeof(es); es.es_Flags=0;
          es.es_Title=(UBYTE*)"Extended Backup";
          es.es_TextFormat=(UBYTE*)"Cannot create backup file.";
          es.es_GadgetFormat=(UBYTE*)"OK";
          EasyRequest(win, &es, NULL); return;
      }

      hdr[0]=ERDB_MAGIC; hdr[1]=ERDB_VERSION;
      hdr[2]=block_lo;   hdr[3]=bd->block_size;
      hdr[4]=num_blocks; hdr[5]=hdr[6]=hdr[7]=0;

      if (Write(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ) {
          Close(fh); FreeVec(buf);
          es.es_StructSize=sizeof(es); es.es_Flags=0;
          es.es_Title=(UBYTE*)"Extended Backup";
          es.es_TextFormat=(UBYTE*)"Write error saving header.";
          es.es_GadgetFormat=(UBYTE*)"OK";
          EasyRequest(win, &es, NULL); return;
      }

      for (blk = block_lo; blk <= block_hi; blk++) {
          if (!BlockDev_ReadBlock(bd, blk, buf)) {
              ULONG k; for (k = 0; k < bd->block_size; k++) buf[k] = 0;
          }
          if (Write(fh, buf, (LONG)bd->block_size) != (LONG)bd->block_size) {
              Close(fh); FreeVec(buf);
              es.es_StructSize=sizeof(es); es.es_Flags=0;
              es.es_Title=(UBYTE*)"Extended Backup";
              es.es_TextFormat=(UBYTE*)"Write error saving block data.";
              es.es_GadgetFormat=(UBYTE*)"OK";
              EasyRequest(win, &es, NULL); return;
          }
      }
      Close(fh);
      { char msg[96];
        sprintf(msg, "Extended RDB backup saved.\n%lu blocks (blocks %lu\x96%lu).",
                (unsigned long)num_blocks,
                (unsigned long)block_lo,
                (unsigned long)block_hi);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Backup";
        es.es_TextFormat=(UBYTE*)msg;
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); }
    }
    FreeVec(buf);
}

static void rdb_restore_extended(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct es;
    UBYTE *buf;
    static char load_path[256];
    BPTR   fh;
    LONG   fsize;
    ULONG  hdr[8];
    ULONG  block_lo, block_size, num_blocks, blk;

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"Device is not accessible.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    es.es_StructSize=sizeof(es); es.es_Flags=0;
    es.es_Title=(UBYTE*)"Extended Restore - WARNING";
    es.es_TextFormat=(UBYTE*)
        "WARNING: This will OVERWRITE multiple blocks\n"
        "(RDB, partitions, filesystems) on the disk!\n\n"
        "An incorrect backup WILL destroy the disk layout.\n"
        "Ensure the backup matches THIS disk.\n\n"
        "Are you absolutely sure?";
    es.es_GadgetFormat=(UBYTE*)"Yes, restore|Cancel";
    if (EasyRequest(win, &es, NULL) != 1) return;

    if (!AslBase) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"asl.library not available.\nCannot open file requester.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    { struct FileRequester *fr; BOOL chosen = FALSE;
      { struct TagItem at[] = {
            { ASLFR_TitleText,    (ULONG)"Select Extended RDB Backup File" },
            { ASLFR_InitialDrawer,(ULONG)"RAM:" },
            { ASLFR_InitialFile,  (ULONG)"RDB_extended.backup" },
            { TAG_DONE, 0 } };
        fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, at); }
      if (fr) {
          if (AslRequest(fr, NULL)) {
              strncpy(load_path, fr->fr_Drawer, sizeof(load_path)-1);
              load_path[sizeof(load_path)-1] = '\0';
              AddPart((UBYTE *)load_path, (UBYTE *)fr->fr_File, sizeof(load_path));
              chosen = TRUE;
          }
          FreeAslRequest(fr);
      }
      if (!chosen) return;
    }

    fh = Open((UBYTE *)load_path, MODE_OLDFILE);
    if (!fh) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"Cannot open backup file.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    /* Get file size (Seek to end returns old pos; seek back returns end pos = size) */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize < ERDB_HDR_SZ) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"File too small — not a valid extended backup.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    if (Read(fh, hdr, ERDB_HDR_SZ) != ERDB_HDR_SZ ||
        hdr[0] != ERDB_MAGIC || hdr[1] != ERDB_VERSION) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"Not a valid extended RDB backup.\n(Bad magic or unsupported version.)";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }
    block_lo   = hdr[2];
    block_size = hdr[3];
    num_blocks = hdr[4];

    if (block_size != bd->block_size) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"Block size mismatch between backup\nand this device. Aborted.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }
    if (fsize != (LONG)(ERDB_HDR_SZ + num_blocks * block_size)) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Extended Restore";
        es.es_TextFormat=(UBYTE*)"File size does not match header.\nBackup may be corrupt. Aborted.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL); return;
    }

    /* Final confirmation */
    { char msg[192];
      sprintf(msg,
          "LAST CHANCE\n\n"
          "Write %lu blocks (blocks %lu\x96%lu) to\n"
          "%s unit %lu ?\n\n"
          "This CANNOT be undone.",
          (unsigned long)num_blocks,
          (unsigned long)block_lo,
          (unsigned long)(block_lo + num_blocks - 1),
          bd->devname, (unsigned long)bd->unit);
      es.es_StructSize=sizeof(es); es.es_Flags=0;
      es.es_Title=(UBYTE*)"Extended Restore - FINAL WARNING";
      es.es_TextFormat=(UBYTE*)msg;
      es.es_GadgetFormat=(UBYTE*)"Write it|Cancel";
      if (EasyRequest(win, &es, NULL) != 1) { Close(fh); return; }
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { Close(fh); return; }

    for (blk = 0; blk < num_blocks; blk++) {
        if (Read(fh, buf, (LONG)block_size) != (LONG)block_size) {
            Close(fh); FreeVec(buf);
            es.es_StructSize=sizeof(es); es.es_Flags=0;
            es.es_Title=(UBYTE*)"Extended Restore";
            es.es_TextFormat=(UBYTE*)"Read error on backup file.";
            es.es_GadgetFormat=(UBYTE*)"OK";
            EasyRequest(win, &es, NULL); return;
        }
        if (!BlockDev_WriteBlock(bd, block_lo + blk, buf)) {
            Close(fh); FreeVec(buf);
            { char msg[80];
              sprintf(msg, "Write failed on block %lu.", (unsigned long)(block_lo + blk));
              es.es_StructSize=sizeof(es); es.es_Flags=0;
              es.es_Title=(UBYTE*)"Extended Restore";
              es.es_TextFormat=(UBYTE*)msg;
              es.es_GadgetFormat=(UBYTE*)"OK";
              EasyRequest(win, &es, NULL); }
            return;
        }
    }
    Close(fh); FreeVec(buf);
    es.es_StructSize=sizeof(es); es.es_Flags=0;
    es.es_Title=(UBYTE*)"Extended Restore";
    es.es_TextFormat=(UBYTE*)"Extended RDB restored successfully.\nPlease reboot for changes to take effect.";
    es.es_GadgetFormat=(UBYTE*)"OK";
    EasyRequest(win, &es, NULL);
}

/* ------------------------------------------------------------------ */
/* View RDB Block — read-only display of all RDB fields               */
/* ------------------------------------------------------------------ */

#define VRDB_LIST     1
#define VRDB_DONE     2
#define VRDB_MAXLINES 120

static char        vrdb_strs[VRDB_MAXLINES][80];
static struct Node vrdb_nodes[VRDB_MAXLINES];
static struct List vrdb_list;
static UWORD       vrdb_count;

static void vrdb_add(const char *s)
{
    if (vrdb_count >= VRDB_MAXLINES) return;
    strncpy(vrdb_strs[vrdb_count], s, 79);
    vrdb_strs[vrdb_count][79] = '\0';
    vrdb_nodes[vrdb_count].ln_Name = vrdb_strs[vrdb_count];
    vrdb_nodes[vrdb_count].ln_Type = NT_USER;
    vrdb_nodes[vrdb_count].ln_Pri  = 0;
    AddTail(&vrdb_list, &vrdb_nodes[vrdb_count]);
    vrdb_count++;
}

/* Copy a fixed-length, possibly space-padded, possibly non-null-terminated
   SCSI-style string into dst (null-terminated).  Non-printable chars → '.'.
   Returns dst. */
static char *vrdb_str(const char *src, UWORD srclen, char *dst, UWORD dstsize)
{
    UWORD i, end = 0;
    for (i = 0; i < srclen; i++) {
        if (src[i] == '\0') break;
        if (src[i] != ' ') end = i + 1;
    }
    for (i = 0; i < end && i < dstsize - 1; i++)
        dst[i] = (src[i] >= 0x20 && src[i] <= 0x7E) ? src[i] : '.';
    dst[i] = '\0';
    if (i == 0) { dst[0] = '-'; dst[1] = '\0'; }
    return dst;
}

static BOOL vrdb_make_gadgets(APTR vi, struct Screen *scr,
                               UWORD win_w, UWORD win_h,
                               struct Gadget **glist_out)
{
    UWORD bor_l   = (UWORD)scr->WBorLeft;
    UWORD bor_t   = (UWORD)scr->WBorTop + (UWORD)scr->Font->ta_YSize + 1;
    UWORD bor_r   = (UWORD)scr->WBorRight;
    UWORD bor_b   = (UWORD)scr->WBorBottom;
    UWORD pad     = 4;
    UWORD row_h   = (UWORD)scr->Font->ta_YSize + 2;
    UWORD btn_h   = (UWORD)scr->Font->ta_YSize + 6;
    UWORD inner_w = win_w - bor_l - bor_r;
    UWORD overhead = bor_t + pad*3 + btn_h + bor_b;
    UWORD lv_h    = (win_h > overhead + row_h) ? (win_h - overhead) : row_h;
    struct NewGadget ng;
    struct Gadget *gctx, *glist = NULL, *prev;

    *glist_out = NULL;
    gctx = CreateContext(&glist);
    if (!gctx) return FALSE;

    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr   = scr->Font;
    ng.ng_LeftEdge   = bor_l + pad;
    ng.ng_TopEdge    = (WORD)(bor_t + pad);
    ng.ng_Width      = inner_w - pad * 2;
    ng.ng_Height     = lv_h;
    ng.ng_GadgetID   = VRDB_LIST;
    ng.ng_Flags      = 0;
    { struct TagItem lt[] = { { GTLV_Labels,(ULONG)&vrdb_list }, { TAG_DONE,0 } };
      prev = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lt);
      if (!prev) { FreeGadgets(glist); return FALSE; } }

    { struct TagItem bt[] = { { TAG_DONE, 0 } };
      ng.ng_TopEdge    = (WORD)(bor_t + pad + lv_h + pad);
      ng.ng_Height     = btn_h;
      ng.ng_Width      = inner_w - pad * 2;
      ng.ng_LeftEdge   = bor_l + pad;
      ng.ng_GadgetText = "Close";
      ng.ng_GadgetID   = VRDB_DONE;
      ng.ng_Flags      = PLACETEXT_IN;
      prev = CreateGadgetA(BUTTON_KIND, prev, &ng, bt);
      if (!prev) { FreeGadgets(glist); return FALSE; } }

    *glist_out = glist;
    return TRUE;
}

static void rdb_view_block(struct Window *win, struct BlockDev *bd,
                            struct RDBInfo *rdb)
{
    struct EasyStruct es;
    UBYTE  *buf = NULL;
    struct RigidDiskBlock *r;

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"View RDB Block";
        es.es_TextFormat=(UBYTE*)"Device is not accessible.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    /* If no valid RDB was found, read block 0 and show it anyway.
       The field display will be garbage, but the checksum and identity
       lines will indicate what is actually on disk. */
    { ULONG read_blk = rdb->valid ? rdb->block_num : 0;
      if (!BlockDev_ReadBlock(bd, read_blk, buf)) {
          FreeVec(buf);
          es.es_StructSize=sizeof(es); es.es_Flags=0;
          es.es_Title=(UBYTE*)"View RDB Block";
          es.es_TextFormat=(UBYTE*)"Failed to read block from disk.";
          es.es_GadgetFormat=(UBYTE*)"OK";
          EasyRequest(win, &es, NULL);
          return;
      }
    }

    r = (struct RigidDiskBlock *)buf;

    /* ---- Verify checksum ---- */
    {
        ULONG sum = 0, n = r->rdb_SummedLongs, i;
        ULONG *lw = (ULONG *)buf;
        if (n > bd->block_size / 4) n = bd->block_size / 4;
        for (i = 0; i < n; i++) sum += lw[i];

        /* ---- Build display lines ---- */
        vrdb_count = 0;
        vrdb_list.lh_Head     = (struct Node *)&vrdb_list.lh_Tail;
        vrdb_list.lh_Tail     = NULL;
        vrdb_list.lh_TailPred = (struct Node *)&vrdb_list.lh_Head;

        if (!rdb->valid)
            vrdb_add("*** No valid RDB found — showing raw block 0 ***");

        /* --- Installed Filesystems (from parsed RDB) --- */
        { char b[80]; UWORD fi;
          sprintf(b, "--- Installed Filesystems (%u) -----------------------", (unsigned)rdb->num_fs);
          vrdb_add(b);
          if (rdb->num_fs == 0) {
              vrdb_add("  (none)");
          } else {
              for (fi = 0; fi < rdb->num_fs; fi++) {
                  const struct FSInfo *fs = &rdb->filesystems[fi];
                  char dt[16], ver[12], sz[16];
                  FormatDosType(fs->dos_type, dt);
                  if (fs->version)
                      sprintf(ver, "v%lu.%lu",
                              (unsigned long)(fs->version >> 16),
                              (unsigned long)(fs->version & 0xFFFFUL));
                  else
                      sprintf(ver, "v?.?");
                  if (fs->code && fs->code_size > 0)
                      FormatSize((UQUAD)fs->code_size, sz);
                  else
                      sprintf(sz, "no code");
                  sprintf(b, "  %-12s  %-8s  %s  (blk %lu)",
                          dt, ver, sz, (unsigned long)fs->block_num);
                  vrdb_add(b);
              }
          }
        }

        /* --- Identity --- */
        vrdb_add("--- Identity -----------------------------------------");
        { char b[80]; sprintf(b, "  Block number  : %lu", (unsigned long)rdb->block_num); vrdb_add(b); }
        { char id[8], b[80];
          id[0]=(char)((r->rdb_ID>>24)&0xFF); id[1]=(char)((r->rdb_ID>>16)&0xFF);
          id[2]=(char)((r->rdb_ID>> 8)&0xFF); id[3]=(char)( r->rdb_ID     &0xFF);
          id[4]='\0';
          for (i=0;i<4;i++) if (id[i]<0x20||id[i]>0x7E) id[i]='.';
          sprintf(b, "  ID            : %s  (0x%08lX)", id, (unsigned long)r->rdb_ID);
          vrdb_add(b); }
        { char b[80]; sprintf(b, "  SummedLongs   : %lu  (%lu bytes covered)",
              (unsigned long)r->rdb_SummedLongs,
              (unsigned long)(r->rdb_SummedLongs * 4)); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Checksum      : 0x%08lX  [%s]",
              (unsigned long)(ULONG)r->rdb_ChkSum,
              (sum == 0) ? "VALID" : "INVALID"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  HostID        : %lu", (unsigned long)r->rdb_HostID); vrdb_add(b); }
        { char b[80]; sprintf(b, "  BlockBytes    : %lu", (unsigned long)r->rdb_BlockBytes); vrdb_add(b); }

        /* --- Flags --- */
        { char b[80]; sprintf(b, "--- Flags: 0x%08lX ---------------------------", (unsigned long)r->rdb_Flags); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit0 LAST      : %s", (r->rdb_Flags & RDBFF_LAST)      ? "SET  (no more disks after this)"       : "not set"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit1 LASTLUN   : %s", (r->rdb_Flags & RDBFF_LASTLUN)   ? "SET  (no more LUNs at this target)"    : "not set"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit2 LASTTID   : %s", (r->rdb_Flags & RDBFF_LASTTID)   ? "SET  (no more target IDs on this bus)" : "not set"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit3 NORESELECT: %s", (r->rdb_Flags & RDBFF_NORESELECT) ? "SET  (no reselect)"                   : "not set"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit4 DISKID    : %s", (r->rdb_Flags & RDBFF_DISKID)     ? "SET  (disk identification valid)"     : "not set (disk ID fields may be garbage)"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit5 CTRLRID   : %s", (r->rdb_Flags & RDBFF_CTRLRID)    ? "SET  (controller ID valid)"           : "not set (ctrl ID fields may be garbage)"); vrdb_add(b); }
        { char b[80]; sprintf(b, "  Bit6 SYNCH     : %s", (r->rdb_Flags & RDBFF_SYNCH)      ? "SET  (SCSI synchronous mode)"         : "not set"); vrdb_add(b); }

        /* --- Block List Heads --- */
        vrdb_add("--- Block List Heads ---------------------------------");
        { char v[32], b[80];
          if (r->rdb_BadBlockList == RDB_END_MARK) sprintf(v,"none");
          else sprintf(v,"%lu",(unsigned long)r->rdb_BadBlockList);
          sprintf(b,"  BadBlockList   : %s  (0x%08lX)", v, (unsigned long)r->rdb_BadBlockList); vrdb_add(b); }
        { char v[32], b[80];
          if (r->rdb_PartitionList == RDB_END_MARK) sprintf(v,"none");
          else sprintf(v,"%lu",(unsigned long)r->rdb_PartitionList);
          sprintf(b,"  PartitionList  : %s  (0x%08lX)", v, (unsigned long)r->rdb_PartitionList); vrdb_add(b); }
        { char v[32], b[80];
          if (r->rdb_FileSysHeaderList == RDB_END_MARK) sprintf(v,"none");
          else sprintf(v,"%lu",(unsigned long)r->rdb_FileSysHeaderList);
          sprintf(b,"  FileSysHdrList : %s  (0x%08lX)", v, (unsigned long)r->rdb_FileSysHeaderList); vrdb_add(b); }
        { char v[32], b[80];
          if (r->rdb_DriveInit == RDB_END_MARK) sprintf(v,"none");
          else sprintf(v,"%lu",(unsigned long)r->rdb_DriveInit);
          sprintf(b,"  DriveInit      : %s  (0x%08lX)", v, (unsigned long)r->rdb_DriveInit); vrdb_add(b); }

        /* --- Physical Drive --- */
        vrdb_add("--- Physical Drive Characteristics -------------------");
        { char b[80]; sprintf(b, "  Cylinders      : %lu", (unsigned long)r->rdb_Cylinders);   vrdb_add(b); }
        { char b[80]; sprintf(b, "  Sectors/track  : %lu", (unsigned long)r->rdb_Sectors);     vrdb_add(b); }
        { char b[80]; sprintf(b, "  Heads          : %lu", (unsigned long)r->rdb_Heads);       vrdb_add(b); }
        { char b[80]; sprintf(b, "  Interleave     : %lu", (unsigned long)r->rdb_Interleave);  vrdb_add(b); }
        { char b[80]; sprintf(b, "  Park cylinder  : %lu", (unsigned long)r->rdb_Park);        vrdb_add(b); }
        { char v[32], b[80];
          if (r->rdb_WritePreComp == RDB_END_MARK) sprintf(v,"not used");
          else sprintf(v,"%lu",(unsigned long)r->rdb_WritePreComp);
          sprintf(b,"  WritePreComp   : %s", v); vrdb_add(b); }
        { char v[32], b[80];
          if (r->rdb_ReducedWrite == RDB_END_MARK) sprintf(v,"not used");
          else sprintf(v,"%lu",(unsigned long)r->rdb_ReducedWrite);
          sprintf(b,"  ReducedWrite   : %s", v); vrdb_add(b); }
        { char b[80]; sprintf(b, "  StepRate       : %lu", (unsigned long)r->rdb_StepRate);    vrdb_add(b); }

        /* --- Logical Drive --- */
        vrdb_add("--- Logical Drive Characteristics --------------------");
        { char b[80]; sprintf(b, "  RDBBlocksLo    : %lu", (unsigned long)r->rdb_RDBBlocksLo);  vrdb_add(b); }
        { char b[80]; sprintf(b, "  RDBBlocksHi    : %lu", (unsigned long)r->rdb_RDBBlocksHi);  vrdb_add(b); }
        { char b[80]; sprintf(b, "  LoCylinder     : %lu", (unsigned long)r->rdb_LoCylinder);   vrdb_add(b); }
        { char b[80]; sprintf(b, "  HiCylinder     : %lu", (unsigned long)r->rdb_HiCylinder);   vrdb_add(b); }
        { char b[80]; sprintf(b, "  CylBlocks      : %lu  (%lu sectors x %lu heads)",
              (unsigned long)r->rdb_CylBlocks,
              (unsigned long)r->rdb_Sectors,
              (unsigned long)r->rdb_Heads); vrdb_add(b); }
        { char b[80]; sprintf(b, "  AutoParkSecs   : %lu", (unsigned long)r->rdb_AutoParkSeconds); vrdb_add(b); }
        { char b[80]; sprintf(b, "  HighRDSKBlock  : %lu", (unsigned long)r->rdb_HighRDSKBlock); vrdb_add(b); }

        /* --- Drive Identification --- */
        vrdb_add("--- Drive Identification -----------------------------");
        { char s[20], b[80]; vrdb_str(r->rdb_DiskVendor,        8, s, sizeof(s)); sprintf(b,"  Disk Vendor    : %s", s); vrdb_add(b); }
        { char s[20], b[80]; vrdb_str(r->rdb_DiskProduct,       16, s, sizeof(s)); sprintf(b,"  Disk Product   : %s", s); vrdb_add(b); }
        { char s[8],  b[80]; vrdb_str(r->rdb_DiskRevision,       4, s, sizeof(s)); sprintf(b,"  Disk Revision  : %s", s); vrdb_add(b); }
        { char s[20], b[80]; vrdb_str(r->rdb_ControllerVendor,   8, s, sizeof(s)); sprintf(b,"  Ctrl Vendor    : %s", s); vrdb_add(b); }
        { char s[20], b[80]; vrdb_str(r->rdb_ControllerProduct, 16, s, sizeof(s)); sprintf(b,"  Ctrl Product   : %s", s); vrdb_add(b); }
        { char s[8],  b[80]; vrdb_str(r->rdb_ControllerRevision, 4, s, sizeof(s)); sprintf(b,"  Ctrl Revision  : %s", s); vrdb_add(b); }
        /* DriveInitName: null-terminated string (jdow extension, 40 bytes) */
        { char s[44], b[80];
          strncpy(s, r->rdb_DriveInitName, 39); s[39] = '\0';
          /* sanitize */
          { UWORD ii; for (ii=0; s[ii]; ii++) if (s[ii]<0x20||s[ii]>0x7E) s[ii]='.'; }
          if (s[0] == '\0') { s[0]='-'; s[1]='\0'; }
          sprintf(b,"  DriveInitName  : %s", s); vrdb_add(b); }

        /* --- Reserved Fields --- */
        vrdb_add("--- Reserved Fields ----------------------------------");
        { UWORD ri; char b[80];
          for (ri = 0; ri < 6; ri++) {
              ULONG v = r->rdb_Reserved1[ri];
              sprintf(b, "  Reserved1[%u]  : 0x%08lX%s", ri, (unsigned long)v,
                      (v == 0xFFFFFFFFUL) ? "" : "  <-- modified");
              vrdb_add(b); } }
        { UWORD ri; char b[80];
          for (ri = 0; ri < 3; ri++) {
              ULONG v = r->rdb_Reserved2[ri];
              sprintf(b, "  Reserved2[%u]  : 0x%08lX%s", ri, (unsigned long)v,
                      (v == 0xFFFFFFFFUL) ? "" : "  <-- modified");
              vrdb_add(b); } }
        { UWORD ri; char b[80];
          for (ri = 0; ri < 5; ri++) {
              ULONG v = r->rdb_Reserved3[ri];
              sprintf(b, "  Reserved3[%u]  : 0x%08lX%s", ri, (unsigned long)v,
                      (v == 0xFFFFFFFFUL) ? "" : "  <-- modified");
              vrdb_add(b); } }
        { char b[80]; ULONG v = r->rdb_Reserved4;
          sprintf(b, "  Reserved4     : 0x%08lX%s", (unsigned long)v,
                  (v == 0xFFFFFFFFUL || v == 0) ? "" : "  <-- modified");
          vrdb_add(b); }

        /* --- Extra data: bytes 256-511 (beyond RigidDiskBlock struct) --- */
        { const UBYTE *extra = buf + sizeof(struct RigidDiskBlock);
          UWORD xlen = (UWORD)((bd->block_size > sizeof(struct RigidDiskBlock))
                               ? bd->block_size - sizeof(struct RigidDiskBlock) : 0);
          vrdb_add("--- Extra Block Data (bytes 256-511) -----------------");
          if (xlen == 0) {
              vrdb_add("  (block size matches struct size - no extra bytes)");
          } else {
              BOOL has_data = FALSE;
              UWORD xi;
              for (xi = 0; xi < xlen; xi++)
                  if (extra[xi] != 0x00 && extra[xi] != 0xFF) { has_data = TRUE; break; }
              if (!has_data) {
                  char b[80];
                  sprintf(b, "  %u bytes - all 0x00 or 0xFF (nothing stored here)",
                          (unsigned)xlen);
                  vrdb_add(b);
              } else {
                  char b[80];
                  sprintf(b, "  %u bytes, contains data:", (unsigned)xlen);
                  vrdb_add(b);
                  for (xi = 0; xi < xlen; xi += 16) {
                      char hex[52], asc[18];
                      UWORD k, h = 0, a = 0;
                      for (k = 0; k < 16; k++) {
                          UBYTE c = (xi + k < xlen) ? extra[xi + k] : 0;
                          hex[h++] = "0123456789ABCDEF"[c >> 4];
                          hex[h++] = "0123456789ABCDEF"[c & 0xF];
                          hex[h++] = ' ';
                          if (k == 7) hex[h++] = ' ';
                          asc[a++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
                      }
                      hex[h] = '\0'; asc[a] = '\0';
                      sprintf(b, " %04X: %s%s",
                              (unsigned)(0x100 + xi), hex, asc);
                      vrdb_add(b);
                  }
              }
          }
        }
    }

    /* ---- Open window ---- */
    {
        struct Screen  *scr    = NULL;
        APTR            vi     = NULL;
        struct Gadget  *glist  = NULL;
        struct Window  *vwin   = NULL;
        UWORD font_h, bor_t, bor_b, row_h, btn_h, pad, win_w, win_h, min_h;
        UWORD scr_w, scr_h;

        scr = LockPubScreen(NULL);
        if (!scr) goto vrdb_cleanup;
        vi = GetVisualInfoA(scr, NULL);
        if (!vi) goto vrdb_cleanup;

        font_h = scr->Font->ta_YSize;
        bor_t  = scr->WBorTop + font_h + 1;
        bor_b  = scr->WBorBottom;
        pad    = 4;
        row_h  = font_h + 2;
        btn_h  = font_h + 6;
        win_w  = 520;
        win_h  = bor_t + pad + row_h * 18 + pad + btn_h + pad + bor_b;
        min_h  = bor_t + pad + row_h *  4 + pad + btn_h + pad + bor_b;
        scr_w  = scr->Width;
        scr_h  = scr->Height;

        if (!vrdb_make_gadgets(vi, scr, win_w, win_h, &glist))
            goto vrdb_cleanup;

        { struct TagItem wt[] = {
              { WA_Left,      (ULONG)((scr_w - win_w) / 2) },
              { WA_Top,       (ULONG)((scr_h - win_h) / 2) },
              { WA_Width,     win_w }, { WA_Height, win_h },
              { WA_Title,     (ULONG)"RDB Block - View" },
              { WA_Gadgets,   (ULONG)glist },
              { WA_PubScreen, (ULONG)scr },
              { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                              IDCMP_GADGETDOWN|IDCMP_REFRESHWINDOW|IDCMP_NEWSIZE },
              { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                              WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH|
                              WFLG_SIZEGADGET|WFLG_SIZEBBOTTOM },
              { WA_MinWidth,  300 },
              { WA_MinHeight, min_h },
              { WA_MaxWidth,  scr_w },
              { WA_MaxHeight, scr_h },
              { TAG_DONE, 0 } };
          vwin = OpenWindowTagList(NULL, wt); }

        UnlockPubScreen(NULL, scr); scr = NULL;
        if (!vwin) goto vrdb_cleanup;
        GT_RefreshWindow(vwin, NULL);

        {
            BOOL running = TRUE;
            while (running) {
                struct IntuiMessage *imsg;
                WaitPort(vwin->UserPort);
                while ((imsg = GT_GetIMsg(vwin->UserPort)) != NULL) {
                    ULONG iclass = imsg->Class;
                    struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                    GT_ReplyIMsg(imsg);
                    switch (iclass) {
                    case IDCMP_CLOSEWINDOW: running = FALSE; break;
                    case IDCMP_NEWSIZE: {
                        struct Gadget *newglist = NULL;
                        RemoveGList(vwin, glist, -1);
                        FreeGadgets(glist);
                        glist = NULL;
                        if (vrdb_make_gadgets(vi, vwin->WScreen,
                                              (UWORD)vwin->Width,
                                              (UWORD)vwin->Height,
                                              &newglist)) {
                            glist = newglist;
                            AddGList(vwin, glist, ~0, -1, NULL);
                            RefreshGList(glist, vwin, NULL, -1);
                        }
                        GT_RefreshWindow(vwin, NULL);
                        break; }
                    case IDCMP_GADGETUP:
                        if (gad->GadgetID == VRDB_DONE) running = FALSE;
                        break;
                    case IDCMP_REFRESHWINDOW:
                        GT_BeginRefresh(vwin); GT_EndRefresh(vwin, TRUE); break;
                    }
                }
            }
        }

vrdb_cleanup:
        if (vwin)  { RemoveGList(vwin, glist, -1); CloseWindow(vwin); }
        if (glist)   FreeGadgets(glist);
        if (vi)      FreeVisualInfo(vi);
        if (scr)     UnlockPubScreen(NULL, scr);
    }
    FreeVec(buf);
}

/* ------------------------------------------------------------------ */
/* Raw Block Scan — diagnostic: CMD_READ blocks 0-15 + block 0 dump   */
/* Works regardless of rdb->valid; bypasses BlockDev_ReadBlock.        */
/* ------------------------------------------------------------------ */

static void rdb_raw_scan(struct Window *win, struct BlockDev *bd)
{
    UBYTE *buf;
    ULONG  blk, i, j;
    BYTE   err;
    char   line[80];
    struct EasyStruct es;

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Raw Block Scan";
        es.es_TextFormat=(UBYTE*)"No device open.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    vrdb_count = 0;
    vrdb_list.lh_Head     = (struct Node *)&vrdb_list.lh_Tail;
    vrdb_list.lh_Tail     = NULL;
    vrdb_list.lh_TailPred = (struct Node *)&vrdb_list.lh_Head;

    /* Helper: decode a 4-byte block ID into printable text + tag */
#define DECODE_ID(buf_, idval_, idtxt_, tag_) do { \
    ULONG _v = ((ULONG)(buf_)[0]<<24)|((ULONG)(buf_)[1]<<16)| \
               ((ULONG)(buf_)[2]<<8)|(ULONG)(buf_)[3]; \
    UWORD _k; \
    (idval_) = _v; \
    for (_k=0;_k<4;_k++) { char _c=(char)(buf_)[_k]; \
        (idtxt_)[_k]=(_c>=0x20&&_c<=0x7E)?_c:'.'; } \
    (idtxt_)[4]='\0'; \
    if      (_v==IDNAME_RIGIDDISK) (tag_)="RDSK"; \
    else if (_v==IDNAME_PARTITION) (tag_)="PART"; \
    else if (_v==IDNAME_FSHEADER)  (tag_)="FSHD"; \
    else if (_v==IDNAME_LOADSEG)   (tag_)="LSEG"; \
    else                           (tag_)=""; \
} while(0)

    /* --- Pass 1: single CMD_READ --- */
    vrdb_add("--- CMD_READ scan (single read), blocks 0-15 ---");

    for (blk = 0; blk < 16; blk++) {
        ULONG id; char idtxt[5]; const char *tag;

        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = 512;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = blk * 512UL;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

        if (err != 0) {
            sprintf(line, "Blk %2lu: err=%ld", (unsigned long)blk, (long)err);
            vrdb_add(line); continue;
        }
        DECODE_ID(buf, id, idtxt, tag);
        sprintf(line, "Blk %2lu: OK  id=%s 0x%08lX  %s",
                (unsigned long)blk, idtxt, id, tag);
        vrdb_add(line);
    }

    /* --- Pass 2: double CMD_READ (same as RDB_Read's read2) ---
       On the A3000 SDMAC the first read of any block may return stale DMA
       FIFO data; the second consecutive read is reliable.  If pass 1 missed
       RDSK but pass 2 finds it, read2 is working correctly.  If neither
       finds it, CMD_READ itself is the problem. */
    vrdb_add("");
    vrdb_add("--- CMD_READ scan (double read = read2), blocks 0-15 ---");

    for (blk = 0; blk < 16; blk++) {
        ULONG id; char idtxt[5]; const char *tag;

        /* First read — discard, just to prime the DMA FIFO */
        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = 512;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = blk * 512UL;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        (void)DoIO((struct IORequest *)&bd->iotd);   /* ignore first result */

        /* Second read — this is the stable one */
        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = 512;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = blk * 512UL;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

        if (err != 0) {
            sprintf(line, "Blk %2lu: err=%ld", (unsigned long)blk, (long)err);
            vrdb_add(line); continue;
        }
        DECODE_ID(buf, id, idtxt, tag);
        sprintf(line, "Blk %2lu: OK  id=%s 0x%08lX  %s",
                (unsigned long)blk, idtxt, id, tag);
        vrdb_add(line);
    }

    /* Re-read block 0 and show hex dump (first 128 bytes) */
    vrdb_add("");
    vrdb_add("--- Block 0 hex dump (bytes 0-127) ---");

    bd->iotd.iotd_Req.io_Command = CMD_READ;
    bd->iotd.iotd_Req.io_Length  = 512;
    bd->iotd.iotd_Req.io_Data    = (APTR)buf;
    bd->iotd.iotd_Req.io_Offset  = 0;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

    if (err != 0) {
        vrdb_add("(re-read block 0 failed)");
    } else {
        for (i = 0; i < 128; i += 16) {
            char *p = line;
            sprintf(p, "%04lX: ", (unsigned long)i);
            p += 6;
            for (j = 0; j < 16; j++) {
                sprintf(p, "%02lX ", (unsigned long)buf[i + j]);
                p += 3;
                if (j == 7) *p++ = ' ';
            }
            *p++ = ' ';
            for (j = 0; j < 16; j++) {
                char c = (char)buf[i + j];
                *p++ = (c >= 0x20 && c <= 0x7E) ? c : '.';
            }
            *p = '\0';
            vrdb_add(line);
        }
    }

    /* Key RDSK fields + first PART block decode.
       Scan blocks 0-15 to find the actual RDSK (same logic as RDB_Read). */
    {
    ULONG rdsk_blk = 0xFFFFFFFFUL;
    ULONG scan_b;
    for (scan_b = 0; scan_b < 16; scan_b++) {
        ULONG id;
        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = 512;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = scan_b * 512UL;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        if (DoIO((struct IORequest *)&bd->iotd) != 0) continue;
        id = ((ULONG)buf[0]<<24)|((ULONG)buf[1]<<16)|
             ((ULONG)buf[2]<<8)|(ULONG)buf[3];
        if (id == IDNAME_RIGIDDISK) { rdsk_blk = scan_b; break; }
    }

    if (rdsk_blk == 0xFFFFFFFFUL) {
        vrdb_add("");
        vrdb_add("--- RDSK: not found in blocks 0-15 ---");
    } else {
        /* Re-read RDSK block 3× with BlockDev_ReadBlock so key fields
           (PartitionList, Cylinders, etc.) are stable despite SDMAC lag.
           The scan loop above used a single CMD_READ which is only reliable
           for the first 4 bytes (block ID) on A3000 hardware. */
        BlockDev_ReadBlock(bd, rdsk_blk, buf);
        BlockDev_ReadBlock(bd, rdsk_blk, buf);
        BlockDev_ReadBlock(bd, rdsk_blk, buf);
        {
        ULONG *lw = (ULONG *)buf;
        ULONG part_blk;
        char  hdr[48];
        sprintf(hdr, "--- RDSK key fields (block %lu) ---", rdsk_blk);
        vrdb_add("");
        vrdb_add(hdr);
        /* lw[7]=PartitionList  lw[16]=Cylinders  lw[17]=Sectors  lw[18]=Heads */
        sprintf(line, "PartList=blk %lu  Cyls=%lu  Secs=%lu  Heads=%lu",
                lw[7], lw[16], lw[17], lw[18]);
        vrdb_add(line);

        part_blk = lw[7];
        vrdb_add("");
        if (part_blk == RDB_END_MARK) {
            vrdb_add("--- PART: PartitionList = end-of-list ---");
        } else {
            sprintf(line, "--- PART block at blk %lu ---", part_blk);
            vrdb_add(line);

            /* Three-pass read matching RDB_Read: two priming reads then
               the real read. */
            BlockDev_ReadBlock(bd, part_blk, buf);   /* prime 1 — discard */
            BlockDev_ReadBlock(bd, part_blk, buf);   /* prime 2 — discard */
            if (!BlockDev_ReadBlock(bd, part_blk, buf))
                err = -1;
            else
                err = 0;

            if (err != 0) {
                sprintf(line, "  read err");
                vrdb_add(line);
            } else {
                /* pb_DriveName BSTR @ offset 36; pb_Environment @ offset 128 */
                UBYTE *bstr = buf + 36;
                ULONG *env  = (ULONG *)(buf + 128);
                UBYTE  nlen = bstr[0];
                char   nm[33];
                ULONG  k;
                ULONG  pid = ((ULONG)buf[0]<<24)|((ULONG)buf[1]<<16)|
                             ((ULONG)buf[2]<< 8)| (ULONG)buf[3];

                sprintf(line, "  ID=0x%08lX  namelen=%lu", pid, (unsigned long)nlen);
                vrdb_add(line);

                /* Show raw bytes 36-43 so we can see the BSTR regardless of length */
                { char *p = line;
                  sprintf(p, "  name raw [36..43]: "); p += 20;
                  for (k = 0; k < 8; k++) {
                      sprintf(p, "%02lX ", (unsigned long)buf[36 + k]); p += 3;
                  }
                  *p = '\0'; vrdb_add(line); }

                if (nlen > 31) nlen = 31;
                for (k = 0; k < (ULONG)nlen; k++) {
                    char c = (char)bstr[1 + k];
                    nm[k] = (c >= 0x20 && c <= 0x7E) ? c : '.';
                }
                nm[nlen] = '\0';
                sprintf(line, "  Name='%s'", nm);
                vrdb_add(line);

                sprintf(line, "  env[0]=TableSz=%lu  env[1]=SizeBlk=%lu",
                        env[0], env[1]);
                vrdb_add(line);
                sprintf(line, "  env[3]=Heads=%lu  env[5]=Secs=%lu",
                        env[3], env[5]);
                vrdb_add(line);
                sprintf(line, "  env[9]=LoCyl=%lu  env[10]=HiCyl=%lu",
                        env[9], env[10]);
                vrdb_add(line);
                sprintf(line, "  env[11]=NumBuf=%lu  env[15]=BootPri=%lu",
                        env[11], env[15]);
                vrdb_add(line);
                sprintf(line, "  env[16]=DosType=0x%08lX  raw@0xC0: %02lX %02lX %02lX %02lX",
                        env[16],
                        (unsigned long)buf[192], (unsigned long)buf[193],
                        (unsigned long)buf[194], (unsigned long)buf[195]);
                vrdb_add(line);
                sprintf(line, "  env[13]=MaxXfer=0x%08lX  env[14]=Mask=0x%08lX",
                        env[13], env[14]);
                vrdb_add(line);

                /* Hex dump of env region: bytes 128-207 (env[0]-env[19]) */
                vrdb_add("  env hex (bytes 0x80-0xCF):");
                { ULONG off;
                  for (off = 128; off < 208; off += 16) {
                      char *p = line;
                      *p++ = ' '; *p++ = ' ';
                      sprintf(p, "%04lX: ", (unsigned long)off); p += 6;
                      for (k = 0; k < 16; k++) {
                          sprintf(p, "%02lX ", (unsigned long)buf[off + k]);
                          p += 3;
                          if (k == 7) *p++ = ' ';
                      }
                      *p++ = ' ';
                      for (k = 0; k < 16; k++) {
                          char c = (char)buf[off + k];
                          *p++ = (c >= 0x20 && c <= 0x7E) ? c : '.';
                      }
                      *p = '\0';
                      vrdb_add(line);
                  }
                }
            }
        }
        } /* end extra { from RDSK re-read */
    } /* end else (rdsk_blk found) */
    } /* end rdsk scan block */

    /* ---- DosList scan: show device entries for this devname+unit ---- */
    {
        struct DosList *dl;
        vrdb_add("");
        vrdb_add("--- DosList entries (this device+unit) ---");
        dl = LockDosList(LDF_DEVICES | LDF_READ);
        while ((dl = NextDosEntry(dl, LDF_DEVICES)) != NULL) {
            struct FileSysStartupMsg *fssm;
            const UBYTE *dev_bstr;
            char  dev_name[64];
            char  node_name[36];
            const UBYTE *nm;
            UBYTE dev_len, nm_len, k;

            if (dl->dol_misc.dol_handler.dol_Startup == 0) continue;
            fssm = (struct FileSysStartupMsg *)
                   BADDR(dl->dol_misc.dol_handler.dol_Startup);
            if (!fssm) continue;

            dev_bstr = (const UBYTE *)BADDR(fssm->fssm_Device);
            if (!dev_bstr) continue;
            dev_len = dev_bstr[0];
            if (dev_len > 62) dev_len = 62;
            for (k = 0; k < dev_len; k++) dev_name[k] = (char)dev_bstr[1+k];
            dev_name[dev_len] = '\0';

            nm = (const UBYTE *)BADDR(dl->dol_Name);
            if (nm) {
                nm_len = nm[0]; if (nm_len > 30) nm_len = 30;
                for (k = 0; k < nm_len; k++) node_name[k] = (char)nm[1+k];
                node_name[nm_len] = '\0';
            } else {
                node_name[0] = '\0';
            }

            if (!fssm->fssm_Environ) continue;
            {
                const ULONG *env = (const ULONG *)BADDR(fssm->fssm_Environ);
                ULONG lo = 0, hi = 0;
                if (env && env[DE_TABLESIZE] >= (ULONG)DE_UPPERCYL) {
                    lo = env[DE_LOWCYL]; hi = env[DE_UPPERCYL];
                }
                sprintf(line, "  %s: unit=%lu dev=%s lo=%lu hi=%lu",
                        node_name, fssm->fssm_Unit, dev_name,
                        (unsigned long)lo, (unsigned long)hi);
                vrdb_add(line);
            }
        }
        UnLockDosList(LDF_DEVICES | LDF_READ);
    }

    /* ---- Multi-read stability test (A3000 DMA shift check) ----
     * 1. Read each block 4× consecutively into separate CHIP RAM buffers.
     *    Report byte diffs between reads.  0 diffs does NOT mean data is good —
     *    it means every read returns the same bytes (possibly all wrong).
     * 2. Show checksum status + key fields from read 1 so on-disk corruption
     *    is visible even when all 4 reads agree.
     * 3. Interleaved test: RDSK → PART → RDSK again.  If the two RDSK reads
     *    differ, reading PART is corrupting the DMA state for subsequent reads.
     * ------------------------------------------------------------------ */
    {
        UBYTE *b[4];
        int    bi, allok;
        ULONG  mrdsk = 0xFFFFFFFFUL, mpart = 0xFFFFFFFFUL;
        ULONG  scan_b2;

        vrdb_add("");
        vrdb_add("--- Multi-read stability test (A3000 DMA check) ---");

        allok = 1;
        for (bi = 0; bi < 4; bi++) b[bi] = NULL;
        for (bi = 0; bi < 4; bi++) {
            b[bi] = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
            if (!b[bi]) { allok = 0; break; }
        }

        if (!allok) {
            vrdb_add("  (AllocVec MEMF_PUBLIC failed)");
        } else {
            /* Re-scan for RDSK (<=16 CMD_READ into b[0]) */
            for (scan_b2 = 0; scan_b2 < 16; scan_b2++) {
                ULONG scan_id;
                bd->iotd.iotd_Req.io_Command = CMD_READ;
                bd->iotd.iotd_Req.io_Length  = 512;
                bd->iotd.iotd_Req.io_Data    = (APTR)b[0];
                bd->iotd.iotd_Req.io_Offset  = scan_b2 * 512UL;
                bd->iotd.iotd_Req.io_Flags   = 0;
                bd->iotd.iotd_Count          = 0;
                if (DoIO((struct IORequest *)&bd->iotd) != 0) continue;
                scan_id = ((ULONG)b[0][0]<<24)|((ULONG)b[0][1]<<16)|
                          ((ULONG)b[0][2]<<8)|(ULONG)b[0][3];
                if (scan_id == IDNAME_RIGIDDISK) {
                    mrdsk = scan_b2;
                    /* PartitionList at longword offset 7 = byte 28 */
                    mpart = ((ULONG)b[0][28]<<24)|((ULONG)b[0][29]<<16)|
                            ((ULONG)b[0][30]<<8)|(ULONG)b[0][31];
                    if (mpart == RDB_END_MARK) mpart = 0xFFFFFFFFUL;
                    break;
                }
            }

            if (mrdsk == 0xFFFFFFFFUL) {
                vrdb_add("  (RDSK not found - skipping)");
            } else {
/* ----------------------------------------------------------------
 * MREAD_BLK(blkno_, is_part_)
 *   Read blkno_ 4× into b[0..3].
 *   Show: differential comparison + checksum of b[0] + key fields.
 *   is_part_: 0=RDSK, 1=PART.
 * -------------------------------------------------------------- */
#define MREAD_BLK(blkno_, is_part_) do { \
    ULONG _bn = (blkno_); \
    int   _r, _cmp, _rdok = 1; \
    char  _hdr[64]; \
    sprintf(_hdr, "  Block %lu (%s): 4 reads", \
            _bn, (is_part_) ? "PART" : "RDSK"); \
    vrdb_add(_hdr); \
    for (_r = 0; _r < 4; _r++) { \
        bd->iotd.iotd_Req.io_Command = CMD_READ; \
        bd->iotd.iotd_Req.io_Length  = 512; \
        bd->iotd.iotd_Req.io_Data    = (APTR)b[_r]; \
        bd->iotd.iotd_Req.io_Offset  = _bn * 512UL; \
        bd->iotd.iotd_Req.io_Flags   = 0; \
        bd->iotd.iotd_Count          = 0; \
        if (DoIO((struct IORequest *)&bd->iotd) != 0) { _rdok = 0; break; } \
    } \
    if (!_rdok) { vrdb_add("    read error"); break; } \
    /* differential comparison */ \
    for (_cmp = 1; _cmp < 4; _cmp++) { \
        ULONG _off, _nd = 0, _sh = 0; \
        char  _ln[80]; char *_lx = _ln; \
        for (_off = 0; _off < 512; _off++) \
            if (b[_cmp][_off] != b[0][_off]) _nd++; \
        sprintf(_lx, "    R%d vs R1: %lu differ", _cmp+1, _nd); \
        _lx += strlen(_lx); \
        if (_nd > 0) { \
            for (_off = 0; _off < 512 && _sh < 6; _off++) { \
                if (b[_cmp][_off] != b[0][_off]) { \
                    sprintf(_lx, "  @%03lX:%02X->%02X", \
                        (unsigned long)_off, \
                        (unsigned)b[0][_off], (unsigned)b[_cmp][_off]); \
                    _lx += strlen(_lx); _sh++; \
                } \
            } \
        } \
        *_lx = '\0'; vrdb_add(_ln); \
    } \
    /* checksum of read 1 — catches on-disk corruption missed by diffs */ \
    { const ULONG *_lp = (const ULONG *)b[0]; \
      ULONG _sl = _lp[1], _sm = 0, _si; \
      if (_sl >= 2 && _sl <= 128) for (_si=0;_si<_sl;_si++) _sm+=_lp[_si]; \
      sprintf(line, "    csum: SL=%lu sum=0x%08lX %s", \
              _sl, _sm, (_sl>=2&&_sl<=128&&_sm==0)?"OK":"BAD"); \
      vrdb_add(line); \
    } \
    /* key fields — show actual bytes so corruption is explicit */ \
    if (!(is_part_)) { \
        const ULONG *_lp = (const ULONG *)b[0]; \
        sprintf(line, "    Cyls=%lu Heads=%lu Secs=%lu PartList=%lu", \
                _lp[16], _lp[18], _lp[17], _lp[7]); \
        vrdb_add(line); \
    } else { \
        /* name BSTR at byte 36; DosEnvec[16]=DosType at byte 192 (0xC0) */ \
        UBYTE _nl = b[0][36]; char _nm[12]; ULONG _dk; \
        ULONG _dt; \
        if (_nl > 10) _nl = 10; \
        for (_dk=0;_dk<(ULONG)_nl;_dk++) { \
            char _c=b[0][37+_dk]; _nm[_dk]=(_c>=0x20&&_c<=0x7E)?_c:'.'; } \
        _nm[_nl]='\0'; \
        _dt=((ULONG)b[0][192]<<24)|((ULONG)b[0][193]<<16)| \
            ((ULONG)b[0][194]<<8)|(ULONG)b[0][195]; \
        sprintf(line, \
            "    n[36]: len=%u '%s' raw:%02X %02X %02X %02X", \
            (unsigned)b[0][36], _nm, \
            b[0][36], b[0][37], b[0][38], b[0][39]); \
        vrdb_add(line); \
        sprintf(line, \
            "    DosType[C0]: %02X %02X %02X %02X = 0x%08lX", \
            b[0][192],b[0][193],b[0][194],b[0][195],_dt); \
        vrdb_add(line); \
    } \
} while(0)

                MREAD_BLK(mrdsk, 0);
                if (mpart != 0xFFFFFFFFUL)
                    MREAD_BLK(mpart, 1);
                else
                    vrdb_add("  (no PART block - PartList is end-mark)");

#undef MREAD_BLK

                /* Interleaved test: RDSK → PART → RDSK
                   If the two RDSK reads differ, reading PART pollutes DMA state.
                   If they agree but csum is BAD, the corruption is on-disk.     */
                if (mpart != 0xFFFFFFFFUL) {
                    ULONG _off, _nd = 0, _sh = 0;
                    /* RDSK → b[0] */
                    bd->iotd.iotd_Req.io_Command = CMD_READ;
                    bd->iotd.iotd_Req.io_Length  = 512;
                    bd->iotd.iotd_Req.io_Data    = (APTR)b[0];
                    bd->iotd.iotd_Req.io_Offset  = mrdsk * 512UL;
                    bd->iotd.iotd_Req.io_Flags   = 0;
                    bd->iotd.iotd_Count          = 0;
                    DoIO((struct IORequest *)&bd->iotd);
                    /* PART → b[1] (advance DMA state) */
                    bd->iotd.iotd_Req.io_Command = CMD_READ;
                    bd->iotd.iotd_Req.io_Length  = 512;
                    bd->iotd.iotd_Req.io_Data    = (APTR)b[1];
                    bd->iotd.iotd_Req.io_Offset  = mpart * 512UL;
                    bd->iotd.iotd_Req.io_Flags   = 0;
                    bd->iotd.iotd_Count          = 0;
                    DoIO((struct IORequest *)&bd->iotd);
                    /* RDSK again → b[2] */
                    bd->iotd.iotd_Req.io_Command = CMD_READ;
                    bd->iotd.iotd_Req.io_Length  = 512;
                    bd->iotd.iotd_Req.io_Data    = (APTR)b[2];
                    bd->iotd.iotd_Req.io_Offset  = mrdsk * 512UL;
                    bd->iotd.iotd_Req.io_Flags   = 0;
                    bd->iotd.iotd_Count          = 0;
                    DoIO((struct IORequest *)&bd->iotd);
                    /* compare b[0] (first RDSK) vs b[2] (second RDSK) */
                    for (_off = 0; _off < 512; _off++)
                        if (b[0][_off] != b[2][_off]) _nd++;
                    {
                        char _ln[80]; char *_lx = _ln;
                        sprintf(_lx, "  Interleaved RDSK re-read: %lu differ", _nd);
                        _lx += strlen(_lx);
                        if (_nd > 0) {
                            for (_off = 0; _off < 512 && _sh < 6; _off++) {
                                if (b[0][_off] != b[2][_off]) {
                                    sprintf(_lx, "  @%03lX:%02X->%02X",
                                        (unsigned long)_off,
                                        (unsigned)b[0][_off],
                                        (unsigned)b[2][_off]);
                                    _lx += strlen(_lx); _sh++;
                                }
                            }
                        }
                        *_lx = '\0';
                        vrdb_add(_ln);
                    }
                }
            }
        }

        for (bi = 0; bi < 4; bi++) if (b[bi]) FreeVec(b[bi]);
    }

    FreeVec(buf);

    /* Open display window */
    {
        struct Screen  *scr    = NULL;
        APTR            vi     = NULL;
        struct Gadget  *glist  = NULL;
        struct Window  *vwin   = NULL;
        UWORD font_h, bor_t, bor_b, row_h, btn_h, pad, win_w, win_h, min_h;
        UWORD scr_w, scr_h;

        scr = LockPubScreen(NULL);
        if (!scr) return;
        vi = GetVisualInfoA(scr, NULL);
        if (!vi) { UnlockPubScreen(NULL, scr); return; }

        font_h = (UWORD)scr->Font->ta_YSize;
        bor_t  = (UWORD)scr->WBorTop + font_h + 1;
        bor_b  = (UWORD)scr->WBorBottom;
        pad    = 4;
        row_h  = font_h + 2;
        btn_h  = font_h + 6;
        win_w  = 560;
        win_h  = bor_t + pad + row_h * 24 + pad + btn_h + pad + bor_b;
        min_h  = bor_t + pad + row_h *  4 + pad + btn_h + pad + bor_b;
        scr_w  = scr->Width;
        scr_h  = scr->Height;

        if (!vrdb_make_gadgets(vi, scr, win_w, win_h, &glist))
            goto rscan_cleanup;

        { struct TagItem wt[] = {
              { WA_Left,      (ULONG)((scr_w - win_w) / 2) },
              { WA_Top,       (ULONG)((scr_h - win_h) / 2) },
              { WA_Width,     win_w }, { WA_Height, win_h },
              { WA_Title,     (ULONG)"Raw Block Scan" },
              { WA_Gadgets,   (ULONG)glist },
              { WA_PubScreen, (ULONG)scr },
              { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                              IDCMP_GADGETDOWN|IDCMP_REFRESHWINDOW|IDCMP_NEWSIZE },
              { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                              WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH|
                              WFLG_SIZEGADGET|WFLG_SIZEBBOTTOM },
              { WA_MinWidth,  300 },
              { WA_MinHeight, min_h },
              { WA_MaxWidth,  scr_w },
              { WA_MaxHeight, scr_h },
              { TAG_DONE, 0 } };
          vwin = OpenWindowTagList(NULL, wt); }

        UnlockPubScreen(NULL, scr); scr = NULL;
        if (!vwin) goto rscan_cleanup;
        GT_RefreshWindow(vwin, NULL);

        { BOOL running = TRUE;
          while (running) {
              struct IntuiMessage *imsg;
              WaitPort(vwin->UserPort);
              while ((imsg = GT_GetIMsg(vwin->UserPort)) != NULL) {
                  ULONG iclass = imsg->Class;
                  struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                  GT_ReplyIMsg(imsg);
                  switch (iclass) {
                  case IDCMP_CLOSEWINDOW:
                      running = FALSE; break;
                  case IDCMP_NEWSIZE: {
                      struct Gadget *ng2 = NULL;
                      RemoveGList(vwin, glist, -1);
                      FreeGadgets(glist); glist = NULL;
                      if (vrdb_make_gadgets(vi, vwin->WScreen,
                                            (UWORD)vwin->Width, (UWORD)vwin->Height, &ng2)) {
                          glist = ng2;
                          AddGList(vwin, glist, ~0, -1, NULL);
                          RefreshGList(glist, vwin, NULL, -1);
                      }
                      GT_RefreshWindow(vwin, NULL);
                      break; }
                  case IDCMP_REFRESHWINDOW:
                      GT_BeginRefresh(vwin); GT_EndRefresh(vwin, TRUE); break;
                  case IDCMP_GADGETUP:
                      if (gad->GadgetID == VRDB_DONE) running = FALSE; break;
                  }
              }
          }
        }

rscan_cleanup:
        if (vwin)  { RemoveGList(vwin, glist, -1); CloseWindow(vwin); }
        if (glist)   FreeGadgets(glist);
        if (vi)      FreeVisualInfo(vi);
        if (scr)     UnlockPubScreen(NULL, scr);
    }
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* raw_disk_read — show all on-disk fields for blocks 0-15            */
/* Single CMD_READ per block, no retries, no fixups.  Shows exactly   */
/* what is stored on the medium — useful for debugging DMA corruption. */
/* ------------------------------------------------------------------ */

static void raw_disk_read(struct Window *win, struct BlockDev *bd)
{
    UBYTE *buf;
    char   line[80];
    ULONG  blk;
    ULONG  rc_last_lba  = 0;   /* last LBA from READ CAPACITY, 0 if unavailable */
    ULONG  rc_blksz     = 512; /* block size from READ CAPACITY */
    BOOL   scsi_ok      = FALSE; /* TRUE if HD_SCSICMD works on this device */
    struct DriveGeometry geom;
    struct EasyStruct es;

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Raw Disk Read";
        es.es_TextFormat=(UBYTE*)"No device open.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    vrdb_count = 0;
    vrdb_list.lh_Head     = (struct Node *)&vrdb_list.lh_Tail;
    vrdb_list.lh_Tail     = NULL;
    vrdb_list.lh_TailPred = (struct Node *)&vrdb_list.lh_Head;

    /* --- TD_GETGEOMETRY --- */
    vrdb_add("=== TD_GETGEOMETRY ===");
    memset(&geom, 0, sizeof(geom));
    bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
    bd->iotd.iotd_Req.io_Length  = sizeof(geom);
    bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
    bd->iotd.iotd_Req.io_Flags   = 0;
    if (DoIO((struct IORequest *)&bd->iotd) == 0) {
        sprintf(line, "  TotalSectors : %lu", (unsigned long)geom.dg_TotalSectors);
        vrdb_add(line);
        sprintf(line, "  Cylinders    : %lu", (unsigned long)geom.dg_Cylinders);
        vrdb_add(line);
        sprintf(line, "  Heads        : %lu", (unsigned long)geom.dg_Heads);
        vrdb_add(line);
        sprintf(line, "  TrackSectors : %lu", (unsigned long)geom.dg_TrackSectors);
        vrdb_add(line);
        sprintf(line, "  SectorSize   : %lu", (unsigned long)geom.dg_SectorSize);
        vrdb_add(line);
        sprintf(line, "  DeviceType   : %lu  Flags: 0x%lX",
                (unsigned long)geom.dg_DeviceType,
                (unsigned long)geom.dg_Flags);
        vrdb_add(line);
    } else {
        vrdb_add("  TD_GETGEOMETRY failed (error or unsupported)");
    }
    vrdb_add("");

    /* --- SCSI INQUIRY — asks the device directly, bypasses driver --- */
    vrdb_add("=== SCSI INQUIRY (HD_SCSICMD, direct to device) ===");
    {
        struct SCSICmd scmd;
        UBYTE cdb[6];
        UBYTE sense[32];
        BYTE  err;
        UBYTE i, j;

        memset(buf,   0, 64);
        memset(sense, 0, sizeof(sense));
        memset(&scmd, 0, sizeof(scmd));
        cdb[0] = 0x12;   /* INQUIRY */
        cdb[1] = 0x00;
        cdb[2] = 0x00;
        cdb[3] = 0x00;
        cdb[4] = 36;     /* allocation length */
        cdb[5] = 0x00;

        scmd.scsi_Data        = (UWORD *)buf;
        scmd.scsi_Length      = 36;
        scmd.scsi_Command     = cdb;
        scmd.scsi_CmdLength   = 6;
        scmd.scsi_Flags       = SCSIF_READ;
        scmd.scsi_SenseData   = sense;
        scmd.scsi_SenseLength = sizeof(sense);

        bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
        bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
        bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

        if (err == IOERR_NOCMD) {
            vrdb_add("  Not supported (non-SCSI device)");
        } else if (err != 0) {
            sprintf(line, "  Error: %ld", (long)err);
            vrdb_add(line);
        } else {
            char vendor[9], product[17], revision[5];
            UBYTE devtype = buf[0] & 0x1F;

            for (i = 0; i < 8;  i++) vendor[i]   = (buf[8 +i] >= 0x20) ? (char)buf[8 +i] : ' ';
            for (i = 0; i < 16; i++) product[i]  = (buf[16+i] >= 0x20) ? (char)buf[16+i] : ' ';
            for (i = 0; i < 4;  i++) revision[i] = (buf[32+i] >= 0x20) ? (char)buf[32+i] : ' ';
            vendor[8] = product[16] = revision[4] = '\0';
            for (i = 7;  i > 0 && vendor[i]   == ' '; i--) vendor[i]   = '\0';
            for (i = 15; i > 0 && product[i]  == ' '; i--) product[i]  = '\0';
            for (i = 3;  i > 0 && revision[i] == ' '; i--) revision[i] = '\0';

            scsi_ok = TRUE;
            sprintf(line, "  DevType : 0x%02X  Vendor  : \"%s\"",
                    (unsigned)devtype, vendor);
            vrdb_add(line);
            sprintf(line, "  Product : \"%s\"  Rev: \"%s\"",
                    product, revision);
            vrdb_add(line);

            /* Hex dump: 36 bytes, 16 per row */
            for (i = 0; i < 36; i += 16) {
                UBYTE end = i + 16; if (end > 36) end = 36;
                char *p = line;
                p += sprintf(p, "  %02X:", (unsigned)i);
                for (j = i; j < end; j++) p += sprintf(p, " %02X", (unsigned)buf[j]);
                vrdb_add(line);
            }
        }
    }
    vrdb_add("");

    /* --- SCSI READ CAPACITY(10) — get real sector count from device --- */
    vrdb_add("=== SCSI READ CAPACITY(10) (HD_SCSICMD, direct to device) ===");
    {
        struct SCSICmd scmd;
        UBYTE cdb[10];
        UBYTE sense[32];
        BYTE  err;

        memset(buf,   0, 16);
        memset(sense, 0, sizeof(sense));
        memset(&scmd, 0, sizeof(scmd));
        memset(cdb,   0, sizeof(cdb));
        cdb[0] = 0x25;   /* READ CAPACITY (10) */

        scmd.scsi_Data        = (UWORD *)buf;
        scmd.scsi_Length      = 8;
        scmd.scsi_Command     = cdb;
        scmd.scsi_CmdLength   = 10;
        scmd.scsi_Flags       = SCSIF_READ;
        scmd.scsi_SenseData   = sense;
        scmd.scsi_SenseLength = sizeof(sense);

        bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
        bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
        bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

        if (err == IOERR_NOCMD) {
            vrdb_add("  Not supported (non-SCSI device)");
        } else if (err != 0) {
            sprintf(line, "  Error: %ld", (long)err);
            vrdb_add(line);
        } else {
            ULONG last_lba = ((ULONG)buf[0]<<24)|((ULONG)buf[1]<<16)|
                             ((ULONG)buf[2]<<8)|(ULONG)buf[3];
            ULONG blksz    = ((ULONG)buf[4]<<24)|((ULONG)buf[5]<<16)|
                             ((ULONG)buf[6]<<8)|(ULONG)buf[7];
            ULONG total    = last_lba + 1;
            char  szbuf[16];

            rc_last_lba = last_lba;
            rc_blksz    = (blksz > 0) ? blksz : 512;
            scsi_ok     = TRUE;

            sprintf(line, "  LastLBA   : %lu  (TotalBlocks: %lu)",
                    (unsigned long)last_lba, (unsigned long)total);
            vrdb_add(line);
            sprintf(line, "  BlockSize : %lu bytes", (unsigned long)blksz);
            vrdb_add(line);
            FormatSize((UQUAD)total * (UQUAD)blksz, szbuf);
            sprintf(line, "  TotalSize : %s", szbuf);
            vrdb_add(line);
            sprintf(line, "  Hex: %02X %02X %02X %02X  %02X %02X %02X %02X",
                    buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
            vrdb_add(line);
        }
    }
    vrdb_add("");

    /* --- Capacity probe: binary-search real last LBA via HD_SCSICMD --- */
    /* Sends SCSI READ(10) directly to the drive at increasing LBAs.      */
    /* Drive responds with success or CHECK CONDITION — no driver         */
    /* interpretation involved.  Finds real capacity even if READ         */
    /* CAPACITY response was corrupted or truncated by the driver.        */
    vrdb_add("=== Real capacity probe (HD_SCSICMD binary search) ===");
    if (!scsi_ok) {
        vrdb_add("  Skipped (HD_SCSICMD not available)");
    } else {
        ULONG lo = rc_last_lba; /* last known readable LBA */
        ULONG hi = 0;           /* first known unreadable LBA */
        ULONG steps = 0;
        BYTE  perr;

/* Issue one SCSI READ(10) at given LBA into buf; result in perr */
#define PROBE_LBA(lba) do { \
    struct SCSICmd _sc; UBYTE _cdb[10]; UBYTE _sns[16]; \
    memset(&_sc,0,sizeof(_sc)); memset(_cdb,0,10); memset(_sns,0,16); \
    _cdb[0]=0x28; \
    _cdb[2]=(UBYTE)((lba)>>24); _cdb[3]=(UBYTE)((lba)>>16); \
    _cdb[4]=(UBYTE)((lba)>>8);  _cdb[5]=(UBYTE)(lba); \
    _cdb[8]=1; \
    _sc.scsi_Data=(UWORD*)buf; _sc.scsi_Length=rc_blksz; \
    _sc.scsi_Command=_cdb; _sc.scsi_CmdLength=10; \
    _sc.scsi_Flags=SCSIF_READ; \
    _sc.scsi_SenseData=_sns; _sc.scsi_SenseLength=16; \
    bd->iotd.iotd_Req.io_Command=HD_SCSICMD; \
    bd->iotd.iotd_Req.io_Length=sizeof(_sc); \
    bd->iotd.iotd_Req.io_Data=(APTR)&_sc; \
    bd->iotd.iotd_Req.io_Flags=0; bd->iotd.iotd_Count=0; \
    perr=(BYTE)DoIO((struct IORequest *)&bd->iotd); \
} while(0)

        /* Step 1: probe just past reported capacity.
           If it reads OK the driver is under-reporting — expand upward.
           If it fails we already have the real boundary. */
        PROBE_LBA(rc_last_lba + 1); steps++;
        if (perr != 0) {
            /* Reported capacity matches reality */
            hi = rc_last_lba + 1;
        } else {
            /* Drive has more sectors than reported — find real upper bound */
            lo = rc_last_lba + 1;
            while (steps < 32) {
                ULONG next = lo * 2 + 1;
                if (next > 0x40000000UL) next = 0x40000000UL; /* cap ~128GB */
                PROBE_LBA(next); steps++;
                if (perr == 0) {
                    lo = next;
                    if (next >= 0x40000000UL) { hi = next; break; }
                } else {
                    hi = next;
                    break;
                }
            }
        }

        /* Step 2: binary search to find exact last readable LBA */
        while (hi > lo + 1 && steps < 64) {
            ULONG mid = lo + (hi - lo) / 2;
            PROBE_LBA(mid); steps++;
            if (perr == 0) lo = mid;
            else           hi = mid;
        }
#undef PROBE_LBA

        {
            char szbuf[16];
            FormatSize((UQUAD)(lo + 1) * (UQUAD)rc_blksz, szbuf);
            sprintf(line, "  Probes used  : %lu", (unsigned long)steps);
            vrdb_add(line);
            sprintf(line, "  Last readable LBA : %lu", (unsigned long)lo);
            vrdb_add(line);
            sprintf(line, "  Real capacity     : %s  (%lu sectors)",
                    szbuf, (unsigned long)(lo + 1));
            vrdb_add(line);
            if (lo != rc_last_lba) {
                char szbuf2[16];
                FormatSize((UQUAD)(rc_last_lba + 1) * (UQUAD)rc_blksz, szbuf2);
                sprintf(line, "  READ CAPACITY said: %s — WRONG!", szbuf2);
                vrdb_add(line);
                vrdb_add("  *** Driver/DMA reports wrong size — use Manual Geometry! ***");
            } else {
                vrdb_add("  READ CAPACITY agrees — reported size is correct.");
            }
        }
    }
    vrdb_add("");

    /* --- Raw block scan --- */
    vrdb_add("=== Blocks 0-15: single CMD_READ, no fixups ===");
    for (blk = 0; blk < 16; blk++) {
        ULONG id, csum_stored, csum_calc;
        ULONG summed_longs;
        const ULONG *lp;
        ULONG i;
        BYTE  err;
        char  idtxt[8];

        memset(buf, 0, 512);
        bd->iotd.iotd_Req.io_Command = CMD_READ;
        bd->iotd.iotd_Req.io_Length  = 512;
        bd->iotd.iotd_Req.io_Data    = (APTR)buf;
        bd->iotd.iotd_Req.io_Offset  = blk * 512UL;
        bd->iotd.iotd_Req.io_Actual  = 0;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
        if (err != 0) {
            sprintf(line, "Block %2lu: CMD_READ error %ld",
                    (unsigned long)blk, (long)err);
            vrdb_add(line);
            continue;
        }

        lp = (const ULONG *)buf;
        id           = lp[0];
        summed_longs = lp[1];
        csum_stored  = (ULONG)((LONG)lp[2]);

        /* Compute checksum (only when SummedLongs is in valid range) */
        csum_calc = 0;
        if (summed_longs >= 2 && summed_longs <= 128)
            for (i = 0; i < summed_longs; i++) csum_calc += lp[i];

        /* ID as 4 printable chars */
        { int j;
          for (j = 0; j < 4; j++) {
              UBYTE c = (UBYTE)(id >> (24 - j * 8));
              idtxt[j] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
          }
          idtxt[4] = '\0'; }

        sprintf(line, "Block %2lu  ID=%08lX (%s)  SL=%lu  csum=%s",
                (unsigned long)blk, (unsigned long)id, idtxt,
                (unsigned long)summed_longs,
                (summed_longs < 2 || summed_longs > 128) ? "?(SL out of range)" :
                (csum_calc == 0) ? "OK" : "BAD");
        vrdb_add(line);

        if (id == IDNAME_RIGIDDISK) {
            /* RDSK fields */
            sprintf(line, "  Cyls=%lu  Heads=%lu  Secs=%lu",
                    (unsigned long)lp[16], (unsigned long)lp[18],
                    (unsigned long)lp[17]);
            vrdb_add(line);
            sprintf(line, "  RDBlo=%lu  RDBhi=%lu  LoCyl=%lu  HiCyl=%lu",
                    (unsigned long)lp[22], (unsigned long)lp[23],
                    (unsigned long)lp[24], (unsigned long)lp[25]);
            vrdb_add(line);
            sprintf(line, "  PartList=%08lX  FSHDList=%08lX",
                    (unsigned long)lp[7], (unsigned long)lp[8]);
            vrdb_add(line);

        } else if (id == IDNAME_PARTITION) {
            /* PART fields — note offsets 0x24 and 0xC0 are DMA-suspect */
            UBYTE *bstr = buf + 0x24;   /* pb_DriveName BSTR */
            ULONG *env  = (ULONG *)(buf + 128); /* pb_Environment */
            char  name[32];
            UBYTE len = bstr[0];
            UBYTE k;
            for (k = 0; k < 30 && k < len; k++) {
                UBYTE c = bstr[1+k];
                name[k] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
            }
            name[k] = '\0';
            sprintf(line, "  Name BSTR raw [0x24]: len=0x%02X \"%s\"",
                    (unsigned)bstr[0], name);
            vrdb_add(line);
            sprintf(line, "  Raw bytes 0x24-0x27: %02X %02X %02X %02X",
                    buf[0x24], buf[0x25], buf[0x26], buf[0x27]);
            vrdb_add(line);
            sprintf(line, "  DosType [0xC0]: %08lX  LoCyl=%lu  HiCyl=%lu",
                    (unsigned long)env[16],
                    (unsigned long)env[9], (unsigned long)env[10]);
            vrdb_add(line);
            sprintf(line, "  Raw bytes 0xC0-0xC3: %02X %02X %02X %02X",
                    buf[0xC0], buf[0xC1], buf[0xC2], buf[0xC3]);
            vrdb_add(line);
            sprintf(line, "  Next=%08lX  Flags=%08lX  DevFlags=%08lX",
                    (unsigned long)lp[4], (unsigned long)lp[5],
                    (unsigned long)lp[6]);
            vrdb_add(line);

        } else if (id == IDNAME_FSHEADER) {
            /* FSHD fields */
            sprintf(line, "  DosType=%08lX  Version=%lu.%lu",
                    (unsigned long)lp[8],
                    (unsigned long)(lp[9] >> 16),
                    (unsigned long)(lp[9] & 0xFFFF));
            vrdb_add(line);
            sprintf(line, "  Next=%08lX  SegListBlk=%08lX",
                    (unsigned long)lp[4], (unsigned long)lp[12]);
            vrdb_add(line);

        } else if (id == IDNAME_LOADSEG) {
            sprintf(line, "  Next=%08lX", (unsigned long)lp[4]);
            vrdb_add(line);

        } else if (id != 0 && id != 0xFFFFFFFFUL) {
            sprintf(line, "  Raw: %08lX %08lX %08lX %08lX",
                    (unsigned long)lp[0], (unsigned long)lp[1],
                    (unsigned long)lp[2], (unsigned long)lp[3]);
            vrdb_add(line);
        }
    }

    FreeVec(buf);

    /* Show in vrdb window */
    {
        struct Screen *scr = NULL;
        APTR           vi  = NULL;
        struct Gadget *glist = NULL;
        struct Window *vwin  = NULL;
        UWORD font_h, bor_t, bor_b, pad, row_h, btn_h, win_w, win_h, min_h, scr_w, scr_h;

        scr = LockPubScreen(NULL);
        if (!scr) return;
        vi = GetVisualInfoA(scr, NULL);
        if (!vi) goto rdr_cleanup;

        font_h = scr->Font->ta_YSize;
        bor_t  = scr->WBorTop + font_h + 1;
        bor_b  = scr->WBorBottom;
        pad    = 4;
        row_h  = font_h + 2;
        btn_h  = font_h + 6;
        win_w  = 520;
        win_h  = bor_t + pad + row_h * 20 + pad + btn_h + pad + bor_b;
        min_h  = bor_t + pad + row_h *  4 + pad + btn_h + pad + bor_b;
        scr_w  = scr->Width;
        scr_h  = scr->Height;

        if (!vrdb_make_gadgets(vi, scr, win_w, win_h, &glist))
            goto rdr_cleanup;

        { struct TagItem wt[] = {
              { WA_Left,      (ULONG)((scr_w - win_w) / 2) },
              { WA_Top,       (ULONG)((scr_h - win_h) / 2) },
              { WA_Width,     win_w }, { WA_Height, win_h },
              { WA_Title,     (ULONG)"Raw Disk Read" },
              { WA_Gadgets,   (ULONG)glist },
              { WA_PubScreen, (ULONG)scr },
              { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                              IDCMP_NEWSIZE|IDCMP_REFRESHWINDOW },
              { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                              WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH|
                              WFLG_SIZEGADGET|WFLG_SIZEBBOTTOM },
              { WA_MinWidth,  300 }, { WA_MinHeight, min_h },
              { WA_MaxWidth,  scr_w }, { WA_MaxHeight, scr_h },
              { TAG_DONE, 0 } };
          vwin = OpenWindowTagList(NULL, wt); }

        UnlockPubScreen(NULL, scr); scr = NULL;
        if (!vwin) goto rdr_cleanup;
        GT_RefreshWindow(vwin, NULL);

        { BOOL running = TRUE;
          while (running) {
            struct IntuiMessage *imsg;
            WaitPort(vwin->UserPort);
            while ((imsg = GT_GetIMsg(vwin->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_NEWSIZE: {
                    struct Gadget *ng2 = NULL;
                    RemoveGList(vwin, glist, -1); FreeGadgets(glist); glist = NULL;
                    if (vrdb_make_gadgets(vi, vwin->WScreen,
                                          (UWORD)vwin->Width, (UWORD)vwin->Height, &ng2)) {
                        glist = ng2;
                        AddGList(vwin, glist, ~0, -1, NULL);
                        RefreshGList(glist, vwin, NULL, -1);
                    }
                    GT_RefreshWindow(vwin, NULL); break; }
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == VRDB_DONE) running = FALSE; break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(vwin); GT_EndRefresh(vwin, TRUE); break;
                }
            }
          }
        }

rdr_cleanup:
        if (vwin)  { RemoveGList(vwin, glist, -1); CloseWindow(vwin); }
        if (glist)   FreeGadgets(glist);
        if (vi)      FreeVisualInfo(vi);
        if (scr)     UnlockPubScreen(NULL, scr);
    }
}

/* ------------------------------------------------------------------ */
/* diag_read_block — read one 512-byte block for diagnostic use only. */
/*                                                                     */
/* Tries HD_SCSICMD (SCSI READ(10)) first, falls back to CMD_READ for  */
/* devices that don't support HD_SCSICMD.                              */
/*                                                                     */
/* buf MUST be AllocVec'd with MEMF_PUBLIC.                            */
/*                                                                     */
/* Returns:                                                            */
/*   0  — success via HD_SCSICMD (SCSI path)                          */
/*   1  — success via CMD_READ fallback (non-SCSI or SCSI unavail.)   */
/*  -1  — both paths failed                                            */
/* ------------------------------------------------------------------ */

static int diag_read_block(struct BlockDev *bd, ULONG blknum, UBYTE *chipbuf)
{
    struct SCSICmd scmd;
    UBYTE  cdb[10];
    UBYTE  sense[16];
    BYTE   err;

    /* HD_SCSICMD: SCSI READ(10), LBA = blknum, transfer length = 1 block.
       scmd/cdb/sense are CPU-read only (not DMA'd), so stack (FAST RAM) is
       fine.  chipbuf is where the SDMAC puts the 512 data bytes — must be
       CHIP RAM, which the caller guarantees. */
    memset(&scmd,  0, sizeof(scmd));
    memset(cdb,    0, sizeof(cdb));
    memset(sense,  0, sizeof(sense));

    cdb[0] = 0x28;                         /* READ(10) operation code */
    cdb[2] = (UBYTE)(blknum >> 24);
    cdb[3] = (UBYTE)(blknum >> 16);
    cdb[4] = (UBYTE)(blknum >>  8);
    cdb[5] = (UBYTE)(blknum);
    cdb[8] = 1;                            /* transfer length: 1 block */

    scmd.scsi_Data        = (UWORD *)chipbuf;
    scmd.scsi_Length      = 512;
    scmd.scsi_Command     = cdb;
    scmd.scsi_CmdLength   = 10;
    scmd.scsi_Flags       = SCSIF_READ;
    scmd.scsi_SenseData   = sense;
    scmd.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

    if (err == 0)
        return 0;   /* HD_SCSICMD read succeeded */

    /* Fall back to CMD_READ: works on non-SCSI devices and as a reference
       read so the caller can compare SCSI vs CMD_READ output. */
    bd->iotd.iotd_Req.io_Command = CMD_READ;
    bd->iotd.iotd_Req.io_Length  = 512;
    bd->iotd.iotd_Req.io_Data    = (APTR)chipbuf;
    bd->iotd.iotd_Req.io_Offset  = blknum * 512UL;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);
    return (err == 0) ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/* raw_hex_dump — dump raw hex+ASCII of blocks 0..N-1                 */
/* Uses diag_read_block: HD_SCSICMD first, CMD_READ fallback.         */
/* Block header shows [SCSI] or [CMD] so you can see which path ran.  */
/* ------------------------------------------------------------------ */

#define DUMP_BLOCKS 8   /* number of blocks to dump */

static void raw_hex_dump(struct Window *win, struct BlockDev *bd)
{
    UBYTE *buf;
    ULONG  blk, i;
    int    rcode;
    char   line[80];
    struct EasyStruct es;

    if (!bd) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Hex Dump";
        es.es_TextFormat=(UBYTE*)"No device open.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    vrdb_count = 0;
    vrdb_list.lh_Head     = (struct Node *)&vrdb_list.lh_Tail;
    vrdb_list.lh_Tail     = NULL;
    vrdb_list.lh_TailPred = (struct Node *)&vrdb_list.lh_Head;

    for (blk = 0; blk < DUMP_BLOCKS; blk++) {
        ULONG id;
        char  idtxt[5];
        UWORD k;

        rcode = diag_read_block(bd, blk, buf);

        if (rcode < 0) {
            sprintf(line, "=== Block %lu: read error ===",
                    (unsigned long)blk);
            vrdb_add(line);
            continue;
        }

        /* Block header: ID tag + checksum + which read path succeeded */
        id = ((ULONG)buf[0]<<24)|((ULONG)buf[1]<<16)|
             ((ULONG)buf[2]<<8)|(ULONG)buf[3];
        for (k = 0; k < 4; k++) {
            char c = (char)buf[k];
            idtxt[k] = (c >= 0x20 && c <= 0x7E) ? c : '.';
        }
        idtxt[4] = '\0';
        {
            const ULONG *lp = (const ULONG *)buf;
            ULONG summed = lp[1];
            ULONG sum = 0, s;
            if (summed >= 2 && summed <= 128)
                for (s = 0; s < summed; s++) sum += lp[s];
            sprintf(line, "=== Block %lu  ID=%s(0x%08lX)  csum=%s  [%s] ===",
                    (unsigned long)blk, idtxt, id,
                    (summed >= 2 && summed <= 128 && sum == 0) ? "OK" : "BAD",
                    (rcode == 0) ? "SCSI" : "CMD");
        }
        vrdb_add(line);

        /* Hex + ASCII, 16 bytes per line */
        for (i = 0; i < 512; i += 16) {
            char hex[52], asc[18];
            UWORD h = 0, a = 0;
            for (k = 0; k < 16; k++) {
                UBYTE c = buf[i + k];
                hex[h++] = "0123456789ABCDEF"[c >> 4];
                hex[h++] = "0123456789ABCDEF"[c & 0xF];
                hex[h++] = ' ';
                if (k == 7) hex[h++] = ' ';
                asc[a++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
            }
            hex[h] = '\0'; asc[a] = '\0';
            sprintf(line, "%03lX: %s%s", (unsigned long)i, hex, asc);
            vrdb_add(line);
        }
        vrdb_add("");
    }

    FreeVec(buf);

    /* Open vrdb viewer window */
    {
        struct Screen  *scr   = NULL;
        APTR            vi    = NULL;
        struct Gadget  *glist = NULL;
        struct Window  *vwin  = NULL;
        UWORD font_h, bor_t, bor_b, row_h, btn_h, pad, win_w, win_h, min_h;
        UWORD scr_w, scr_h;

        scr = LockPubScreen(NULL);
        if (!scr) return;
        vi = GetVisualInfoA(scr, NULL);
        if (!vi) { UnlockPubScreen(NULL, scr); return; }

        font_h = scr->Font->ta_YSize;
        bor_t  = scr->WBorTop + font_h + 1;
        bor_b  = scr->WBorBottom;
        pad    = 4;
        row_h  = font_h + 2;
        btn_h  = font_h + 6;
        win_w  = 560;
        win_h  = bor_t + pad + row_h * 20 + pad + btn_h + pad + bor_b;
        min_h  = bor_t + pad + row_h *  4 + pad + btn_h + pad + bor_b;
        scr_w  = scr->Width;
        scr_h  = scr->Height;

        if (!vrdb_make_gadgets(vi, scr, win_w, win_h, &glist))
            goto hexdump_cleanup;

        { struct TagItem wt[] = {
              { WA_Left,      (ULONG)((scr_w - win_w) / 2) },
              { WA_Top,       (ULONG)((scr_h - win_h) / 2) },
              { WA_Width,     win_w }, { WA_Height, win_h },
              { WA_Title,     (ULONG)"Hex Dump - Raw Blocks (SCSI/CMD)" },
              { WA_Gadgets,   (ULONG)glist },
              { WA_PubScreen, (ULONG)scr },
              { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                              IDCMP_GADGETDOWN|IDCMP_REFRESHWINDOW|IDCMP_NEWSIZE },
              { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                              WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH|
                              WFLG_SIZEGADGET|WFLG_SIZEBBOTTOM },
              { WA_MinWidth,  300 },
              { WA_MinHeight, min_h },
              { WA_MaxWidth,  scr_w },
              { WA_MaxHeight, scr_h },
              { TAG_DONE, 0 } };
          vwin = OpenWindowTagList(NULL, wt); }

        UnlockPubScreen(NULL, scr); scr = NULL;
        if (!vwin) goto hexdump_cleanup;
        GT_RefreshWindow(vwin, NULL);

        {
            BOOL running = TRUE;
            while (running) {
                struct IntuiMessage *imsg;
                WaitPort(vwin->UserPort);
                while ((imsg = GT_GetIMsg(vwin->UserPort)) != NULL) {
                    ULONG iclass = imsg->Class;
                    struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                    GT_ReplyIMsg(imsg);
                    switch (iclass) {
                    case IDCMP_CLOSEWINDOW: running = FALSE; break;
                    case IDCMP_NEWSIZE: {
                        struct Gadget *ng2 = NULL;
                        RemoveGList(vwin, glist, -1);
                        FreeGadgets(glist); glist = NULL;
                        if (vrdb_make_gadgets(vi, vwin->WScreen,
                                              (UWORD)vwin->Width,
                                              (UWORD)vwin->Height, &ng2)) {
                            glist = ng2;
                            AddGList(vwin, glist, ~0, -1, NULL);
                            RefreshGList(glist, vwin, NULL, -1);
                        }
                        GT_RefreshWindow(vwin, NULL);
                        break; }
                    case IDCMP_GADGETUP:
                        if (gad->GadgetID == VRDB_DONE) running = FALSE;
                        break;
                    case IDCMP_REFRESHWINDOW:
                        GT_BeginRefresh(vwin); GT_EndRefresh(vwin, TRUE); break;
                    }
                }
            }
        }

hexdump_cleanup:
        if (vwin)  { RemoveGList(vwin, glist, -1); CloseWindow(vwin); }
        if (glist)   FreeGadgets(glist);
        if (vi)      FreeVisualInfo(vi);
        if (scr)     UnlockPubScreen(NULL, scr);
    }
}

/* ------------------------------------------------------------------ */

static void show_about(struct Window *win)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"About " DISKPART_VERTITLE;
    es.es_TextFormat   = (UBYTE *)
        "DiskPart\n"
        "AmigaOS 3.x RDB Hard Disk Partition Editor\n"
        "\n"
        "A native GadTools application with full RDB support.\n"
        "No external library dependencies beyond the ROM.\n"
        "\n"
        "Director: John Hertell\n"
        "Code: Claude Code (Anthropic)\n"
        "\n"
        "https://github.com/ChuckyGang/DiskPart\n"
        "\n"
        "MIT License \xa9 2026 John Hertell";
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(win, &es, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* geometry_dialog — manual disk geometry entry                        */
/* Used when TD_GETGEOMETRY fails or user wants to override.           */
/* Returns TRUE (OK with valid values) or FALSE (cancelled).           */
/* ------------------------------------------------------------------ */

#define GDLG_CYLS    1
#define GDLG_HEADS   2
#define GDLG_SECS    3
#define GDLG_OK      4
#define GDLG_CANCEL  5
#define GDLG_ROWS    3

static BOOL geometry_dialog(ULONG def_cyls, ULONG def_heads, ULONG def_secs,
                            ULONG *out_cyls, ULONG *out_heads, ULONG *out_secs)
{
    struct Screen  *scr       = NULL;
    APTR            vi        = NULL;
    struct Gadget  *glist     = NULL;
    struct Gadget  *gctx      = NULL;
    struct Gadget  *cyls_gad  = NULL;
    struct Gadget  *heads_gad = NULL;
    struct Gadget  *secs_gad  = NULL;
    struct Window  *win       = NULL;
    BOOL            result    = FALSE;
    UWORD           warn_y    = 0;
    UWORD           warn_fh   = 8;
    char  cyls_str[12], heads_str[12], secs_str[12];

    if (def_cyls  == 0) def_cyls  = 1;
    if (def_heads == 0) def_heads = 1;
    if (def_secs  == 0) def_secs  = 1;

    sprintf(cyls_str,  "%lu", (unsigned long)def_cyls);
    sprintf(heads_str, "%lu", (unsigned long)def_heads);
    sprintf(secs_str,  "%lu", (unsigned long)def_secs);

    scr = LockPubScreen(NULL);
    if (!scr) goto geom_cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto geom_cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 380;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 3;
        UWORD row_h   = font_h + 4;
        UWORD lbl_w   = 110;
        UWORD gad_x   = bor_l + lbl_w;
        UWORD gad_w   = inner_w - lbl_w - pad;
        UWORD warn_rh = (UWORD)(font_h + 3);
        UWORD gad_top = bor_t + pad + warn_rh * 2 + pad * 2;
        UWORD win_h   = gad_top
                      + (UWORD)GDLG_ROWS * (row_h + pad)
                      + row_h + pad + bor_b;

        warn_y  = bor_t + pad;
        warn_fh = font_h;

        gctx = CreateContext(&glist);
        if (!gctx) goto geom_cleanup;

        {
            struct NewGadget ng;
            struct Gadget *prev = gctx;
            UWORD row = 0;
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

#define GROW_Y(r) ((WORD)(gad_top + (r) * (row_h + pad)))
#define GSTR_GAD(gid, lbl, istr, pgad) \
    ng.ng_LeftEdge=(WORD)gad_x; ng.ng_TopEdge=GROW_Y(row); \
    ng.ng_Width=(WORD)gad_w; ng.ng_Height=(WORD)row_h; \
    ng.ng_GadgetText=(lbl); ng.ng_GadgetID=(gid); ng.ng_Flags=PLACETEXT_LEFT; \
    { struct TagItem _gs[]={{GTST_String,(ULONG)(istr)}, \
                            {GTST_MaxChars,10},{TAG_DONE,0}}; \
      *(pgad)=CreateGadgetA(STRING_KIND,prev,&ng,_gs); \
      if (!*(pgad)) goto geom_cleanup; prev=*(pgad); } row++;

            GSTR_GAD(GDLG_CYLS,  "Cylinders",   cyls_str,  &cyls_gad)
            GSTR_GAD(GDLG_HEADS, "Heads",        heads_str, &heads_gad)
            GSTR_GAD(GDLG_SECS,  "Sectors/Trk", secs_str,  &secs_gad)

#undef GSTR_GAD
#undef GROW_Y

            {
                UWORD btn_y  = gad_top + (UWORD)GDLG_ROWS * (row_h + pad);
                UWORD half_w = (inner_w - pad * 2 - pad) / 2;
                struct TagItem bt[] = { { TAG_DONE, 0 } };
                ng.ng_TopEdge=(WORD)btn_y; ng.ng_Height=(WORD)row_h;
                ng.ng_Width=(WORD)half_w; ng.ng_Flags=PLACETEXT_IN;
                ng.ng_LeftEdge=(WORD)(bor_l+pad); ng.ng_GadgetText="OK";
                ng.ng_GadgetID=GDLG_OK;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
                if (!prev) goto geom_cleanup;
                ng.ng_LeftEdge=(WORD)(bor_l+pad+half_w+pad);
                ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=GDLG_CANCEL;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
                if (!prev) goto geom_cleanup;
            }
        }

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width,     (ULONG)win_w },
                { WA_Height,    (ULONG)win_h },
                { WA_Title,     (ULONG)"Manual Geometry Entry" },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto geom_cleanup;
    GT_RefreshWindow(win, NULL);

    /* Draw 2-line warning */
#define GEOM_WARN(win_, y_, fh_, str_) do { \
    struct RastPort *_rp = (win_)->RPort; \
    SetAPen(_rp, 1); \
    Move(_rp, (WORD)((win_)->BorderLeft + 4), \
              (WORD)((y_) + _rp->TxBaseline)); \
    Text(_rp, (str_), (WORD)strlen(str_)); \
} while(0)
    GEOM_WARN(win, warn_y,            warn_fh, "WARNING: Incorrect values may cause data loss.");
    GEOM_WARN(win, warn_y+warn_fh+3,  warn_fh, "Use only when automatic detection fails.");

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW:
                    running = FALSE;
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GDLG_OK: {
                        struct StringInfo *si;
                        ULONG c, h, s;
                        si = (struct StringInfo *)cyls_gad->SpecialInfo;
                        c  = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)heads_gad->SpecialInfo;
                        h  = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)secs_gad->SpecialInfo;
                        s  = parse_num((char *)si->Buffer);
                        if (c > 0 && h > 0 && s > 0) {
                            *out_cyls  = c;
                            *out_heads = h;
                            *out_secs  = s;
                            result = TRUE;
                        }
                        running = FALSE;
                        break;
                    }
                    case GDLG_CANCEL:
                        running = FALSE;
                        break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    GEOM_WARN(win, warn_y,           warn_fh, "WARNING: Incorrect values may cause data loss.");
                    GEOM_WARN(win, warn_y+warn_fh+3, warn_fh, "Use only when automatic detection fails.");
                    break;
                }
            }
        }
    }
#undef GEOM_WARN

geom_cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return result;
}

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
static void check_ffs_root(struct Window *win, struct BlockDev *bd,
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

/* ------------------------------------------------------------------ */
/* Offer to grow an FFS/OFS filesystem after a partition was extended.  */
/* Called from all three "edit partition" code paths.                   */
/* ------------------------------------------------------------------ */
static void ffs_grow_progress(void *ud, const char *msg)
{
    struct Window *pw = (struct Window *)ud;
    struct RastPort *rp;
    WORD x1, y1, x2, y2;
    UWORD len;

    if (!pw) return;
    rp = pw->RPort;
    x1 = pw->BorderLeft;
    y1 = pw->BorderTop;
    x2 = (WORD)(pw->Width  - 1 - pw->BorderRight);
    y2 = (WORD)(pw->Height - 1 - pw->BorderBottom);
    SetAPen(rp, 0);
    RectFill(rp, x1, y1, x2, y2);
    SetAPen(rp, 1);
    Move(rp, (WORD)(x1 + 6), (WORD)(y1 + 4 + rp->TxBaseline));
    for (len = 0; msg[len]; len++) {}
    Text(rp, (STRPTR)msg, (WORD)len);
}

static void offer_ffs_grow(struct Window *win, struct BlockDev *bd,
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
        /* Open a small progress window so the user can see what is happening.
           The grow operation is synchronous; the progress window shows the
           current phase in its interior via the ffs_grow_progress callback. */
        struct Screen *scr = win->WScreen;
        UWORD font_h = scr->Font ? scr->Font->ta_YSize : 8;
        UWORD bor_t  = (UWORD)(scr->WBorTop + font_h + 1);
        UWORD pw_w   = 360;
        UWORD pw_h   = (UWORD)(bor_t + scr->WBorBottom + font_h + 10);
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

        BOOL result = FFS_GrowPartition(bd, rdb, pi, old_hi, errbuf,
                                        ffs_grow_progress, prog_win);
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

                                drag_part    = part;
                                drag_edge    = edge;
                                drag_orig_lo = rdb->parts[part].low_cyl;
                                drag_orig_hi = rdb->parts[part].high_cyl;
                                dbl_part     = -1;
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
                                                dirty = TRUE;
                                                refresh_listview(win, lv_gad, rdb, sel);
                                                draw_static(win, devname, unit, rdb, (bd ? bd->disk_brand : ""),
                                                            ix, iy, iw, bx, by, bw, bh,
                                                            hx, hy, hw, sel, lastdisk_gad, lastlun_gad);
                                            }
                                        }
                                    } else {
                                        /* Single click: select partition */
                                        sel      = blk;
                                        dbl_part = blk;
                                        dbl_sec  = ev_sec;
                                        dbl_mic  = ev_mic;
                                        refresh_listview(win, lv_gad, rdb, sel);
                                        draw_map(win, rdb, sel, bx, by, bw, bh);
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
