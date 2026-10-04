.DEFAULT_GOAL := all
DEBUG = FALSE
CODECS ?= h264 mpeg4 hevc av1
SUPPORTED_CODECS = h264 mpeg4 hevc av1
ifneq ($(strip $(filter-out $(SUPPORTED_CODECS),$(CODECS))),)
$(error Unknown codec in CODECS="$(CODECS)"; choose h264, mpeg4, hevc and/or av1)
endif
SELECTED_CODECS := $(strip $(foreach codec,$(SUPPORTED_CODECS),$(if $(filter $(codec),$(CODECS)),$(codec))))
ifeq ($(SELECTED_CODECS),)
$(error CODECS must select at least one codec)
endif
empty :=
space := $(empty) $(empty)
CODEC_TAG := $(subst $(space),-,$(SELECTED_CODECS))
CODEC_SUFFIX := $(if $(filter-out $(subst $(space),-,$(SUPPORTED_CODECS)),$(CODEC_TAG)),-$(CODEC_TAG))
CODEC_MODULES ?= $(if $(word 2,$(SELECTED_CODECS)),1,0)
ifeq ($(filter $(CODEC_MODULES),0 1),)
$(error CODEC_MODULES must be 0 or 1)
endif
CODEC_FLAGS = -DNDVIDEO_WITH_H264=$(if $(filter h264,$(SELECTED_CODECS)),1,0) \
	-DNDVIDEO_WITH_MPEG4=$(if $(filter mpeg4,$(SELECTED_CODECS)),1,0) \
	-DNDVIDEO_WITH_HEVC=$(if $(filter hevc,$(SELECTED_CODECS)),1,0) \
	-DNDVIDEO_WITH_AV1=$(if $(filter av1,$(SELECTED_CODECS)),1,0) \
	-DNDVIDEO_CODEC_MODULES=$(CODEC_MODULES)
OBJDIR = build/$(if $(filter TRUE,$(DEBUG)),debug,release)/$(if $(filter 1,$(CODEC_MODULES)),modules/)$(CODEC_TAG)
MODULEDIR = build/$(if $(filter TRUE,$(DEBUG)),debug,release)/codec-modules
ifneq ($(wildcard ./external/Ndless-official/ndless-sdk/include/libndls.h),)
SDKROOT ?= ./external/Ndless-official/ndless-sdk
else
SDKROOT ?= ./external/Ndless/ndless-sdk
endif
PYTHON ?= python
PACKZEHN = $(PYTHON) tools/pack_zehn.py
RAW_GXX ?= arm-none-eabi-g++
RAW_GCC ?= arm-none-eabi-gcc
OBJCOPY ?= arm-none-eabi-objcopy
LOADER = $(SDKROOT)/tools/zehn_loader/zehn_loader.tns
LOADER_DIR = $(SDKROOT)/tools/zehn_loader
LOADER_ELF = $(LOADER_DIR)/zehn_loader.tns.elf

export PATH := $(abspath $(SDKROOT)/bin):$(PATH)

GCC = nspire-gcc
AS  = nspire-as
GXX = nspire-g++
LD  = nspire-gcc -nodefaultlibs

GCCFLAGS_BASE = -Wall -Wextra -Wno-unused-parameter -std=c99 -marm -mcpu=arm926ej-s -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections -Isrc -Isrc/codecs/h264bsd -Isrc/codecs -Isrc/codecs/xvid -DARCH_IS_32BIT -DARCH_IS_ARM -DXVID_DECODER_ONLY -DNDVIDEO_XVID_RGB565_ONLY -DNDVIDEO_XVID_NO_POSTPROC
LDFLAGS = -Wl,--gc-sections -lSDL -lm -flto -O3
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
HOT_PLAYER_SRCS = src/player/video_lookahead.c src/player/video_decoder.c src/player/codec_streaming.c src/player/render_primitives.c \
	src/player/playback_ui.c src/player/playback_loop.c src/player/subtitles.c \
	src/player/input_timing_memory.c src/player/night_mode.c \
	src/player/movie_open_scan.c src/player/platform_debug.c
