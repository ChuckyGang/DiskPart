/*
 * partview_dialogs.c — Partition and geometry dialogs for DiskPart.
 *
 * Contains:
 *   FriendlyDosType, parse helpers (parse_num/long/dostype),
 *   partition_advanced_dialog, partition_dialog,
 *   show_about, geometry_dialog.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "version.h"
#include "partview_internal.h"

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct IntuitionBase *IntuitionBase;
extern struct Library       *GadToolsBase;

static const struct { const char *name; ULONG dostype; } builtin_fs[] = {
    { "OFS",      0x444F5300UL },
    { "FFS",      0x444F5301UL },
    { "FFS+Intl", 0x444F5303UL },
};
#define NUM_BUILTIN_FS 3

/* Returns a human-readable filesystem name: built-in friendly name if known,
   otherwise falls back to FormatDosType (e.g. "PFS\3").  buf >= 16 bytes. */
void FriendlyDosType(ULONG dostype, char *buf)
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
ULONG parse_num(const char *s)
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
LONG parse_long(const char *s)
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
ULONG parse_dostype(const char *s)
{
    ULONG val = 0;
    UBYTE bytes[4];
    UBYTE i, nb;

    while (*s == ' ') s++;

    /* Hex: 0xNNNNNNNN */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_num(s);

    /*
     * Quoted: "PFS3" — take bytes literally (ASCII '3' = 0x33).
     * Useful when you actually want the ASCII digit in the dostype.
     */
    if (s[0] == '"') {
        s++;
        nb = 0;
        while (*s && *s != '"' && nb < 4)
            bytes[nb++] = (UBYTE)*s++;
        for (i = nb; i < 4; i++) bytes[i] = 0;
        return ((ULONG)bytes[0] << 24) | ((ULONG)bytes[1] << 16) |
               ((ULONG)bytes[2] <<  8) |  (ULONG)bytes[3];
    }

    /*
     * Short name with trailing digit: PFS3, DOS3, SFS0 etc.
     * If the string is exactly 4 chars and the last is 0-9, treat
     * the digit as a binary byte (PFS3 → 0x50465303, not 0x50465333).
     * This matches what PFS\3 / DOS\3 notation means.
     * Use quotes ("PFS3") to force literal ASCII bytes instead.
     */
    if (s[0] && s[1] && s[2] &&
        s[3] >= '0' && s[3] <= '9' && s[4] == '\0') {
        return ((ULONG)(UBYTE)s[0] << 24) |
               ((ULONG)(UBYTE)s[1] << 16) |
               ((ULONG)(UBYTE)s[2] <<  8) |
                (ULONG)(s[3] - '0');
    }

    /* General string encoding: up to 4 chars, \N escape for binary byte */
    nb = 0;
    while (*s && nb < 4) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '9') {
            bytes[nb++] = (UBYTE)(s[1] - '0');
            s += 2;
        } else {
            bytes[nb++] = (UBYTE)*s++;
        }
    }
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

void partition_advanced_dialog(struct PartInfo *pi)
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

BOOL partition_dialog(struct PartInfo *pi, const char *title,
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

void show_about(struct Window *win)
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

BOOL geometry_dialog(ULONG def_cyls, ULONG def_heads, ULONG def_secs,
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

