# CurrentPod - korskompileras med bebbo/AmigaPorts gcc 6.5 (m68k-amigaos)
# Källorna är UTF-8 på disk och konverteras till Latin-1 innan de kompileras,
# så att strängarna blir rätt på Workbench.
TC    ?= /opt/amiga
X     ?= ../sdk
MPEGA ?= $(X)/mpega_library
MKCAT ?= ../tools/mkcatalog.py
CC     = $(TC)/bin/m68k-amigaos-gcc
STRIP  = $(TC)/bin/m68k-amigaos-strip
CPU   ?= 68020
OUT   ?= ../build
L1     = $(OUT)/latin1

INC = -Iinclude -I$(OUT) -I$(MPEGA)/developer/include \
      -I$(X)/MUI-3.9-2015R1-os3/SDK/MUI/C/include \
      -I$(X)/AmiSSL-5.27-SDK/AmiSSL/Developer/include

CFLAGS = -O2 -m$(CPU) -m68881 -noixemul -fomit-frame-pointer -fno-strict-aliasing \
         -Wall -Wno-pointer-sign -Wno-int-conversion -Wno-unused-function -Wno-format \
         -Wno-incompatible-pointer-types -Wno-misleading-indentation $(INC)
LDFLAGS = -noixemul
LIBS = -lamiga -lm

SRC = main.c gui.c util.c db.c http.c rss.c json.c worker.c player.c convert.c locale.c
OBJ = $(patsubst %.c,$(OUT)/%.o,$(SRC))
HDR = $(L1)/currentpod.h $(OUT)/strings.h include/inline/mpega.h include/proto/mpega.h

all: $(OUT)/CurrentPod $(OUT)/catalogs/svenska/CurrentPod.catalog

$(OUT)/strings.h: CurrentPod.cd tools_mkcatalog | $(OUT)
	python3 $(MKCAT) header CurrentPod.cd $@

$(OUT)/catalogs/svenska/CurrentPod.catalog: CurrentPod.cd catalogs/svenska.ct tools_mkcatalog
	mkdir -p $(dir $@)
	python3 $(MKCAT) catalog CurrentPod.cd catalogs/svenska.ct $@

tools_mkcatalog: $(MKCAT)
.PHONY: tools_mkcatalog

$(L1)/%: % | $(OUT)
	@mkdir -p $(dir $@)
	iconv -f UTF-8 -t ISO-8859-1 $< > $@

$(OUT)/%.o: $(L1)/%.c $(HDR) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/CurrentPod: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	$(STRIP) --strip-all $@ 2>/dev/null || true
	@ls -l $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all clean
