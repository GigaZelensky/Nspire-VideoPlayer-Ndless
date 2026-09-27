.DEFAULT_GOAL := all
DEBUG = FALSE
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
LDFLAGS += -Wl,--wrap=fopen,--wrap=fread,--wrap=fwrite,--wrap=fseek,--wrap=fclose,--wrap=fflush,--wrap=_open,--wrap=_read,--wrap=_write,--wrap=_lseek,--wrap=_close,--wrap=remove,--wrap=rename,--wrap=nuc_opendir,--wrap=nuc_readdir,--wrap=nuc_closedir
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
HOT_PLAYER_SRCS = src/player/h264_lookahead.c src/player/codec_streaming.c src/player/render_primitives.c \
	src/player/playback_ui.c src/player/playback_loop.c src/player/subtitles.c \
	src/player/input_timing_memory.c src/player/night_mode.c \
	src/player/movie_open_scan.c src/player/platform_debug.c
FAST_SRCS = src/codecs/h264bsd/% src/codecs/xvid/% src/codecs/mpeg4_xvid.c \
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

OBJS = $(patsubst %.c, %.o, $(shell find src -type d -name '.*' -prune -o -type f -name "*.c" -not -path "src/codecs/xvid/*" -print))
OBJS += $(patsubst %.c, %.o, $(XVID_DECODER_SRCS))
OBJS += $(patsubst %.cpp, %.o, $(shell find src -type d -name '.*' -prune -o -type f -name "*.cpp" -print))
OBJS += $(patsubst %.S, %.o, $(shell find src -type d -name '.*' -prune -o -type f -name "*.S" -print))
EXE = ndvideo
DISTDIR = dist
LEGACY_OBJS = src/player.o
vpath %.tns $(DISTDIR)
vpath %.elf $(DISTDIR)

.PHONY: all clean

all: $(EXE).tns

%.o: %.c
	$(GCC) $(if $(filter $(FAST_SRCS),$<),$(FAST_GCCFLAGS),$(GCCFLAGS)) $(if $(filter src/codecs/xvid/%,$<),-Wno-incompatible-pointer-types,-Werror=incompatible-pointer-types) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(GXX) $(GCCFLAGS) -MMD -MP -c $< -o $@

%.o: %.S
	$(AS) -c $< -o $@

$(EXE).elf: $(OBJS)
	mkdir -p $(DISTDIR)
	$(LD) $^ -o $(DISTDIR)/$@ $(LDFLAGS)

$(LOADER):
	cd $(LOADER_DIR) && $(RAW_GXX) $(LOADER_GXXFLAGS) loader.cpp -o zehn_loader.tns.elf
	$(OBJCOPY) --set-section-flags .pad=alloc,load,contents -O binary $(LOADER_ELF) $(LOADER)

$(EXE).tns: $(EXE).elf $(LOADER)
	$(PACKZEHN) --input $(DISTDIR)/$< --output $(DISTDIR)/$@ --zehn-output $(DISTDIR)/$(EXE).zehn --loader $(LOADER) $(PACKFLAGS)

clean:
	rm -f $(OBJS) $(LEGACY_OBJS) $(DISTDIR)/$(EXE).tns $(DISTDIR)/$(EXE).elf $(DISTDIR)/$(EXE).zehn
	rm -f $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
