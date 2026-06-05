/*
 * locale_support.c - locale.library catalog support for DiskPart.
 *
 * See locale_support.h.  Kept deliberately small so it can be the only file
 * that needs <proto/locale.h>; everything else just calls GS(MSG_xxx).
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/locale.h>

#include "locale_support.h"

/* Instantiate the built-in English default table here (one definition). */
#define DPSTRINGS_DEFINE_TABLE
#include "diskpart_strings.h"
#undef DPSTRINGS_DEFINE_TABLE

/* proto/locale.h declares this; we own the single definition. */
struct LocaleBase   *LocaleBase      = NULL;
static struct Catalog *DiskPartCatalog = NULL;

void LocaleOpen(void)
{
    /* v38 = AmigaOS 2.1.  NULL on 2.04 and earlier -> built-in English. */
    LocaleBase = (struct LocaleBase *)OpenLibrary("locale.library", 38);
    if (LocaleBase) {
        /* Explicit tag array: the varargs OpenCatalog() inline is disabled by
         * this build's -DNO_INLINE_STDARG, so we call OpenCatalogA directly. */
        struct TagItem tags[] = {
            { OC_BuiltInLanguage, (ULONG)"english" },
            { OC_Version,         1 },
            { TAG_DONE,           0 },
        };
        DiskPartCatalog = OpenCatalogA(NULL, (STRPTR)"DiskPart.catalog", tags);
    }
}

void LocaleClose(void)
{
    if (LocaleBase) {
        if (DiskPartCatalog) {
            CloseCatalog(DiskPartCatalog);
            DiskPartCatalog = NULL;
        }
        CloseLibrary((struct Library *)LocaleBase);
        LocaleBase = NULL;
    }
}

CONST_STRPTR GetDPString(LONG id)
{
    CONST_STRPTR def = (id >= 0 && id < MSG_COUNT)
                       ? DPStringDefaults[id]
                       : (CONST_STRPTR)"";
    /* GetCatalogStr safely returns `def` for a NULL catalog or missing id. */
    if (LocaleBase)
        return (CONST_STRPTR)GetCatalogStr(DiskPartCatalog, id, (STRPTR)def);
    return def;
}