FAST_SRCS = src/codecs/av1/% src/codecs/av1_decoder.c src/codecs/hevc/% src/codecs/hevc_decoder.cpp src/codecs/h264bsd/% src/codecs/xvid/% src/codecs/mpeg4_xvid.c \
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
SOURCES := $(filter-out src/codecs/xvid/% src/codecs/modules/%,$(call source_files,src/,%.c %.cpp %.S))
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
ifeq ($(filter av1,$(SELECTED_CODECS)),)
SOURCES := $(filter-out src/codecs/av1/% src/codecs/av1_decoder.c,$(SOURCES))
endif
ifeq ($(CODEC_MODULES),1)
SOURCES := $(filter-out src/codecs/h264bsd/% src/codecs/xvid/% src/codecs/mpeg4_xvid.c \
    src/codecs/hevc/% src/codecs/hevc_decoder.cpp src/codecs/av1/% src/codecs/av1_decoder.c,$(SOURCES))
MODULE_PROXY_h264 = src/codecs/modules/h264_module_proxy.c
MODULE_PROXY_mpeg4 = src/codecs/modules/mpeg4_module_proxy.c
MODULE_PROXY_hevc = src/codecs/modules/hevc_proxy.c
MODULE_PROXY_av1 = src/codecs/modules/av1_proxy.c
SOURCES += $(foreach codec,$(SELECTED_CODECS),$(MODULE_PROXY_$(codec)))
FAST_SRCS += src/codecs/modules/%
MODULE_FILES = $(foreach codec,$(SELECTED_CODECS),$(MODULEDIR)/$(codec).zehn)
MODULE_ARGS = $(foreach codec,$(SELECTED_CODECS),--module $(codec)=$(MODULEDIR)/$(codec).zehn)
MODULE_MANIFEST = $(OBJDIR)/codec_module_manifest.h
MODULE_SOURCES := $(call source_files,src/codecs/,%.c %.cpp %.S %.h)
CODEC_FLAGS += -I$(OBJDIR)
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
RELEASE_VARIANTS = h264 mpeg4 hevc av1 h264-mpeg4 h264-hevc h264-av1 mpeg4-hevc mpeg4-av1 hevc-av1 \
	h264-mpeg4-hevc h264-mpeg4-av1 h264-hevc-av1 mpeg4-hevc-av1 h264-mpeg4-hevc-av1
RELEASE_TARGETS = $(addprefix release-,$(RELEASE_VARIANTS))

.PHONY: all clean release $(RELEASE_TARGETS)

all: $(PLAYER)
	mkdir -p $(DISTDIR)
	cp "$(PLAYER)" "$(DISTDIR)/$(TNS)"
	cp "$(ELF)" "$(DISTDIR)/$(EXE)$(CODEC_SUFFIX).elf"
	cp "$(ZEHN)" "$(DISTDIR)/$(EXE)$(CODEC_SUFFIX).zehn"
ifeq ($(CODEC_MODULES),1)
	mkdir -p $(DISTDIR)/modules
	cp $(foreach codec,$(SELECTED_CODECS),"$(MODULEDIR)/$(codec).elf" "$(MODULEDIR)/$(codec).zehn") $(DISTDIR)/modules/
endif

$(OBJDIR)/%.o: %.c Makefile
	mkdir -p $(dir $@)
	$(GCC) $(if $(filter src/codecs/av1/% src/codecs/av1_decoder.c,$<),$(AV1_GCCFLAGS),$(if $(filter $(FAST_SRCS),$<),$(FAST_GCCFLAGS),$(GCCFLAGS))) $(if $(filter src/codecs/av1/%_tmpl.c,$<),-DBITDEPTH=8) $(CODEC_FLAGS) $(if $(filter src/codecs/xvid/%,$<),-Wno-incompatible-pointer-types,-Werror=incompatible-pointer-types) -MMD -MP -c $< -o $@

