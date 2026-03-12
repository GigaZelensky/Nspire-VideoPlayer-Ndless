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
SDK_ZLIB = $(SDKROOT)/lib/libz.a
SDK_ZLIB_HEADERS = $(SDKROOT)/include/zlib.h $(SDKROOT)/include/zconf.h

export PATH := $(abspath $(SDKROOT)/bin):$(PATH)

GCC = nspire-gcc
AS  = nspire-as
GXX = nspire-g++
LD  = nspire-ld

GCCFLAGS_BASE = -Wall -Wextra -Wno-unused-parameter -std=c99 -marm -mcpu=arm926ej-s -mtune=arm926ej-s -mfloat-abi=soft -ffunction-sections -fdata-sections -Isrc/h264bsd
LDFLAGS = -Wl,--gc-sections -lSDL -lz -flto -O3
LOADER_GXXFLAGS = -g -Os -Wall -Wextra -march=armv5te -fPIE -std=c++11 -fno-rtti -fno-exceptions -Wl,-Tldscript -Wl,--gc-sections -nostdlib -nostartfiles -ffreestanding -I ../../include
PACKFLAGS = --name "ND Video Player" --author "GigaZelensky" --version 1 --ndless-min 45 --hww-support --no-uses-lcd-blit

ifeq ($(DEBUG),FALSE)
	GCCFLAGS = $(GCCFLAGS_BASE) -Os
	FAST_GCCFLAGS = $(GCCFLAGS_BASE) -O3 -fno-strict-aliasing -fomit-frame-pointer -falign-functions=32 -falign-loops=32 -flto -funroll-loops
else
	GCCFLAGS = $(GCCFLAGS_BASE) -O0 -g
	FAST_GCCFLAGS = $(GCCFLAGS_BASE) -O0 -g -falign-functions=32 -falign-loops=32
endif

OBJS = $(patsubst %.c, %.o, $(shell find src -name \*.c))
OBJS += $(patsubst %.cpp, %.o, $(shell find src -name \*.cpp))
OBJS += $(patsubst %.S, %.o, $(shell find src -name \*.S))
EXE = ndvideo
DISTDIR = dist
vpath %.tns $(DISTDIR)
vpath %.elf $(DISTDIR)

all: $(EXE).tns

$(SDK_ZLIB) $(SDK_ZLIB_HEADERS):
	$(MAKE) -C $(SDKROOT)/thirdparty zlib

$(OBJS): $(SDK_ZLIB_HEADERS)

%.o: %.c
	$(GCC) $(if $(filter src/h264bsd/% src/player.c,$<),$(FAST_GCCFLAGS),$(GCCFLAGS)) -c $< -o $@

%.o: %.cpp
	$(GXX) $(GCCFLAGS) -c $< -o $@

%.o: %.S
	$(AS) -c $< -o $@

$(EXE).elf: $(OBJS) $(SDK_ZLIB)
	mkdir -p $(DISTDIR)
	$(LD) $^ -o $(DISTDIR)/$@ $(LDFLAGS)

$(LOADER):
	cd $(LOADER_DIR) && $(RAW_GXX) $(LOADER_GXXFLAGS) loader.cpp -o zehn_loader.tns.elf
	$(OBJCOPY) --set-section-flags .pad=alloc,load,contents -O binary $(LOADER_ELF) $(LOADER)

$(EXE).tns: $(EXE).elf $(LOADER)
	$(PACKZEHN) --input $(DISTDIR)/$< --output $(DISTDIR)/$@ --zehn-output $(DISTDIR)/$(EXE).zehn --loader $(LOADER) $(PACKFLAGS)

clean:
	rm -f $(OBJS) $(DISTDIR)/$(EXE).tns $(DISTDIR)/$(EXE).elf $(DISTDIR)/$(EXE).zehn
