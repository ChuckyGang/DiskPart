/*
 * partview_rdb.c — RDB backup/restore/view and disk diagnostic tools.
 *
 * Contains: rdb_backup_block, rdb_restore_block, rdb_backup_extended,
 *           rdb_restore_extended, rdb_view_block, rdb_raw_scan,
 *           raw_disk_read, diag_read_block, raw_hex_dump.
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
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
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
#include "devices.h"
#include "version.h"
#include "partview_internal.h"

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct Library       *AslBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *GadToolsBase;

void rdb_backup_block(struct Window *win, struct BlockDev *bd,
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

void rdb_restore_block(struct Window *win, struct BlockDev *bd)
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

void rdb_backup_extended(struct Window *win, struct BlockDev *bd,
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

void rdb_restore_extended(struct Window *win, struct BlockDev *bd)
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

void rdb_view_block(struct Window *win, struct BlockDev *bd,
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

void rdb_raw_scan(struct Window *win, struct BlockDev *bd)
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

void raw_disk_read(struct Window *win, struct BlockDev *bd)
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

void raw_hex_dump(struct Window *win, struct BlockDev *bd)
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
/* SMART attribute name lookup table                                   */
/* ------------------------------------------------------------------ */

static const struct { UBYTE id; const char *name; } smart_names[] = {
    {   1, "Raw Read Error Rate"    },
    {   3, "Spin Up Time"           },
    {   4, "Start/Stop Count"       },
    {   5, "Reallocated Sector Ct"  },
    {   7, "Seek Error Rate"        },
    {   9, "Power On Hours"         },
    {  10, "Spin Retry Count"       },
    {  11, "Calibration Retry Ct"   },
    {  12, "Power Cycle Count"      },
    { 177, "Wear Leveling Count"    },
    { 179, "Used Reserved Blk Ct"   },
    { 181, "Program Fail Ct Total"  },
    { 182, "Erase Fail Ct Total"    },
    { 183, "Runtime Bad Block"      },
    { 187, "Reported Incorrect"     },
    { 188, "Command Timeout"        },
    { 189, "High Fly Writes"        },
    { 190, "Airflow Temperature"    },
    { 191, "G-Sense Error Rate"     },
    { 192, "Power-Off Retract Ct"   },
    { 193, "Load/Unload Cycle Ct"   },
    { 194, "Temperature Celsius"    },
    { 196, "Reallocation Event Ct"  },
    { 197, "Current Pending Sector" },
    { 198, "Offline Uncorrectable"  },
    { 199, "UDMA CRC Error Count"   },
    { 200, "Multi Zone Error Rate"  },
    { 241, "Total LBAs Written"     },
    { 242, "Total LBAs Read"        },
    {   0, NULL                     }
};

static const char *smart_attr_name(UBYTE id)
{
    int i;
    for (i = 0; smart_names[i].name; i++)
        if (smart_names[i].id == id) return smart_names[i].name;
    return "Unknown Attribute";
}

/* ------------------------------------------------------------------ */
/* smart_send — issue one SMART sub-command via ATA PASS-THROUGH.     */
/*                                                                     */
/* feature : ATA FEATURES register value:                             */
/*   0xD0 = READ DATA, 0xD1 = READ THRESHOLDS, 0xD8 = ENABLE OPS     */
/* buf     : 512-byte MEMF_PUBLIC buffer for data-in commands, or     */
/*           NULL for non-data commands (e.g. 0xD8 ENABLE).           */
/*                                                                     */
/* Returns: 0  = ok via 16-byte ATA PASS-THROUGH(16) CDB             */
/*          1  = ok via 12-byte ATA PASS-THROUGH(12) fallback         */
/*         -1  = command failed on both CDB lengths                   */
/*         -2  = HD_SCSICMD not supported by driver                   */
/* ------------------------------------------------------------------ */

