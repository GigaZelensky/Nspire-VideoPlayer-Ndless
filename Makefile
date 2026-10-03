.DEFAULT_GOAL := all
DEBUG = FALSE
CODECS ?= h264 mpeg4 hevc
SUPPORTED_CODECS = h264 mpeg4 hevc
ifneq ($(strip $(filter-out $(SUPPORTED_CODECS),$(CODECS))),)
$(error Unknown codec in CODECS="$(CODECS)"; choose h264, mpeg4 and/or hevc)
endif
SELECTED_CODECS := $(strip $(foreach codec,$(SUPPORTED_CODECS),$(if $(filter $(codec),$(CODECS)),$(codec))))
ifeq ($(SELECTED_CODECS),)
$(error CODECS must select at least one codec)
endif
empty :=
space := $(empty) $(empty)
CODEC_TAG := $(subst $(space),-,$(SELECTED_CODECS))
CODEC_SUFFIX := $(if $(filter-out h264-mpeg4-hevc,$(CODEC_TAG)),-$(CODEC_TAG))
CODEC_FLAGS = -DNDVIDEO_WITH_H264=$(if $(filter h264,$(SELECTED_CODECS)),1,0) \
	-DNDVIDEO_WITH_MPEG4=$(if $(filter mpeg4,$(SELECTED_CODECS)),1,0) \
	-DNDVIDEO_WITH_HEVC=$(if $(filter hevc,$(SELECTED_CODECS)),1,0)
OBJDIR = build/$(if $(filter TRUE,$(DEBUG)),debug,release)/$(CODEC_TAG)
ifneq ($(wildcard ./external/Ndless-official/ndless-sdk/include/libndls.h),)
SDKROOT ?= ./external/Ndless-official/ndless-sdk
else
SDKROOT ?= ./external/Ndless/ndless-sdk
endif
PYTHON ?= python
PACKZEHN = $(PYTHON) tools/pack_zehn.py
RAW_GXX ?= arm-none-eabi-g++
OBJCOPY ?= arm-none-eabi-objcopy
LOADER = $(SDKROOT)/tools/zehn_loader/zehn_loader.tns
LOADER_DIR = $(SDKROOT)/tools/zehn_loader
LOADER_ELF = $(LOADER_DIR)/zehn_loader.tns.elf

export PATH := $(abspath $(SDKROOT)/bin):$(PATH)

GCC = nspire-gcc
AS  = nspire-as
GXX = nspire-g++
LD  = nspire-gcc -nodefaultlibs

GCCFLAGS_BASE = -Wall -Wextra -Wno-unused-parameter -std=c99 -marm -mcpu=arm926ej-s -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections -Isrc -Isrc/codecs/h264bsd -Isrc/codecs -Isrc/codecs/xvid -DARCH_IS_32BIT -DARCH_IS_ARM -DXVID_DECODER_ONLY
LDFLAGS = -Wl,--gc-sections -lSDL -flto -O3
LDFLAGS += -Wl,--wrap=fopen,--wrap=fread,--wrap=fwrite,--wrap=fseek,--wrap=fclose,--wrap=fflush,--wrap=_open,--wrap=_read,--wrap=_write,--wrap=_lseek,--wrap=_close,--wrap=remove,--wrap=nuc_opendir,--wrap=nuc_readdir,--wrap=nuc_closedir
LOADER_GXXFLAGS = -g -Os -Wall -Wextra -march=armv5te -fPIE -std=c++11 -fno-rtti -fno-exceptions -Wl,-Tldscript -Wl,--gc-sections -nostdlib -nostartfiles -ffreestanding -I ../../include
PACKFLAGS = --name "ND Video Player" --author "GigaZelensky" --version 1 --ndless-min 45 --hww-support --uses-lcd-blit --no-support-32mb

ifeq ($(DEBUG),FALSE)
	GCCFLAGS = $(GCCFLAGS_BASE) -Os -flto
	FAST_GCCFLAGS = $(GCCFLAGS_BASE) -O3 -DNDEBUG -fno-strict-aliasing -fomit-frame-pointer -falign-functions=32 -falign-loops=32 -flto -funroll-loops
else
	GCCFLAGS = $(GCCFLAGS_BASE) -O0 -g
	FAST_GCCFLAGS = $(GCCFLAGS_BASE) -O0 -g -falign-functions=32 -falign-loops=32
endif

# Keep aggressive unrolling/alignment on decode, pixel, and frame-timing paths.
# Picker, resource setup, and history code benefit from a smaller instruction footprint.
HOT_PLAYER_SRCS = src/player/video_lookahead.c src/player/hevc_playback.c src/player/codec_streaming.c src/player/render_primitives.c \
	src/player/playback_ui.c src/player/playback_loop.c src/player/subtitles.c \
	src/player/input_timing_memory.c src/player/night_mode.c \
	src/player/movie_open_scan.c src/player/platform_debug.c
FAST_SRCS = src/codecs/hevc/% src/codecs/hevc_decoder.cpp src/codecs/h264bsd/% src/codecs/xvid/% src/codecs/mpeg4_xvid.c \
	src/crypto/bearssl/% src/movie/nve_crypto.c $(HOT_PLAYER_SRCS)

