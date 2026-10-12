.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif
DEVKITARM ?= $(DEVKITPRO)/devkitARM
export DEVKITPRO DEVKITARM

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	biohazard
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=	source

APP_TITLE	:=	Resident Evil Degeneration
APP_DESCRIPTION	:=	Game data in sdmc:/reDe
APP_AUTHOR	:=	KiddRwxSsj

# makerom and bannertool from tools/bin if they're there, otherwise from PATH
MAKEROM		?=	$(firstword $(wildcard $(TOPDIR)/tools/bin/makerom) makerom)
BANNERTOOL	?=	$(firstword $(wildcard $(TOPDIR)/tools/bin/bannertool) bannertool)
META		:=	$(TOPDIR)/meta

ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations -ffunction-sections \
		-fno-strict-aliasing $(ARCH) $(INCLUDE) -D__3DS__
ifneq ($(HEAP_LIMIT_MB),)
CFLAGS	+=	-DHEAP_LIMIT_MB=$(HEAP_LIMIT_MB)
endif
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	-llzma -lcitro3d -lctru -lm
LIBDIRS	:=	$(PORTLIBS) $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))

export LD	:=	$(CC)
export OFILES_BIN	:=	$(PICAFILES:.v.pica=.shbin.o)
export OFILES_SOURCES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES	:=	$(OFILES_BIN) $(OFILES_SOURCES)
export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).cia $(TARGET).elf

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).cia	:	$(OUTPUT).elf icon.icn banner.bnr
	@$(MAKEROM) -f cia -o $@ -elf $(OUTPUT).elf -rsf $(META)/app.rsf -icon icon.icn -banner banner.bnr -target t -exefslogo
	@echo built ... $(notdir $@)

icon.icn	:	$(META)/icon.png $(TOPDIR)/Makefile
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $< -o $@ >/dev/null

banner.bnr	:	$(META)/banner.png $(META)/banner.wav
	@$(BANNERTOOL) makebanner -i $(META)/banner.png -a $(META)/banner.wav -o $@ >/dev/null

$(OFILES_SOURCES)	:	$(HFILES)
$(OUTPUT).elf	:	$(OFILES)

.PRECIOUS	:	%.shbin

%.shbin.o %_shbin.h : %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

endif
