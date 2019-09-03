# Makefile for netbench
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

streamcluster_src = streamcluster.cc
streamcluster_obj = $(streamcluster_src:.cc=.o)

lib_shim = $(ROOT_PATH)/shim/libshim.a -ldl

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

CXXFLAGS += -DENABLE_THREADS -march=native
LDFLAGS += -Wl,--wrap=main -no-pie

RUNTIME_LIBS := $(RUNTIME_LIBS)

# must be first
all: streamcluster

streamcluster: $(streamcluster_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(streamcluster_obj) \
	$(lib_shim) $(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(streamcluster_src)
obj = $(src:.cc=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.cc
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) streamcluster
