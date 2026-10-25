# libVkLayer_bng_fsr4.so, its Vulkan manifest, the FSR 4.1.1 assets and the menu-label BeamNG mod.
#   make && make install PREFIX=~/.local
# The manifest points at the installed library by absolute path, so PREFIX must be the final location.

PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
DATADIR  ?= $(PREFIX)/share
ASSETDIR ?= $(DATADIR)/bng-fsr4/assets

CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-missing-field-initializers -Ifsr411
LDFLAGS  += -shared -Wl,--no-undefined -Wl,-z,defs

LIB      = build/libVkLayer_bng_fsr4.so
MANIFEST = build/VkLayer_bng_fsr4.json
MOD      = build/bng_fsr4_linux.zip
OBJS     = build/bng_fsr4_layer.o build/fsr411.o

all: $(LIB) $(MANIFEST) $(MOD)

build/bng_fsr4_layer.o: layer/bng_fsr4_layer.cpp fsr411/fsr411.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DBNG_FSR4_ASSET_DIR='"$(ASSETDIR)"' -c $< -o $@

build/fsr411.o: fsr411/fsr411.cpp fsr411/fsr411.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

$(MANIFEST): packaging/VkLayer_bng_fsr4.json.in
	@mkdir -p build
	sed 's|@LIBRARY@|$(LIBDIR)/libVkLayer_bng_fsr4.so|' $< > $@

$(MOD): $(shell find game-mod -type f)
	@mkdir -p build
	rm -f $@
	cd game-mod/bng_fsr4_linux && zip -qrX ../../$@ lua scripts locales

install: all
	install -Dm755 $(LIB) $(DESTDIR)$(LIBDIR)/libVkLayer_bng_fsr4.so
	install -Dm644 $(MANIFEST) $(DESTDIR)$(DATADIR)/vulkan/implicit_layer.d/VkLayer_bng_fsr4.json
	install -Dm644 $(MOD) $(DESTDIR)$(DATADIR)/bng-fsr4/bng_fsr4_linux.zip
	mkdir -p $(DESTDIR)$(ASSETDIR)
	cp -r assets/t1080_m0 assets/t1080_m1 assets/t2160_m0 assets/t2160_m1 assets/NOTICE.md $(DESTDIR)$(ASSETDIR)/

clean:
	rm -rf build

.PHONY: all install clean