AV1_GCCFLAGS = $(filter-out -std=c99,$(FAST_GCCFLAGS)) -std=c11 -DDAV1D_STATIC \
	-Isrc/codecs/av1/dav1d -Isrc/codecs/av1/dav1d/include

HEVC_GXXFLAGS = $(filter-out -std=c99,$(FAST_GCCFLAGS)) -std=c++11 -fno-exceptions -fno-rtti \
    -DLIBDE265_STATIC_BUILD -DHAVE_STDINT_H -DHAVE_ALLOCA_H -Isrc/codecs/hevc

$(OBJDIR)/%.o: %.cpp Makefile
	mkdir -p $(dir $@)
	$(GXX) $(HEVC_GXXFLAGS) $(CODEC_FLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.o: %.S Makefile
	mkdir -p $(dir $@)
	$(AS) $(CODEC_FLAGS) -c $< -o $@

$(ELF): $(OBJS)
	$(LD) $^ -o $@ $(LDFLAGS) $(if $(filter 0,$(CODEC_MODULES)),$(if $(filter hevc,$(SELECTED_CODECS)),-lstdc++))

$(LOADER):
	cd $(LOADER_DIR) && $(RAW_GXX) $(LOADER_GXXFLAGS) loader.cpp -o zehn_loader.tns.elf
	$(OBJCOPY) --set-section-flags .pad=alloc,load,contents -O binary $(LOADER_ELF) $(LOADER)

ifeq ($(CODEC_MODULES),1)
$(MODULEDIR)/%.zehn: $(MODULE_SOURCES) src/sram.h tools/build_codec_modules.py tools/codec_module.ld tools/pack_zehn.py Makefile
	$(PYTHON) tools/build_codec_modules.py --codecs $* --output-dir "$(MODULEDIR)" --sdk "$(SDKROOT)" --cc "$(RAW_GCC)" --cxx "$(RAW_GXX)" --flags="$(FAST_GCCFLAGS)" --jobs 2

$(MODULE_MANIFEST): $(MODULE_FILES) tools/bundle_codec_modules.py
	mkdir -p $(dir $@)
	$(PYTHON) tools/bundle_codec_modules.py --manifest "$@" $(MODULE_ARGS)

$(OBJS): $(MODULE_MANIFEST)

$(PLAYER): $(ELF) $(LOADER) $(MODULE_FILES) tools/pack_zehn.py tools/bundle_codec_modules.py
	$(PACKZEHN) --input $< --output "$(OBJDIR)/host.tns" --zehn-output "$(OBJDIR)/host.zehn" --loader $(LOADER) $(PACKFLAGS)
	$(PYTHON) tools/bundle_codec_modules.py --host "$(OBJDIR)/host.zehn" --output "$(ZEHN)" $(MODULE_ARGS)
	$(PYTHON) tools/bundle_codec_modules.py --host "$(OBJDIR)/host.tns" --output "$@" $(MODULE_ARGS)
else
$(PLAYER): $(ELF) $(LOADER) tools/pack_zehn.py
	$(PACKZEHN) --input $< --output "$@" --zehn-output $(ZEHN) --loader $(LOADER) $(PACKFLAGS)
endif

$(RELEASE_TARGETS): release-%:
	$(MAKE) CODECS="$(subst -, ,$*)" all

release: $(RELEASE_TARGETS)
	$(PYTHON) tools/package_release.py --dist "$(DISTDIR)" --output "$(RELEASEDIR)" --notes build/release-notes.md

clean:
	rm -f $(OBJS) $(LEGACY_OBJS) $(PLAYER) $(ELF) $(ZEHN) "$(DISTDIR)/$(TNS)" $(DISTDIR)/$(EXE)$(CODEC_SUFFIX).elf $(DISTDIR)/$(EXE)$(CODEC_SUFFIX).zehn
	rm -f $(OBJS:.o=.d)
ifeq ($(CODEC_MODULES),1)
	rm -f "$(MODULE_MANIFEST)" "$(OBJDIR)/host.tns" "$(OBJDIR)/host.zehn"
endif

-include $(OBJS:.o=.d)
