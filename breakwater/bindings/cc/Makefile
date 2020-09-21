# Makefile for C++ binidngs
ROOT_PATH=../../..
include $(ROOT_PATH)/build/shared.mk

CXXFLAGS += -I$(ROOT_PATH)/breakwater/inc
CXXFLAGS += -I$(ROOT_PATH)/breakwater/bindings/cc/inc

# libbw+++.a - the c++ breakwater library
bw_src = rpc++.cc
bw_obj = $(bw_src:.cc=.o)

all: libbw++.a

libbw++.a: $(bw_obj)
	$(AR) rcs $@ $^

#general build rules for all targets
src = $(bw_src)
obj = $(src:.cc=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)		# include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
%.d: %.cc
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) libbw++.a
