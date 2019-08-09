# Makefile for netbench
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

lib_src = synthetic_worker.cc distribution.cc util.cc
lib_obj = $(lib_src:.cc=.o)

netbench_src = netbench.cc
netbench_obj = $(netbench_src:.cc=.o)

stress_src = stress.cc
stress_obj = $(stress_src:.cc=.o)

stress_linux_src = stress_linux.cc
stress_linux_obj = $(stress_linux_src:.cc=.o)

stress_shm_src = stress_shm.cc
stress_shm_obj = $(stress_shm_src:.cc=.o)

stress_shm_query_src = stress_shm_query.cc
stress_shm_query_obj = $(stress_shm_query_src:.cc=.o)

interference_src = interference.cc
interference_obj = $(interference_src:.cc=.o)

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma

# must be first
all: netbench stress interference stress_linux stress_shm stress_shm_query

netbench: $(lib_obj) $(netbench_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

stress: $(lib_obj) $(stress_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(stress_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

stress_linux: $(lib_obj) $(stress_linux_obj) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(stress_linux_obj) $(RUNTIME_LIBS)

stress_shm: $(lib_obj) $(stress_shm_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(stress_shm_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

stress_shm_query: $(lib_obj) $(stress_shm_query_obj) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(stress_shm_query_obj) $(RUNTIME_LIBS)

interference: $(lib_obj) $(interference_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(interference_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(lib_src) $(netbench_src) $(stress_src) $(interference_src) $(stress_linux_src) \
        $(stress_shm_src) $(stress_shm_query_src)
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
	rm -f $(obj) $(dep) netbench stress interference stress_linux stress_shm stress_shm_query