XVID_DECODER_SRCS = \
	src/codecs/xvid/xvid.c \
	src/codecs/xvid/decoder.c \
	src/codecs/xvid/bitstream/bitstream.c \
	src/codecs/xvid/bitstream/cbp.c \
	src/codecs/xvid/bitstream/mbcoding.c \
	src/codecs/xvid/dct/fdct.c \
	src/codecs/xvid/dct/idct.c \
	src/codecs/xvid/dct/simple_idct.c \
	src/codecs/xvid/image/arm/yv12_to_rgb565.c \
	src/codecs/xvid/image/colorspace.c \
	src/codecs/xvid/image/font.c \
	src/codecs/xvid/image/image.c \
	src/codecs/xvid/image/interpolate8x8.c \
	src/codecs/xvid/image/postprocessing.c \
	src/codecs/xvid/image/qpel.c \
	src/codecs/xvid/image/reduced.c \
	src/codecs/xvid/motion/estimation_common.c \
	src/codecs/xvid/motion/gmc.c \
	src/codecs/xvid/motion/motion_comp.c \
	src/codecs/xvid/motion/sad.c \
	src/codecs/xvid/prediction/mbprediction.c \
	src/codecs/xvid/quant/quant_h263.c \
	src/codecs/xvid/quant/quant_matrix.c \
	src/codecs/xvid/quant/quant_mpeg.c \
	src/codecs/xvid/utils/emms.c \
	src/codecs/xvid/utils/mbtransquant.c \
	src/codecs/xvid/utils/mem_align.c \
	src/codecs/xvid/utils/mem_transfer.c \
	src/codecs/xvid/utils/sram_tables.c \
	src/codecs/xvid/utils/timer.c

# GNU make's wildcard skips hidden directories, including nested worktrees.
source_files = $(foreach entry,$(wildcard $(1)*),$(call source_files,$(entry)/,$(2)) $(filter $(2),$(entry)))
SOURCES := $(filter-out src/codecs/xvid/%,$(call source_files,src/,%.c %.cpp %.S))
ifeq ($(filter h264,$(SELECTED_CODECS)),)
SOURCES := $(filter-out src/codecs/h264bsd/%,$(SOURCES))
endif
ifeq ($(filter mpeg4,$(SELECTED_CODECS)),)
SOURCES := $(filter-out src/codecs/mpeg4_xvid.c,$(SOURCES))
else
SOURCES += $(XVID_DECODER_SRCS)
endif
ifeq ($(filter hevc,$(SELECTED_CODECS)),)
SOURCES := $(filter-out src/codecs/hevc/% src/codecs/hevc_decoder.cpp,$(SOURCES))
endif
OBJS = $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(SOURCES))))
EXE = ndvideo
TNS = _$(EXE)$(CODEC_SUFFIX).tns
DISTDIR = dist
RELEASEDIR = release
ELF = $(OBJDIR)/$(EXE).elf
ZEHN = $(OBJDIR)/$(EXE).zehn
PLAYER = $(OBJDIR)/$(TNS)
LEGACY_OBJS = src/player.o
RELEASE_VARIANTS = h264 mpeg4 hevc h264-mpeg4 h264-hevc mpeg4-hevc h264-mpeg4-hevc
RELEASE_TARGETS = $(addprefix release-,$(RELEASE_VARIANTS))

.PHONY: all clean release $(RELEASE_TARGETS)

all: $(PLAYER)
	mkdir -p $(DISTDIR)
	cp "$(PLAYER)" "$(DISTDIR)/$(TNS)"
	cp "$(ELF)" "$(DISTDIR)/$(EXE)$(CODEC_SUFFIX).elf"
	cp "$(ZEHN)" "$(DISTDIR)/$(EXE)$(CODEC_SUFFIX).zehn"

$(OBJDIR)/%.o: %.c Makefile
	mkdir -p $(dir $@)
	$(GCC) $(if $(filter $(FAST_SRCS),$<),$(FAST_GCCFLAGS),$(GCCFLAGS)) $(CODEC_FLAGS) $(if $(filter src/codecs/xvid/%,$<),-Wno-incompatible-pointer-types,-Werror=incompatible-pointer-types) -MMD -MP -c $< -o $@

HEVC_GXXFLAGS = $(filter-out -std=c99,$(FAST_GCCFLAGS)) -std=c++11 -fno-exceptions -fno-rtti \
    -DLIBDE265_STATIC_BUILD -DHAVE_STDINT_H -DHAVE_ALLOCA_H -Isrc/codecs/hevc

$(OBJDIR)/%.o: %.cpp Makefile
	mkdir -p $(dir $@)
	$(GXX) $(HEVC_GXXFLAGS) $(CODEC_FLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.o: %.S Makefile
	mkdir -p $(dir $@)
	$(AS) $(CODEC_FLAGS) -c $< -o $@

$(ELF): $(OBJS)
	$(LD) $^ -o $@ $(LDFLAGS) $(if $(filter hevc,$(SELECTED_CODECS)),-lstdc++)

$(LOADER):
	cd $(LOADER_DIR) && $(RAW_GXX) $(LOADER_GXXFLAGS) loader.cpp -o zehn_loader.tns.elf
	$(OBJCOPY) --set-section-flags .pad=alloc,load,contents -O binary $(LOADER_ELF) $(LOADER)

$(PLAYER): $(ELF) $(LOADER) tools/pack_zehn.py
	$(PACKZEHN) --input $< --output "$@" --zehn-output $(ZEHN) --loader $(LOADER) $(PACKFLAGS)

$(RELEASE_TARGETS): release-%:
	$(MAKE) CODECS="$(subst -, ,$*)" all

release: $(RELEASE_TARGETS)
	$(PYTHON) tools/package_release.py --dist "$(DISTDIR)" --output "$(RELEASEDIR)" --notes build/release-notes.md

clean:
	rm -f $(OBJS) $(LEGACY_OBJS) $(PLAYER) $(ELF) $(ZEHN) "$(DISTDIR)/$(TNS)" $(DISTDIR)/$(EXE)$(CODEC_SUFFIX).elf $(DISTDIR)/$(EXE)$(CODEC_SUFFIX).zehn
	rm -f $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
