# DiskPart Makefile
# AmigaOS 2.x+ GadTools hard-drive selector
#
# Toolchains supported (auto-detected, override with TOOLCHAIN=...):
#   bartman - m68k-amiga-elf-gcc (Bartman/Abyss VSCode extension), ELF + elf2hunk
#   bebbo   - m68k-amigaos-gcc   (Bebbo's amiga-gcc), native hunk output
#
# Override paths or selection from the command line, e.g.:
#   make TOOLCHAIN=bebbo
#   make TOOLCHAIN=bebbo AMIGA=/opt/amiga
#   make TOOLCHAIN=bartman BARTMAN=/path/to/bartman/bin/linux

# --- Toolchain root paths (overridable) --------------------------------------

BARTMAN ?= /home/john/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/linux
AMIGA   ?= /opt/amiga

# Auto-detect TOOLCHAIN if not specified: prefer Bartman when its extension
# is installed (the Bebbo build currently has a startup-hang issue on this
# project), otherwise fall back to Bebbo. CI/docker.sh sets TOOLCHAIN=bebbo
# explicitly, so it bypasses this preference.
ifeq ($(origin TOOLCHAIN),undefined)
  ifneq ($(wildcard $(BARTMAN)/opt/bin/m68k-amiga-elf-gcc),)
    TOOLCHAIN := bartman
  else ifneq ($(wildcard $(AMIGA)/bin/m68k-amigaos-gcc),)
    TOOLCHAIN := bebbo
  else
    TOOLCHAIN := bartman
  endif
endif

program = out/DiskPart

# --- Per-toolchain configuration ---------------------------------------------

ifeq ($(TOOLCHAIN),bartman)
  CC       := $(BARTMAN)/opt/bin/m68k-amiga-elf-gcc
  AS       := $(BARTMAN)/opt/bin/m68k-amiga-elf-as
  OBJDUMP  := $(BARTMAN)/opt/bin/m68k-amiga-elf-objdump
  ELF2HUNK := $(BARTMAN)/elf2hunk
  SDKDIR   := $(BARTMAN)/opt/m68k-amiga-elf/sys-include

  TC_CCFLAGS     := -nostdlib
  TC_LDFLAGS     := -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(program).map
  TC_SUPPORT_OBJ := obj/gcc8_c_support.o obj/gcc8_a_support.o
  NEED_ELF2HUNK  := 1
else ifeq ($(TOOLCHAIN),bebbo)
  CC       := $(AMIGA)/bin/m68k-amigaos-gcc
  AS       := $(AMIGA)/bin/m68k-amigaos-as
  OBJDUMP  := $(AMIGA)/bin/m68k-amigaos-objdump
  STRIP    := $(AMIGA)/bin/m68k-amigaos-strip
  SDKDIR   := $(AMIGA)/m68k-amigaos/ndk-include

  TC_CCFLAGS     := -noixemul
  TC_LDFLAGS     := -noixemul -Wl,--gc-sections,-Map=$(program).map
  TC_SUPPORT_OBJ :=
  NEED_ELF2HUNK  := 0
else
  $(error Unknown TOOLCHAIN '$(TOOLCHAIN)' - use bartman or bebbo)
endif

# --- Common flags ------------------------------------------------------------

CCFLAGS = -g -MP -MMD -m68000 -O2 $(TC_CCFLAGS) \
          -Wextra -Wno-unused-function -Wno-volatile-register-var \
          -Wno-int-conversion -Wno-incompatible-pointer-types \
          -DNO_INLINE_STDARG \
          -fomit-frame-pointer -fno-tree-loop-distribution \
          -fno-exceptions -ffunction-sections -fdata-sections \
          -Isrc -I$(SDKDIR)

ASFLAGS = -mcpu=68000 -g --register-prefix-optional -I$(SDKDIR)
LDFLAGS = $(TC_LDFLAGS)

$(shell mkdir -p obj out)