static int smart_send(struct BlockDev *bd, UBYTE feature, UBYTE *buf)
{
    struct SCSICmd scmd;
    UBYTE          cdb[16];
    UBYTE          sense[32];
    BYTE           err;
    BOOL           has_data = (BOOL)(buf != NULL);

    memset(&scmd, 0, sizeof(scmd));
    memset(cdb,   0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));

    /* ATA PASS-THROUGH(16), SAT-2 encoding.
       cdb[1]: PROTOCOL field (bits 4:1): 4=PIO Data-In, 3=Non-data.
       cdb[2]: T_DIR(bit3)|BYTE_BLOCK(bit2)|T_LENGTH(bits1:0)=2 for data.  */
    cdb[0]  = 0x85;
    cdb[1]  = has_data ? 0x08 : 0x06;   /* PIO-In(4<<1) or Non-data(3<<1) */
    cdb[2]  = has_data ? 0x0E : 0x00;   /* T_DIR|BYTE_BLOCK|T_LENGTH=2    */
    cdb[4]  = feature;                   /* ATA FEATURES register           */
    cdb[6]  = has_data ? 1 : 0;         /* ATA SECTOR COUNT = 1 block      */
    cdb[10] = 0x4F;                      /* ATA LBA MID — SMART signature   */
    cdb[12] = 0xC2;                      /* ATA LBA HIGH — SMART signature  */
    cdb[14] = 0xB0;                      /* ATA COMMAND = SMART             */

    scmd.scsi_Data        = has_data ? (UWORD *)buf : NULL;
    scmd.scsi_Length      = has_data ? 512 : 0;
    scmd.scsi_Command     = cdb;
    scmd.scsi_CmdLength   = 16;
    scmd.scsi_Flags       = has_data ? SCSIF_READ : 0;
    scmd.scsi_SenseData   = sense;
    scmd.scsi_SenseLength = sizeof(sense);

    bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
    bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
    bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
    bd->iotd.iotd_Req.io_Flags   = 0;
    bd->iotd.iotd_Count          = 0;
    err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

    if (err == 0)           return 0;
    if (err == IOERR_NOCMD) return -2;

    /* Fall back to ATA PASS-THROUGH(12): same semantics, shorter CDB. */
    {
        UBYTE cdb12[12];
        memset(&scmd,  0, sizeof(scmd));
        memset(cdb12,  0, sizeof(cdb12));
        memset(sense,  0, sizeof(sense));

        cdb12[0] = 0xA1;
        cdb12[1] = has_data ? 0x08 : 0x06;
        cdb12[2] = has_data ? 0x0E : 0x00;
        cdb12[3] = feature;
        cdb12[4] = has_data ? 1 : 0;
        cdb12[6] = 0x4F;
        cdb12[7] = 0xC2;
        cdb12[9] = 0xB0;

        scmd.scsi_Data        = has_data ? (UWORD *)buf : NULL;
        scmd.scsi_Length      = has_data ? 512 : 0;
        scmd.scsi_Command     = cdb12;
        scmd.scsi_CmdLength   = 12;
        scmd.scsi_Flags       = has_data ? SCSIF_READ : 0;
        scmd.scsi_SenseData   = sense;
        scmd.scsi_SenseLength = sizeof(sense);

        bd->iotd.iotd_Req.io_Command = HD_SCSICMD;
        bd->iotd.iotd_Req.io_Length  = sizeof(scmd);
        bd->iotd.iotd_Req.io_Data    = (APTR)&scmd;
        bd->iotd.iotd_Req.io_Flags   = 0;
        bd->iotd.iotd_Count          = 0;
        err = (BYTE)DoIO((struct IORequest *)&bd->iotd);

        if (err == 0) return 1;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* smart_status — display ATA SMART health data for the open device.  */
/* ------------------------------------------------------------------ */

void smart_status(struct Window *win, struct BlockDev *bd)
{
    struct EasyStruct  es;
    UBYTE             *data_buf = NULL;
    UBYTE             *thr_buf  = NULL;
    char               line[80];
    int                rc;
    BOOL               thr_ok   = FALSE;
    BOOL               any_fail = FALSE;
    WORD               i, j;

    es.es_StructSize = sizeof(es);
    es.es_Flags      = 0;
    es.es_Title      = (UBYTE *)"SMART Status";

    if (!bd) {
        es.es_TextFormat   = (UBYTE *)"No device open.";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    /* Buffers for DMA — MEMF_PUBLIC required for hardware DMA access. */
    data_buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    thr_buf  = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!data_buf || !thr_buf) {
        if (data_buf) FreeVec(data_buf);
        if (thr_buf)  FreeVec(thr_buf);
        return;
    }

    vrdb_count = 0;
    vrdb_list.lh_Head     = (struct Node *)&vrdb_list.lh_Tail;
    vrdb_list.lh_Tail     = NULL;
    vrdb_list.lh_TailPred = (struct Node *)&vrdb_list.lh_Head;

    vrdb_add("=== SMART Status ===");
    if (bd->disk_brand[0]) {
        sprintf(line, "  Drive: %s", bd->disk_brand);
        vrdb_add(line);
    }
    vrdb_add("");

    smart_send(bd, 0xD8, NULL);          /* SMART ENABLE OPERATIONS (best effort) */

    rc = smart_send(bd, 0xD0, data_buf); /* SMART READ DATA */

    if (rc == -2) {
        vrdb_add("  ATA SMART not supported:");
        vrdb_add("  Device or driver does not implement HD_SCSICMD /");
        vrdb_add("  ATA PASS-THROUGH.");
        goto smart_show;
    }
    if (rc < 0) {
        vrdb_add("  SMART READ DATA command failed.");
        vrdb_add("  Drive may not support SMART, or the driver does not");
        vrdb_add("  support ATA PASS-THROUGH CDBs.");
        goto smart_show;
    }
    if (data_buf[0] == 0 && data_buf[1] == 0) {
        vrdb_add("  SMART returned all-zero data.");
        vrdb_add("  Drive may not support SMART.");
        goto smart_show;
    }

    sprintf(line, "  Data structure revision: 0x%02X%02X",
            (unsigned)data_buf[1], (unsigned)data_buf[0]);
    vrdb_add(line);

    thr_ok = (BOOL)(smart_send(bd, 0xD1, thr_buf) >= 0); /* READ THRESHOLDS */

    /* Pass 1: determine overall health from pre-failure attributes.
       An attribute fails when: pre-failure flag set AND current < threshold. */
    for (i = 0; i < 30; i++) {
        UBYTE *attr    = data_buf + 2 + i * 12;
        UBYTE  id      = attr[0];
        UBYTE  cur     = attr[3];
        UBYTE  thr_val = 0;
        UWORD  flags;

        if (id == 0) continue;
        flags = (UWORD)attr[1] | ((UWORD)attr[2] << 8);
        if (thr_ok) {
            for (j = 0; j < 30; j++) {
                if (thr_buf[2 + j*12] == id) {
                    thr_val = thr_buf[2 + j*12 + 3];
                    break;
                }
            }
        }
        if (thr_val > 0 && cur < thr_val && (flags & 0x0001))
            any_fail = TRUE;
    }

    if (any_fail)
        vrdb_add("  Overall health: ** FAILED ** (pre-failure threshold exceeded)");
    else if (thr_ok)
        vrdb_add("  Overall health: PASSED");
    else
        vrdb_add("  Overall health: UNKNOWN (threshold data unavailable)");
    vrdb_add("");
    vrdb_add("   ID Name                    Cur  Wst  Thr       Raw  Status");
    vrdb_add("  ---+------------------------+----+----+----+-----------+------");

    /* Pass 2: display each attribute. */
    for (i = 0; i < 30; i++) {
        UBYTE *attr    = data_buf + 2 + i * 12;
        UBYTE  id      = attr[0];
        UBYTE  cur     = attr[3];
        UBYTE  wst     = attr[4];
        UBYTE  thr_val = 0;
        ULONG  raw32;
        const char *status;

        if (id == 0) continue;

        /* Raw value: 6 bytes little-endian; show lower 32 bits. */
        raw32 = (ULONG)attr[5]
              | ((ULONG)attr[6] << 8)
              | ((ULONG)attr[7] << 16)
              | ((ULONG)attr[8] << 24);

        if (thr_ok) {
            for (j = 0; j < 30; j++) {
                if (thr_buf[2 + j*12] == id) {
                    thr_val = thr_buf[2 + j*12 + 3];
                    break;
                }
            }
        }

        if      (!thr_ok || thr_val == 0) status = "----";
        else if (cur < thr_val)           status = "FAIL";
        else                              status = "OK";

        sprintf(line, "  %3d %-22s  %3d  %3d  %3d  %10lu  %s",
                (int)id, smart_attr_name(id),
                (int)cur, (int)wst, (int)thr_val,
                (unsigned long)raw32, status);
        vrdb_add(line);
    }

smart_show:
    FreeVec(data_buf); data_buf = NULL;
    FreeVec(thr_buf);  thr_buf  = NULL;

    {
        struct Screen *scr   = NULL;
        APTR           vi    = NULL;
        struct Gadget *glist = NULL;
        struct Window *vwin  = NULL;
        UWORD font_h, bor_t, bor_b, pad, row_h, btn_h, win_w, win_h, min_h, scr_w, scr_h;

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
        win_h  = bor_t + pad + row_h * 22 + pad + btn_h + pad + bor_b;
        min_h  = bor_t + pad + row_h *  5 + pad + btn_h + pad + bor_b;
        scr_w  = scr->Width;
        scr_h  = scr->Height;

        if (!vrdb_make_gadgets(vi, scr, win_w, win_h, &glist))
            goto smart_win_cleanup;

        { struct TagItem wt[] = {
              { WA_Left,      (ULONG)((scr_w - win_w) / 2) },
              { WA_Top,       (ULONG)((scr_h - win_h) / 2) },
              { WA_Width,     win_w }, { WA_Height,    win_h },
              { WA_Title,     (ULONG)"SMART Status" },
              { WA_Gadgets,   (ULONG)glist },
              { WA_PubScreen, (ULONG)scr },
              { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                              IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW },
              { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                              WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH |
                              WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM },
              { WA_MinWidth,  300 }, { WA_MinHeight, min_h },
              { WA_MaxWidth,  scr_w }, { WA_MaxHeight, scr_h },
              { TAG_DONE, 0 } };
          vwin = OpenWindowTagList(NULL, wt); }

        UnlockPubScreen(NULL, scr); scr = NULL;
        if (!vwin) goto smart_win_cleanup;
        GT_RefreshWindow(vwin, NULL);

        { BOOL running = TRUE;
          while (running) {
              struct IntuiMessage *imsg;
              WaitPort(vwin->UserPort);
              while ((imsg = GT_GetIMsg(vwin->UserPort)) != NULL) {
                  ULONG  iclass = imsg->Class;
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
                  case IDCMP_GADGETUP:
                      if (gad->GadgetID == VRDB_DONE) running = FALSE; break;
                  case IDCMP_REFRESHWINDOW:
                      GT_BeginRefresh(vwin); GT_EndRefresh(vwin, TRUE); break;
                  }
              }
          }
        }

smart_win_cleanup:
        if (vwin)  { RemoveGList(vwin, glist, -1); CloseWindow(vwin); }
        if (glist)   FreeGadgets(glist);
        if (vi)      FreeVisualInfo(vi);
        if (scr)     UnlockPubScreen(NULL, scr);
    }
}

