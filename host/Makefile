# Host build (macOS/Linux, SDL2) of the ESP32-S3 configuration: one binary with the engine,
# filesystem, software renderer, menu and the hlsdk client and server linked in statically.
#
# Each game library is linked into one relocatable object that only exports its
# lib_<name>_exports table (engine/platform/misc/lib_static.c looks them up by name), so the
# libraries can keep their own copies of the same functions (pm_shared, UTIL_...).

ROOT  := ..
XASH  ?= $(ROOT)/xash3d-fwgs
HLSDK ?= $(ROOT)/hlsdk-portable
OBJ   ?= obj
BIN   ?= hlhost

UNAME := $(shell uname)
CC    ?= cc
CXX   ?= c++
LD      ?= ld
OBJCOPY ?= objcopy
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS   ?= $(shell sdl2-config --libs)
# ESP_HEAP=1: the allocator of the device (ESP-IDF multi_heap) for the code of the binary
# (port/host_mem.c HOST_ESP_HEAP), with IDF_HEAP its sources
ESP_HEAP ?=
IDF_HEAP ?= $(firstword $(wildcard $(HOME)/.platformio/packages/framework-espidf*/components/heap) $(IDF_PATH)/components/heap)

OPT      ?= -O2 -g
SAN      ?=
# limits of the port, for all the parts (struct layouts shared between the engine and the libraries)
PORT_DEFS := -DHISTORY_MAX=4 -DVOICE_HEAD_MODELS=4 -DSOUND_DMA_SPEED=22050 -DDEFAULT_MAX_EDICTS=900 -DMAX_PHYSENTS=128 -DXASH_FS_CASE_INSENSITIVE=1
COMMON   := $(OPT) $(SAN) -fno-strict-aliasing -fno-common -MMD -MP $(PORT_DEFS)
CFLAGS_  := $(COMMON) -std=gnu11
CXXFLAGS_:= $(COMMON) -std=gnu++11 -fno-exceptions -Wno-deprecated -Wno-register

# ---- source lists (shared with the ESP-IDF build) ----
src = $(patsubst port/%,$(ROOT)/port/%,$(patsubst hlsdk/%,$(HLSDK)/%,$(patsubst xash/%,$(XASH)/%,$(shell cat $(ROOT)/sources/$(1).txt))))

LIBS := filesystem_stdio ref_soft menu client server

# ---- defines ----
XASH_DEFS := -DXASH_GAMEDIR=\"valve\" -DXASH_LOW_MEMORY=2 -DXASH_STATIC_LIBS=1 \
	-DXASH_MINIZ=1 -DMINIZ_NO_ARCHIVE_APIS -D_GNU_SOURCE=1 -DALLOCA_H="<alloca.h>" \
	-DHAVE_TGMATH_H=1 -DHAVE_STRNCASECMP=1 -DHAVE_STRCASECMP=1 -DHAVE_STRCASESTR=1 \
	-DHAVE_STRNLEN=1 -DHAVE_LARGEFILE=1 \
	-DXASH_BUILD_COMMIT=\"esp32s3\" -DXASH_BUILD_BRANCH=\"esp32s3\" -DXASH_BUILD_COMMIT_DATE=\"\"
ifneq ($(UNAME),Darwin)
XASH_DEFS += -DHAVE_STRCHRNUL=1
else
XASH_DEFS += -DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1
endif
XASH_INC := -I$(XASH)/public -I$(XASH)/common -I$(XASH)/pm_shared -I$(XASH)/engine \
	-I$(XASH)/3rdparty/library_suffix/include -I$(XASH)/3rdparty/miniz

