# Makefile for benchmarks

BASEPATH = ../..
CXXPATH = ../../bindings/cc
INC     = -I../../inc -I../../bindings/cc
CXXFLAGS  = -g -Wall -std=gnu++11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T../../base/base.ld

LD	= g++
CC	= g++
AR	= ar

ifneq ($(DEBUG),)
CXXFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
CXXFLAGS += -DNDEBUG -O3
endif

# handy for debugging
print-%  : ; @echo $* = $($*)

synthetic_worker_src = synthetic_worker.cc
synthetic_worker_obj = $(synthetic_worker_src:.cc=.o)

netbench_src = netbench.cc
netbench_obj = $(netbench_src:.cc=.o)

stress_src = stress.cc
stress_obj = $(stress_src:.cc=.o)

librt_libs = $(CXXPATH)/librt++.a $(BASEPATH)/libruntime.a \
	     $(BASEPATH)/libnet.a $(BASEPATH)/libbase.a

# must be first
all: netbench stress

netbench: $(synthetic_worker_obj) $(netbench_obj) $(librt_libs)
	$(LD) -o $@ $(LDFLAGS) $(synthetic_worker_obj) $(netbench_obj) \
	$(librt_libs) -lpthread

stress: $(synthetic_worker_obj) $(stress_obj) $(librt_libs)
	$(LD) -o $@ $(LDFLAGS) $(synthetic_worker_obj) $(stress_obj) \
	$(librt_libs) -lpthread

# general build rules for all targets
src = $(netbench_src) $(stress_src) $(synthetic_worker_src)
obj = $(src:.cc=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.cc
	@$(CC) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.cc
	$(CC) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) netbench stress