# Auto-clean stale objects when the toolchain changes. The .o files don't
# track which compiler produced them, so switching Bebbo<->Bartman would
# otherwise produce "file not recognized" link errors (or worse, mismatched
# ABI). Stamp the current toolchain in obj/.toolchain and wipe on mismatch.
TC_STAMP := obj/.toolchain
ifneq ($(shell cat $(TC_STAMP) 2>/dev/null),$(TOOLCHAIN))
  $(info Toolchain changed -> $(TOOLCHAIN); clearing stale objects)
  $(shell rm -f obj/*.o obj/*.d 2>/dev/null)
  $(shell echo $(TOOLCHAIN) > $(TC_STAMP))
endif

src_c   := $(wildcard src/*.c)
src_obj := $(addprefix obj/,$(patsubst src/%.c,%.o,$(src_c)))
objects := $(src_obj) $(TC_SUPPORT_OBJ)

# --- Localization (locale.library catalogs) ----------------------------------
# catalogs/DiskPart.cd is the single source of truth for translatable strings.
# It generates src/diskpart_strings.h (ids + built-in English defaults).
CD_FILE   := catalogs/DiskPart.cd
STRINGS_H := src/diskpart_strings.h
GENCAT    := support/gencat.py

.PHONY: all clean icon adf FORCE strings catalog-template catalog

# Keep 'all' the default goal (rules below would otherwise grab it).
all: $(program) $(program).info

# Autoboot ADF that boots straight into DiskPart, no Workbench required.
# Needs amitools (`pip install amitools`) for its xdftool.
adf: $(program).adf

$(program).adf: $(program) support/make_adf.py
	$(info Building autoboot ADF $@)
	@python3 support/make_adf.py $@ $(program)

$(STRINGS_H): $(CD_FILE) $(GENCAT)
	$(info Generating $@ from $<)
	@python3 $(GENCAT) header $< $@

# Every object depends on the generated header (most .c files pull it in via
# locale_support.h).
$(src_obj): $(STRINGS_H)

# Regenerate the string header on demand.
strings: $(STRINGS_H)

# Emit a fresh translation template:  make catalog-template LANG=deutsch
LANG ?= deutsch
catalog-template: $(CD_FILE) $(GENCAT)
	$(info Generating catalogs/$(LANG).ct)
	@python3 $(GENCAT) ct $(CD_FILE) catalogs/$(LANG).ct $(LANG)

# Compile a finished translation into a binary catalog:
#   make catalog LANG=deutsch        (reads catalogs/deutsch.ct)
# Install the result as LOCALE:Catalogs/deutsch/DiskPart.catalog
catalog: $(CD_FILE) $(GENCAT)
	$(info Compiling catalogs/$(LANG)/DiskPart.catalog)
	@mkdir -p catalogs/$(LANG)
	@python3 $(GENCAT) catalog $(CD_FILE) catalogs/$(LANG).ct catalogs/$(LANG)/DiskPart.catalog

# build.o embeds __DATE__/__TIME__ via build.c - force-rebuild every
# invocation so the stamp matches the current build.
obj/build.o: FORCE
FORCE:

icon: $(program).info

$(program).info: support/hdicon.png support/make_icon.py
	$(info Generating $@)
	@python3 support/make_icon.py $@

ifeq ($(NEED_ELF2HUNK),1)

$(program): $(program).elf
	$(info Elf2Hunk $@)
	@$(ELF2HUNK) $(program).elf $(program)
	@cp $(program) $(program).exe

$(program).elf: $(objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins \
	    --visualize-jumps -S $@ >$(program).s

else

$(program): $(objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins \
	    --visualize-jumps -S $@ >$(program).s
	$(info Stripping $@)
	@$(STRIP) $@

endif

clean:
	$(info Cleaning...)
	@find obj out -mindepth 1 -maxdepth 1 -type f -delete 2>/dev/null || true

-include $(src_obj:.o=.d)
ifeq ($(TOOLCHAIN),bartman)
-include obj/gcc8_c_support.d obj/gcc8_a_support.d
endif

$(src_obj) : obj/%.o : src/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_c_support.o : support/gcc8_c_support.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_a_support.o : support/gcc8_a_support.s
	$(info Assembling $<)
	@$(AS) $(ASFLAGS) --MD obj/gcc8_a_support.d -o $@ $<