/* ------------------------------------------------------------------ */
/* write_badb — write a BADB chain to the RDB reserved area and patch */
/* the RDSK header to point BadBlockList at it.                       */
/*                                                                     */
/* bbe_GoodBlock is set to RDB_END_MARK (no replacement block) for    */
/* each entry — marks the sector as unrecoverable.                    */
/* ------------------------------------------------------------------ */

static BOOL write_badb(struct Window *win, struct BlockDev *bd,
                       const struct RDBInfo *rdb,
                       const ULONG *bad_blocks, ULONG bad_count)
{
    UBYTE                *buf          = NULL;
    ULONG                 badb_start;
    ULONG                 num_badb_blks;
    ULONG                 entries_per  = 61;   /* (512 - 24) / 8 */
    ULONG                 b, i, idx, sum;
    struct BadBlockBlock  *badb;
    struct RigidDiskBlock *rdsk;
    struct EasyStruct     es;
    BOOL                  ok = FALSE;

    es.es_StructSize = sizeof(es);
    es.es_Flags      = 0;
    es.es_Title      = (UBYTE *)"Bad Block List";

    badb_start    = rdb->rdb_block_hi + 1;
    num_badb_blks = (bad_count == 0) ? 1 :
                    (bad_count + entries_per - 1) / entries_per;

    /* Ensure BADB blocks don't overlap the first partition cylinder. */
    if (rdb->lo_cyl > 0 && rdb->heads > 0 && rdb->sectors > 0) {
        ULONG first_part_blk = rdb->lo_cyl * rdb->heads * rdb->sectors;
        if (badb_start + num_badb_blks > first_part_blk) {
            es.es_TextFormat   = (UBYTE *)"Not enough space in the RDB\nreserved area for a bad block list.";
            es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &es, NULL);
            return FALSE;
        }
    }

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return FALSE;

    /* Write each BADB block */
    for (b = 0; b < num_badb_blks; b++) {
        ULONG this_blk    = badb_start + b;
        ULONG next_blk    = (b + 1 < num_badb_blks) ?
                             badb_start + b + 1 : RDB_END_MARK;
        ULONG entry_start = b * entries_per;
        ULONG entry_end   = entry_start + entries_per;
        if (entry_end > bad_count) entry_end = bad_count;

        memset(buf, 0, 512);
        badb = (struct BadBlockBlock *)buf;
        badb->bbb_ID          = IDNAME_BADBLOCK;
        badb->bbb_SummedLongs = 512 / 4;   /* 128 longs */
        badb->bbb_ChkSum      = 0;
        badb->bbb_HostID      = 7;
        badb->bbb_Next        = next_blk;
        badb->bbb_Reserved    = 0;

        for (i = entry_start; i < entry_end; i++) {
            badb->bbb_BlockPairs[i - entry_start].bbe_BadBlock  = bad_blocks[i];
            badb->bbb_BlockPairs[i - entry_start].bbe_GoodBlock = RDB_END_MARK;
        }

        /* Checksum: negate sum of all 128 longs. */
        sum = 0;
        for (idx = 0; idx < 128; idx++) sum += ((ULONG *)buf)[idx];
        badb->bbb_ChkSum = (LONG)(-(LONG)sum);

        if (!BlockDev_WriteBlock(bd, this_blk, buf)) {
            es.es_TextFormat   = (UBYTE *)"Failed to write BADB block to disk.";
            es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &es, NULL);
            goto badb_done;
        }
    }

    /* Patch RDSK: update BadBlockList and extend RDBBlocksHi. */
    if (!BlockDev_ReadBlock(bd, rdb->block_num, buf)) {
        es.es_TextFormat   = (UBYTE *)"Failed to read RDSK block for update.";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(win, &es, NULL);
        goto badb_done;
    }
    rdsk = (struct RigidDiskBlock *)buf;
    rdsk->rdb_BadBlockList  = badb_start;
    rdsk->rdb_RDBBlocksHi   = badb_start + num_badb_blks - 1;
    rdsk->rdb_HighRDSKBlock = rdsk->rdb_RDBBlocksHi;
    rdsk->rdb_ChkSum        = 0;
    sum = 0;
    for (idx = 0; idx < rdsk->rdb_SummedLongs; idx++)
        sum += ((ULONG *)buf)[idx];
    rdsk->rdb_ChkSum = (LONG)(-(LONG)sum);

    if (!BlockDev_WriteBlock(bd, rdb->block_num, buf)) {
        es.es_TextFormat   = (UBYTE *)"Failed to write updated RDSK block.";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(win, &es, NULL);
        goto badb_done;
    }

    ok = TRUE;

