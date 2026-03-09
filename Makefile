DEBUG = FALSE
SDKROOT ?= ./external/Ndless/ndless-sdk
PYTHON ?= python
PACKZEHN = $(PYTHON) tools/pack_zehn.py
LOADER = $(SDKROOT)/tools/zehn_loader/zehn_loader.tns

export PATH := $(abspath $(SDKROOT)/bin):$(PATH)

GCC = nspire-gcc
AS  = nspire-as
GXX = nspire-g++
LD  = nspire-ld

GCCFLAGS = -Wall -Wextra -Wno-unused-parameter -std=c99 -marm -ffunction-sections -fdata-sections
LDFLAGS = -Wl,--gc-sections -lSDL_ttf -lfreetype -lSDL -lz
PACKFLAGS = --name "ND Video Player" --author "GigaZelensky" --version 1 --ndless-min 45

ifeq ($(DEBUG),FALSE)
	GCCFLAGS += -Os
else
	GCCFLAGS += -O0 -g
endif

OBJS = $(patsubst %.c, %.o, $(shell find src -name \*.c))
OBJS += $(patsubst %.cpp, %.o, $(shell find src -name \*.cpp))
OBJS += $(patsubst %.S, %.o, $(shell find src -name \*.S))
EXE = ndvideo
DISTDIR = dist
vpath %.tns $(DISTDIR)
vpath %.elf $(DISTDIR)

all: $(EXE).tns

%.o: %.c
	$(GCC) $(GCCFLAGS) -c $< -o $@

%.o: %.cpp
	$(GXX) $(GCCFLAGS) -c $< -o $@

%.o: %.S
	$(AS) -c $< -o $@

$(EXE).elf: $(OBJS)
	mkdir -p $(DISTDIR)
	$(LD) $^ -o $(DISTDIR)/$@ $(LDFLAGS)

$(LOADER):
	$(MAKE) -C $(SDKROOT)/tools/zehn_loader zehn_loader.tns

$(EXE).tns: $(EXE).elf $(LOADER)
	$(PACKZEHN) --input $(DISTDIR)/$< --output $(DISTDIR)/$@ --zehn-output $(DISTDIR)/$(EXE).zehn --loader $(LOADER) $(PACKFLAGS)

clean:
	rm -f $(OBJS) $(DISTDIR)/$(EXE).tns $(DISTDIR)/$(EXE).elf $(DISTDIR)/$(EXE).zehn
