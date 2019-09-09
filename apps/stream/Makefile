# Makefile for netbench
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

stream_src = stream.cc
stream_obj = $(stream_src:.cc=.o)

stream_query_src = stream_query.cc
stream_query_obj = $(stream_query_src:.cc=.o)

lib_shim = $(ROOT_PATH)/shim/libshim.a -ldl

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS)

# must be first
all: stream stream_query stream_linux

stream: $(stream_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(stream_obj) \
	-Wl,--wrap=main $(lib_shim) $(librt_libs) $(RUNTIME_LIBS)

stream_linux: $(stream_obj)
	$(LDXX) -o $@ $(LDFLAGS) $(stream_obj) -lpthread

stream_query: $(stream_query_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(stream_query_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(stream_src) $(stream_query_src)
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
	rm -f $(obj) $(dep) stream stream_query stream_linux
