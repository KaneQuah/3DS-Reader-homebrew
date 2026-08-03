#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET   : output filename (no extension)
# BUILD    : directory for intermediate files
# SOURCES  : directories containing source code (space-separated)
# DATA     : directories containing binary data to embed
# INCLUDES : directories containing header files
# ROMFS    : directory packed into RomFS (fonts, TTS voice data)
#---------------------------------------------------------------------------------
TARGET          :=  BookReader
BUILD           :=  build
SOURCES         :=  source \
                    source/ui \
                    source/formats \
                    source/renderer \
                    source/settings \
                    source/input \
                    source/audio \
                    source/thirdparty
DATA            :=  data
INCLUDES        :=  include source source/thirdparty
GRAPHICS        :=  gfx
GFXBUILD        :=  $(BUILD)
ROMFS           :=  romfs

APP_TITLE       :=  3DS EPUB Reader
APP_DESCRIPTION :=  Dual-screen EPUB, TXT and PDF reader
APP_AUTHOR      :=  BookReader Project

#---------------------------------------------------------------------------------
# Feature flags
#
#   ENABLE_PDF=1  requires MuPDF cross-compiled for 3DS (not in devkitPro
#                 portlibs — must be built from source). When 0, PdfRenderer
#                 compiles to no-op stubs and PDF files are rejected at open.
#
#   ENABLE_TTS=1  requires eSpeak NG cross-compiled for 3DS (not in devkitPro
#                 portlibs). When 0, TtsEngine compiles to no-op stubs.
#
# Override on the command line, e.g.:   make ENABLE_PDF=1
#---------------------------------------------------------------------------------
#   ENABLE_TRACE=1 writes a step-by-step log to /3DS/BookReader/trace.log,
#                 flushed and closed after every line so it survives a hang.
#                 Set to 0 for release builds — it is very slow.
#---------------------------------------------------------------------------------
ENABLE_PDF      ?=  0
ENABLE_TTS      ?=  0
ENABLE_TRACE    ?=  1

#---------------------------------------------------------------------------------
# Code-generation options
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  :=  -g -Wall -O2 -mword-relocations \
            -fomit-frame-pointer -ffunction-sections \
            $(ARCH)

CFLAGS  +=  $(INCLUDE) -D__3DS__ \
            -DENABLE_PDF=$(ENABLE_PDF) -DENABLE_TTS=$(ENABLE_TTS) \
            -DENABLE_TRACE=$(ENABLE_TRACE)

# FreeType installs its headers into <portlibs>/include/freetype2, one level
# deeper than the generic -I<portlibs>/include that LIBDIRS provides.
CFLAGS  +=  -I$(PORTLIBS)/include/freetype2

CXXFLAGS    :=  $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Libraries
#   Link order matters: citro2d before citro3d before ctru.
#
#   Available from devkitPro portlibs (install with:
#     pacman -S 3ds-freetype 3ds-libpng 3ds-libjpeg-turbo 3ds-zlib 3ds-bzip2)
#
#   pugixml is NOT a portlib — it is vendored in source/thirdparty/ and
#   compiled directly into the project, so no -l flag is needed.
#
#   MuPDF and eSpeak NG are NOT portlibs either. They must be cross-compiled
#   from source and installed into $(PORTLIBS) before enabling their flags.
#---------------------------------------------------------------------------------
LIBS    :=  -lcitro2d -lcitro3d -lctru \
            -lfreetype -lpng -ljpeg \
            -lz -lbz2 -lm

ifeq ($(ENABLE_PDF),1)
LIBS    :=  -lmupdf -lmupdf-third $(LIBS)
endif

ifeq ($(ENABLE_TTS),1)
LIBS    :=  -lespeak-ng $(LIBS)
endif

#---------------------------------------------------------------------------------
# Library search paths
#   $(CTRULIB)  : libctru headers + libs  (set by devkitPro rules)
#   $(PORTLIBS) : portlibs for 3DS        (set by devkitPro rules)
#---------------------------------------------------------------------------------
LIBDIRS :=  $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# No real need to edit anything past this point unless you need to add
# additional rules for different file extensions.
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES   :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES    :=  $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# Use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
    export LD   :=  $(CC)
else
    export LD   :=  $(CXX)
endif

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
export T3XFILES         :=  $(GFXFILES:.t3s=.t3x)
else
export ROMFS_T3XFILES   :=  $(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES        :=  $(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
endif

export OFILES_SOURCES   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES)) \
                        $(PICAFILES:.v.pica=.shbin.o) \
                        $(SHLISTFILES:.shlist=.shbin.o) \
                        $(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES   :=  $(PICAFILES:.v.pica=_shbin.h) \
                    $(SHLISTFILES:.shlist=_shbin.h) \
                    $(addsuffix .h,$(subst .,_,$(BINFILES))) \
                    $(GFXFILES:.t3s=.h)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS    :=  $(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(findstring icon.png,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# Main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx  :   $(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf   :   $(OFILES)

#---------------------------------------------------------------------------------
# You need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
