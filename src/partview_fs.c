/*
 * partview_fs.c — Filesystem manager dialog for DiskPart.
 *
 * Contains: build_fs_list, fs_load_file, fs_addedit_dialog,
 *           filesystem_manager_dialog.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <libraries/asl.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "partview_internal.h"

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct Library       *AslBase;
extern struct IntuitionBase *IntuitionBase;
extern struct Library       *GadToolsBase;

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
BOOL filesystem_manager_dialog(struct RDBInfo *rdb)
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
