# Makefile for netbench
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

storage_server_src = storage_server.cc
storage_server_obj = $(storage_server_src:.cc=.o)

src = $(storage_server_src)
obj = $(src:.cc=.o)
dep = $(obj:.o=.d)

# must be first
all: storage_server

storage_server: $(storage_server_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(obj) $(librt_libs) $(RUNTIME_LIBS)

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
	rm -f $(obj) $(dep) storage_server
