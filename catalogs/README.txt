================================================================================
                       DiskPart - Localization (Catalogs)
================================================================================

DiskPart uses the AmigaOS locale.library catalog system for translation.

  * All translatable strings live in  DiskPart.cd  (the catalog description -
    the single source of truth).
  * The C build generates  ../src/diskpart_strings.h  from it (string ids +
    the built-in ENGLISH defaults compiled into the program).
  * Translators produce a  <language>.ct  file and compile it into a binary
    <language>/DiskPart.catalog.

locale.library only exists on AmigaOS 2.1 (Kickstart V38) and later.  On
Kickstart 2.04 (V37) DiskPart runs unchanged and simply shows the built-in
English strings - no catalog is loaded, nothing fails.  Full 2.x compatibility
is preserved.


--------------------------------------------------------------------------------
 TOOLCHAIN
--------------------------------------------------------------------------------
The standard FlexCat / CatComp tools understand DiskPart.cd directly.  For a
self-contained build (no FlexCat installed) a small reproducible generator is
provided: ../support/gencat.py  (used automatically by the Makefile to produce
diskpart_strings.h).  It also creates templates and compiles catalogs:

  Generate the C header (done automatically by `make`):
    python3 ../support/gencat.py header DiskPart.cd ../src/diskpart_strings.h

  Create a fresh translation template (or use `make catalog-template LANG=deutsch`):
    python3 ../support/gencat.py ct DiskPart.cd deutsch.ct deutsch

  Compile a finished translation into a binary catalog:
    python3 ../support/gencat.py catalog DiskPart.cd deutsch.ct deutsch/DiskPart.catalog

FlexCat equivalents (if you prefer the real tools):
    flexcat DiskPart.cd NEWCTFILE deutsch.ct
    flexcat DiskPart.cd deutsch.ct CATALOG deutsch/DiskPart.catalog


--------------------------------------------------------------------------------
 TRANSLATING
--------------------------------------------------------------------------------
1. Generate a template:        make catalog-template LANG=deutsch
   (writes catalogs/deutsch.ct, pre-filled with the English text)
2. Edit deutsch.ct: translate the line under each MSG_ identifier.
   * Keep every printf placeholder (%s %lu %ld ...) - same count and order.
   * Keep the "|" separators in button strings (e.g. "OK|Cancel").
   * The Amiga character set is ISO-8859-1 (Latin-1); save the file as Latin-1.
3. Compile:  make catalog LANG=deutsch   (or the gencat.py command above)


--------------------------------------------------------------------------------
 INSTALLING A CATALOG
--------------------------------------------------------------------------------
Copy the compiled catalog to either of locale.library's search paths:

    LOCALE:Catalogs/deutsch/DiskPart.catalog      (system-wide)
    PROGDIR:Catalogs/deutsch/DiskPart.catalog     (next to the DiskPart binary)

Set your preferred language in the Locale Preferences and relaunch DiskPart.


--------------------------------------------------------------------------------
 EDITING DiskPart.cd (developers)
--------------------------------------------------------------------------------
  * ALWAYS append new messages at the END of their section, never insert in the
    middle - string ids are positional and must stay stable so existing
    translated catalogs keep matching.
  * Encode line breaks as \n, tabs as \t, embedded quotes as \", backslash as \\.
  * One logical string per message (use \n for internal newlines).
================================================================================