engine_FLAGS := $(XASH_DEFS) -DXASH_PARALLEL2=1 -DENGINE_DLL=1 -DXASH_REF_SOFT_ENABLED=1 -DXASH_SDL=2 -DXASH_ENABLE_MAIN=1 -DXASH_PORT_INPUT=1 -DXASH_MAPPED_FILES=1 \
	$(SDL_CFLAGS) \
	-I$(XASH)/engine/server -I$(XASH)/engine/client -I$(XASH)/engine/client/vgui \
	-I$(XASH)/engine/common/soundlib -I$(XASH)/engine/platform -I$(XASH)/engine/common \
	-I$(XASH)/engine/common/imagelib -I$(XASH)/filesystem $(XASH_INC) \
	-I$(XASH)/3rdparty/MultiEmulator/include -I$(XASH)/3rdparty/bzip2/bzip2 -I$(OBJ)/gen

filesystem_stdio_FLAGS := $(XASH_DEFS) -I$(XASH)/filesystem $(XASH_INC)
ifneq ($(UNAME),Darwin)
filesystem_stdio_FLAGS += -DHAVE_DIRENT_D_TYPE=1
endif
ref_soft_FLAGS := $(XASH_DEFS) -DREF_DLL=1 -DREF_CONST_TABLES=1 -I$(XASH)/ref/soft -I$(XASH)/ref/common \
	-I$(XASH)/engine/common -I$(XASH)/engine/common/imagelib -I$(XASH)/filesystem $(XASH_INC)
# text menu (port/menu_text.c)
menu_FLAGS := $(XASH_DEFS) -I$(XASH)/engine $(XASH_INC)
HL_DEFS := -DHAVE_TGMATH_H=1 -DHAVE_CMATH=1 -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp \
	-D_snprintf=snprintf -D_vsnprintf=vsnprintf -D_LINUX=1 -DLINUX=1 -DCLIENT_WEAPONS=1 \
	-DCROWBAR_FIX_RAPID_CROWBAR=1 -DSPEAKABLE_TARGETS=1 -DHD_HUD_DEFAULT=1 \
	$(if $(filter Darwin,$(UNAME)),-DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1)
HL_INC := -I$(HLSDK)/common -I$(HLSDK)/engine -I$(HLSDK)/pm_shared -I$(HLSDK)/game_shared \
	-I$(HLSDK)/public
server_FLAGS := $(HL_DEFS) -I$(HLSDK)/dlls $(HL_INC)
client_FLAGS := $(HL_DEFS) -DCLIENT_DLL -DINTERNAL_VGUI_SUPPORT -DNO_VGUI_VIEWPORT=1 -I$(HLSDK)/cl_dll -I$(HLSDK)/dlls \
	$(HL_INC) -I$(HLSDK)/utils/vgui/include -I$(HLSDK)/freevgui
# freevgui (built into the client) uses its own headers instead of utils/vgui/include
freevgui_FLAGS := $(HL_DEFS) -DINTERNAL_VGUI_SUPPORT -I$(HLSDK)/freevgui -I$(HLSDK)/engine

# ---- objects ----
# object path: $(OBJ)/<lib>/<source path with ../ removed>.o
objs = $(addprefix $(OBJ)/$(1)/,$(addsuffix .o,$(subst ../,,$(2))))

ENGINE_SRC := $(call src,engine) $(call src,engine_host)
ifneq ($(UNAME),Darwin)
ENGINE_SRC := $(filter-out %/apple/id_apple.c,$(ENGINE_SRC)) $(XASH)/engine/platform/linux/sys_linux.c
endif
ifeq ($(ESP_HEAP),1)
ENGINE_SRC += $(IDF_HEAP)/multi_heap.c $(IDF_HEAP)/tlsf/tlsf.c
engine_FLAGS += -DHOST_ESP_HEAP=1 -I$(IDF_HEAP)/include -I$(IDF_HEAP)/tlsf -I$(IDF_HEAP)/tlsf/include -I$(IDF_HEAP)
endif
ENGINE_OBJ := $(call objs,engine,$(ENGINE_SRC))