badb_done:
    FreeVec(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* bad_block_scan — scan every block on the disk for read failures.   */
/* Shows a cancellable progress window during the scan, then displays */
/* results in a scrollable list.  If bad blocks are found and an RDB  */
/* exists, offers to write a BADB chain.                              */
/* ------------------------------------------------------------------ */

#define BBSCAN_CANCEL  1
#define MAX_BAD_BLOCKS 488   /* 8 BADB blocks x 61 entries each */

void bad_block_scan(struct Window *win, struct BlockDev *bd,
                    struct RDBInfo *rdb)
{
    static ULONG  bbl[MAX_BAD_BLOCKS];
    static char   title_buf[96];
    UBYTE        *buf        = NULL;
    ULONG         total_blks = 0;
    ULONG         bad_count  = 0;
    ULONG         blk        = 0;
    BOOL          cancelled  = FALSE;
    BOOL          capped     = FALSE;
    char          line[80];
    struct EasyStruct es;

    es.es_StructSize = sizeof(es);
    es.es_Flags      = 0;
    es.es_Title      = (UBYTE *)"Bad Block Scan";

    if (!bd) {
        es.es_TextFormat   = (UBYTE *)"No device open.";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    /* Determine total block count */
    if (bd->total_bytes > 0 && bd->block_size > 0)
        total_blks = (ULONG)(bd->total_bytes / bd->block_size);
    if (total_blks == 0 && rdb && rdb->valid)
        total_blks = rdb->cylinders * rdb->heads * rdb->sectors;
    if (total_blks == 0) {
        es.es_TextFormat   = (UBYTE *)"Cannot determine disk size.\nScan not possible.";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(win, &es, NULL);
        return;
    }

    buf = (UBYTE *)AllocVec(bd->block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return;

    /* ---- Phase 1: scan with progress window ---- */
    {
        struct Screen  *scr       = NULL;
        APTR            vi        = NULL;
        struct Gadget  *scan_list = NULL;
        struct Window  *scan_win  = NULL;
        UWORD font_h, bor_t, bor_b, bor_l, bor_r, pad, btn_h, win_w, win_h;

        scr = LockPubScreen(NULL);
        if (scr) {
            vi = GetVisualInfoA(scr, NULL);
            if (vi) {
                struct Gadget *gctx;
                struct NewGadget ng;
                struct TagItem bt[] = { { TAG_DONE, 0 } };

                font_h = (UWORD)scr->Font->ta_YSize;
                bor_t  = (UWORD)scr->WBorTop + font_h + 1;
                bor_b  = (UWORD)scr->WBorBottom;
                bor_l  = (UWORD)scr->WBorLeft;
                bor_r  = (UWORD)scr->WBorRight;
                pad    = 4;
                btn_h  = font_h + 6;
                win_w  = 360;
                win_h  = (UWORD)(bor_t + pad + btn_h + pad + bor_b);

                memset(&ng, 0, sizeof(ng));
                ng.ng_VisualInfo = vi;
                ng.ng_TextAttr   = scr->Font;
                ng.ng_LeftEdge   = (WORD)(bor_l + pad);
                ng.ng_TopEdge    = (WORD)(bor_t + pad);
                ng.ng_Width      = (WORD)(win_w - bor_l - bor_r - pad * 2);
                ng.ng_Height     = btn_h;
                ng.ng_GadgetText = "Cancel";
                ng.ng_GadgetID   = BBSCAN_CANCEL;
                ng.ng_Flags      = PLACETEXT_IN;

                gctx = CreateContext(&scan_list);
                if (gctx && CreateGadgetA(BUTTON_KIND, gctx, &ng, bt)) {
                    struct TagItem wt[] = {
                        { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                        { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                        { WA_Width,     win_w }, { WA_Height, win_h },
                        { WA_Title,     (ULONG)"Bad Block Scan" },
                        { WA_Gadgets,   (ULONG)scan_list },
                        { WA_PubScreen, (ULONG)scr },
                        { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                        IDCMP_REFRESHWINDOW },
                        { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                        WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                        WFLG_SIMPLE_REFRESH },
                        { TAG_DONE, 0 }
                    };
                    scan_win = OpenWindowTagList(NULL, wt);
                }
                if (!scan_win) { FreeGadgets(scan_list); scan_list = NULL;
                                 FreeVisualInfo(vi);     vi        = NULL; }
            }
            UnlockPubScreen(NULL, scr); scr = NULL;
        }
        if (scan_win) GT_RefreshWindow(scan_win, NULL);

        /* Scan loop — polls scan_win for cancel between every block read. */
        for (blk = 0; blk < total_blks && !cancelled; blk++) {
            if (scan_win) {
                struct IntuiMessage *imsg;
                while ((imsg = GT_GetIMsg(scan_win->UserPort)) != NULL) {
                    ULONG  iclass = imsg->Class;
                    struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                    GT_ReplyIMsg(imsg);
                    if (iclass == IDCMP_GADGETUP && gad->GadgetID == BBSCAN_CANCEL)
                        cancelled = TRUE;
                    if (iclass == IDCMP_CLOSEWINDOW)
                        cancelled = TRUE;
                    if (iclass == IDCMP_REFRESHWINDOW) {
                        GT_BeginRefresh(scan_win);
                        GT_EndRefresh(scan_win, TRUE);
                    }
                }
            }
            if (cancelled) break;

            if (!BlockDev_ReadBlock(bd, blk, buf)) {
                if (!capped) {
                    if (bad_count < MAX_BAD_BLOCKS)
                        bbl[bad_count++] = blk;
                    else
                        capped = TRUE;
                }
            }

            /* Update title every 1000 blocks */
            if (scan_win && (blk % 1000 == 0)) {
                ULONG pct = blk * 100UL / total_blks;
                sprintf(title_buf, "Bad Block Scan - %lu/%lu (%lu%%) Bad:%lu",
                        (unsigned long)(blk + 1), (unsigned long)total_blks,
                        (unsigned long)pct, (unsigned long)bad_count);
                SetWindowTitles(scan_win, title_buf, (UBYTE *)-1L);
            }
        }

        if (scan_win)  { RemoveGList(scan_win, scan_list, -1); CloseWindow(scan_win); }
        if (scan_list)   FreeGadgets(scan_list);
        if (vi)          FreeVisualInfo(vi);
    }

    FreeVec(buf); buf = NULL;

    /* ---- Phase 2: show results ---- */
    vrdb_count = 0;
    vrdb_list.lh_Head     = (struct Node *)&vrdb_list.lh_Tail;
    vrdb_list.lh_Tail     = NULL;
    vrdb_list.lh_TailPred = (struct Node *)&vrdb_list.lh_Head;

    vrdb_add("=== Bad Block Scan Results ===");
    if (bd->disk_brand[0]) {
        sprintf(line, "  Drive: %s", bd->disk_brand);
        vrdb_add(line);
    }
    sprintf(line, "  Blocks scanned: %lu%s",
            (unsigned long)blk,
            cancelled ? " (cancelled)" : "");
    vrdb_add(line);
    sprintf(line, "  Bad blocks found: %lu%s",
            (unsigned long)bad_count,
            capped ? " (limit reached, more may exist)" : "");
    vrdb_add(line);
    vrdb_add("");

    if (bad_count == 0 && !cancelled) {
        vrdb_add("  No bad blocks found. Disk surface appears healthy.");
    } else if (bad_count > 0) {
        ULONG i;
        vrdb_add("  Bad block list:");
        for (i = 0; i < bad_count; i++) {
            sprintf(line, "    %lu  (0x%08lX)",
                    (unsigned long)bbl[i], (unsigned long)bbl[i]);
            vrdb_add(line);
        }
        if (capped)
            vrdb_add("    ... (more bad blocks exist - limit reached)");
    }

    {
        struct Screen *scr   = NULL;
        APTR           vi    = NULL;
        struct Gadget *glist = NULL;
        struct Window *vwin  = NULL;
        UWORD font_h, bor_t, bor_b, pad, row_h, btn_h, win_w, win_h, min_h, scr_w, scr_h;

        scr = LockPubScreen(NULL);
        if (!scr) goto bbs_no_vrdb;
        vi = GetVisualInfoA(scr, NULL);
        if (!vi) { UnlockPubScreen(NULL, scr); goto bbs_no_vrdb; }

        font_h = scr->Font->ta_YSize;
        bor_t  = scr->WBorTop + font_h + 1;
        bor_b  = scr->WBorBottom;
        pad    = 4;
        row_h  = font_h + 2;
        btn_h  = font_h + 6;
        win_w  = 480;
        win_h  = bor_t + pad + row_h * 20 + pad + btn_h + pad + bor_b;
        min_h  = bor_t + pad + row_h *  4 + pad + btn_h + pad + bor_b;
        scr_w  = scr->Width;
        scr_h  = scr->Height;

        if (!vrdb_make_gadgets(vi, scr, win_w, win_h, &glist))
            goto bbs_win_cleanup;

        { struct TagItem wt[] = {
              { WA_Left,      (ULONG)((scr_w - win_w) / 2) },
              { WA_Top,       (ULONG)((scr_h - win_h) / 2) },
              { WA_Width,     win_w }, { WA_Height, win_h },
              { WA_Title,     (ULONG)"Bad Block Scan Results" },
              { WA_Gadgets,   (ULONG)glist },
              { WA_PubScreen, (ULONG)scr },
              { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                              IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW },
              { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                              WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH |
                              WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM },
              { WA_MinWidth,  300 }, { WA_MinHeight, min_h },
              { WA_MaxWidth,  scr_w }, { WA_MaxHeight, scr_h },
              { TAG_DONE, 0 } };
          vwin = OpenWindowTagList(NULL, wt); }

        UnlockPubScreen(NULL, scr); scr = NULL;
        if (!vwin) goto bbs_win_cleanup;
        GT_RefreshWindow(vwin, NULL);

        { BOOL running = TRUE;
          while (running) {
              struct IntuiMessage *imsg;
              WaitPort(vwin->UserPort);
              while ((imsg = GT_GetIMsg(vwin->UserPort)) != NULL) {
                  ULONG  iclass = imsg->Class;
                  struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                  GT_ReplyIMsg(imsg);
                  switch (iclass) {
                  case IDCMP_CLOSEWINDOW: running = FALSE; break;
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
                  case IDCMP_GADGETUP:
                      if (gad->GadgetID == VRDB_DONE) running = FALSE; break;
                  case IDCMP_REFRESHWINDOW:
                      GT_BeginRefresh(vwin); GT_EndRefresh(vwin, TRUE); break;
                  }
              }
          }
        }

bbs_win_cleanup:
        if (vwin)  { RemoveGList(vwin, glist, -1); CloseWindow(vwin); }
        if (glist)   FreeGadgets(glist);
        if (vi)      FreeVisualInfo(vi);
        if (scr)     UnlockPubScreen(NULL, scr);
    }

bbs_no_vrdb:
    /* ---- Phase 3: offer BADB write if bad blocks found and RDB exists ---- */
    if (bad_count > 0 && !cancelled && rdb && rdb->valid) {
        LONG r;
        static char badb_msg[160];
        sprintf(badb_msg,
                "%lu bad block(s) found.\n"
                "Write bad block list to RDB?\n\n"
                "Note: entries will be cleared if\n"
                "you write the partition table later.",
                (unsigned long)bad_count);
        es.es_TextFormat   = (UBYTE *)badb_msg;
        es.es_GadgetFormat = (UBYTE *)"Write|Skip";
        r = EasyRequest(win, &es, NULL);
        if (r == 1) {
            if (write_badb(win, bd, rdb, bbl, bad_count)) {
                es.es_TextFormat   = (UBYTE *)"Bad block list written to RDB.";
                es.es_GadgetFormat = (UBYTE *)"OK";
                EasyRequest(win, &es, NULL);
            }
        }
    }
}
