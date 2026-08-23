# OpenHypervisor - clean-room Hypervisor.framework reimplementation (arm64)
CC      := clang++
ARCH    := arm64
CXXFLAGS := -std=c++17 -O0 -g -Wall -Wextra -arch $(ARCH) -fno-omit-frame-pointer \
            -Iinclude -Isrc -D_GNU_SOURCE -MMD -MP
LDFLAGS := -arch $(ARCH) -dynamiclib -install_name @rpath/libopenhyp.dylib

SRCS := src/ohv_trap.cpp src/ohv_state.cpp src/ohv_vm.cpp src/ohv_vcpu.cpp \
        src/ohv_gic.cpp src/ohv_misc.cpp src/ohv_sysreg_apple.cpp src/ohv_sysreg_table.cpp \
        src/ohv_caps_field.cpp src/ohv_extras.cpp
OBJS := $(SRCS:.cpp=.o)

all: libopenhyp.dylib smoke smoke_static

# Single-binary variant: no dyld involvement at all (library validation proof).
smoke_static: $(OBJS) tests/smoke.cpp
	$(CC) $(CXXFLAGS) -o tests/smoke_static tests/smoke.cpp $(OBJS)

libopenhyp.dylib: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CC) $(CXXFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

# Dylib-linked variant. Loading an ad-hoc dylib requires disabling library
# validation in the entitlements (tests/hv.ent) plus amfidont (see
# tools/run-smoke.sh); the static variant (smoke_static) has no such needs.
smoke: tests/smoke.cpp libopenhyp.dylib
	$(CC) $(CXXFLAGS) -o tests/smoke_bin tests/smoke.cpp -L. -lopenhyp
	install_name_tool -change @rpath/libopenhyp.dylib $(CURDIR)/tests/libopenhyp.dylib tests/smoke_bin
	cp -f libopenhyp.dylib tests/libopenhyp.dylib
	codesign --force --sign - --entitlements tests/hv.ent tests/smoke_bin
	codesign --force --sign - --entitlements tests/hv.ent tests/libopenhyp.dylib

clean:
	rm -f $(OBJS) libopenhyp.dylib tests/smoke_bin

.PHONY: all clean smoke