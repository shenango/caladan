# Makefile for netbench
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

synthetic_src = synthetic_worker.cc
synthetic_obj = $(synthetic_src:.cc=.o)

netbench_src = netbench.cc
netbench_obj = $(netbench_src:.cc=.o)

stress_src = stress.cc
stress_obj = $(stress_src:.cc=.o)

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma

# must be first
all: netbench stress

netbench: $(synthetic_obj) $(netbench_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(synthetic_obj) $(netbench_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

stress: $(synthetic_obj) $(stress_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(synthetic_obj) $(stress_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(netbench_src) $(stress_src) $(synthetic_src)
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
	rm -f $(obj) $(dep) netbench stress
