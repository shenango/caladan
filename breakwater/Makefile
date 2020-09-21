ROOT_PATH=..
include $(ROOT_PATH)/build/shared.mk

CFLAGS += -I$(ROOT_PATH)/breakwater/inc

# breakwater - RPC layer with server overload control
breakwater_src = $(wildcard src/*.c)
breakwater_obj = $(breakwater_src:.c=.o)

all: libbw.a

libbw.a: $(breakwater_obj)
	$(AR) rcs $@ $^

# general build rules for all targets
src = $(breakwater_src)
obj = $(src:.c=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)		# include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
%.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.c
	@$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) libbw.a
