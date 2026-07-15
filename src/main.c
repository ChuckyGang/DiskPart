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
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
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
#include "diskselect.h"
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

/* Gadget IDs for image-size dialog */
#define GID_SZ_STR  10
#define GID_SZ_OK   11
#define GID_SZ_CANC 12

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

    DP_SNPRINTF(prompt, GS(MSG_MAIN_IMG_NOEXIST_FMT), path);

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
        DP_SNPRINTF(body, GS(MSG_MAIN_IMG_CREATE_FAIL_FMT), path);
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
            DP_SNPRINTF(body, GS(MSG_MAIN_WARN_BODY),
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
        while ((name_idx = DiskSelect_PickDeviceName(&dev_names, manual_devname, TRUE)) != -1 &&
               name_idx != DISKSEL_EXIT) {
            const char *devname;
            WORD unit_idx;
            BOOL quit = FALSE;

            /* Image-file backend - skip device probe and unit selection;
             * after the editor closes, return to the device-selection window. */
            if (name_idx == DISKSEL_IMAGE) {
                if (!pick_image_path(image_path, sizeof(image_path)))
                    continue;
                if (!prepare_image(image_path))
                    continue;
                DP_SNPRINTF(image_devname, "FILE:%s", image_path);
                if (partview_run(image_devname, 0))
                    break;
                continue;
            }

            devname = (name_idx == DISKSEL_MANUAL)
                      ? manual_devname
                      : dev_names.names[name_idx];

            if (!DiskSelect_ProbeUnits(devname, &unit_list)) continue;

            while (!quit && (unit_idx = DiskSelect_PickUnit(devname, &unit_list)) >= 0) {
                if (partview_run(devname, unit_list.entries[unit_idx].unit))
                    quit = TRUE;
            }
            if (quit || unit_idx == DISKSEL_EXIT) break;
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