define LIB_template
$(1)_SRC := $$(call src,$(1))
$(1)_OBJ := $$(call objs,$(1),$$($(1)_SRC)) $(OBJ)/$(1)/exports.o

$(OBJ)/$(1)/exports.c: $(ROOT)/host/exports/$(1).txt Makefile
	@mkdir -p $$(@D)
	@{ sed 's/.*/extern void &(void);/' $$<; echo 'struct { const char *name; void *func; } lib_$(1)_exports[] = {'; \
	  sed 's/.*/{ "&", (void *)\&& },/' $$<; echo '{ 0, 0 } };'; } > $$@

$(OBJ)/$(1)/exports.o: $(OBJ)/$(1)/exports.c
	$$(CC) -c -O1 -w $$< -o $$@

$(OBJ)/$(1).o: $$($(1)_OBJ)
	@printf '%s\n' $$^ > $(OBJ)/$(1).list
	$$(LD_R) $(OBJ)/$(1).list $(1) $$@

$$(foreach s,$$($(1)_SRC),$$(eval $$(call OBJ_rule,$(1),$$(s))))
endef

COMPILE.c   = $(CC) $(CFLAGS_)
COMPILE.cpp = $(CXX) $(CXXFLAGS_)

define OBJ_rule
$(call objs,$(1),$(2)): $(2) Makefile
	@mkdir -p $$(@D)
	$$(COMPILE$(suffix $(2))) $(if $(findstring /freevgui/,$(2)),$$(freevgui_FLAGS),$$($(1)_FLAGS)) -c $$< -o $$@
endef

# relocatable link keeping only lib_<name>_exports global
ifeq ($(UNAME),Darwin)
LD_R = ld_r() { ld -r -filelist $$1 -exported_symbol _lib_$$2_exports -o $$3; }; ld_r
else
LD_R = ld_r() { $(LD) -r --force-group-allocation @$$1 -o $$3.tmp && $(OBJCOPY) -G lib_$$2_exports $$3.tmp $$3 && rm $$3.tmp; }; ld_r
endif

$(foreach l,$(LIBS),$(eval $(call LIB_template,$(l))))

$(foreach s,$(ENGINE_SRC),$(eval $(call OBJ_rule,engine,$(s))))

LINKLIBS := $(SDL_LIBS)
ifeq ($(UNAME),Darwin)
LINKLIBS += -framework IOKit -framework CoreFoundation
else
# libstdc++ in the binary: its allocations (operator new) are those of the binary (HOST_ESP_HEAP)
LINKLIBS += -lm -lpthread -ldl -l:libbsd.so.0 -static-libstdc++
endif

.DEFAULT_GOAL := all
all: $(BIN)

$(OBJ)/gen/generated_library_tables.h: Makefile
	@mkdir -p $(@D)
	@{ for l in $(LIBS); do echo "extern table_t lib_$${l}_exports[];"; done; \
	   echo 'struct { const char *name; void *func; } libs[] = {'; \
	   for l in $(LIBS); do echo "{ \"$$l\", &lib_$${l}_exports },"; done; echo '{ 0, 0 } };'; } > $@

$(OBJ)/engine/$(subst ../,,$(XASH))/engine/platform/misc/lib_static.c.o: $(OBJ)/gen/generated_library_tables.h

# constant tables of the software renderer (REF_CONST_TABLES)
$(OBJ)/gen/reftables.c: $(ROOT)/tools/gen_reftables.py
	@mkdir -p $(@D)
	python3 $< $@

$(OBJ)/gen/reftables.o: $(OBJ)/gen/reftables.c
	$(CC) -c -O1 $< -o $@

$(BIN): $(ENGINE_OBJ) $(OBJ)/gen/reftables.o $(addprefix $(OBJ)/,$(addsuffix .o,$(LIBS)))
	$(CXX) $(SAN) -o $@ $^ $(LINKLIBS)

clean:
	rm -rf $(OBJ) $(BIN)

.PHONY: all clean

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
