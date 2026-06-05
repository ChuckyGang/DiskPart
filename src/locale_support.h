/*
 * locale_support.h - locale.library catalog support for DiskPart.
 *
 * DiskPart's translatable strings live in catalogs/DiskPart.cd; the C ids and
 * built-in English defaults are generated from it into diskpart_strings.h.
 *
 * locale.library only exists on AmigaOS 2.1 (Kickstart v38) and later.  On
 * Kickstart 2.04 (v37) - which DiskPart still supports - LocaleOpen() is a
 * harmless no-op and GS() returns the built-in English strings.
 */

#ifndef LOCALE_SUPPORT_H
#define LOCALE_SUPPORT_H

#include <exec/types.h>
#include "diskpart_strings.h"

/* Open locale.library (v38+) and the DiskPart.catalog for the user's
 * preferred language.  No-op when locale.library is unavailable. */
void LocaleOpen(void);
void LocaleClose(void);

/* Localized string for message id `id`, or the built-in English default
 * when no catalog/locale is available.  Never returns NULL. */
CONST_STRPTR GetDPString(LONG id);

/* Convenience wrapper for call sites.  The cast keeps the many char* /
 * const char* / STRPTR call sites warning-free under this build's flags. */
#define GS(id) ((char *)GetDPString(id))

#endif /* LOCALE_SUPPORT_H */
