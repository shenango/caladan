# Makefile for C++ bindings
ROOT_PATH=../..
include $(ROOT_PATH)/build/shared.mk

# librt++.a - the c++ runtime library
rt_src = runtime.cc thread.cc net.cc
rt_obj = $(rt_src:.cc=.o)

test_src = test.cc
test_obj = $(test_src:.cc=.o)

# must be first
all: librt++.a test

librt++.a: $(rt_obj)
	$(AR) rcs $@ $^

test: $(test_obj) librt++.a $(RUNTIME_DEPS)
	$(LDXX) $(LDFLAGS) -o $@ $(test_obj) librt++.a $(RUNTIME_LIBS)

# general build rules for all targets
src = $(rt_src) $(test_src)
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
	rm -f $(obj) $(dep) librt++.a test
