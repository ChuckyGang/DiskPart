/*
 * partview_shrink.c - GUI "Shrink Report..." (Advanced menu).
 *
 * Runs the same read-only ShrinkInfo_Run() the CLI/script SHRINKINFO
 * command uses, collecting its report lines into a buffer and showing
 * them in one EasyRequest.  Never writes to disk.
 */

#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>
#include <proto/exec.h>
#include <dos/dos.h>

#include "clib.h"
#include "rdb.h"
#include "shrinkinfo.h"
#include "partview_internal.h"
#include "locale_support.h"

struct si_collect {
    char  *buf;
    ULONG  len, cap;
};

static void pv_si_emit(void *ud, const char *line)
{
    struct si_collect *c = (struct si_collect *)ud;
    ULONG n = strlen(line);
    if (n > c->cap - 1 - c->len) n = c->cap - 1 - c->len;
    memcpy(c->buf + c->len, line, n);
    c->len += n;
    c->buf[c->len] = '\0';
}

void pv_shrink_report(struct Window *win, struct BlockDev *bd,
                      struct RDBInfo *rdb, struct PartInfo *pi)
{
    struct EasyStruct es;
    struct si_collect c;
    char   buf[1024];

    buf[0] = '\0';
    c.buf = buf; c.len = 0; c.cap = sizeof(buf);

    /* Success and failure both emit their message lines - the requester
       shows whichever the scan produced. */
    ShrinkInfo_Run(bd, rdb, pi, pv_si_emit, &c);

    while (c.len > 0 && buf[c.len - 1] == '\n')
        buf[--c.len] = '\0';

    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)GS(MSG_PV_SHRINK_TITLE);
    /* Body goes through "%s" - the report contains literal '%' (in-use
       percentage), which would be eaten if passed as the format itself. */
    es.es_TextFormat   = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)GS(MSG_OK);
    EasyRequest(win, &es, NULL, buf);
}
