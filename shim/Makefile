INC     = -I../inc
CFLAGS  = -g -Wall -std=gnu11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T../base/base.ld
LD	= gcc
CC	= gcc
AR	= ar

ifneq ($(DEBUG),)
CFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
CFLAGS += -DNDEBUG -O3
endif

# handy for debugging
print-%  : ; @echo $* = $($*)

# libshim.a - the shenango shim library
shim_src = $(wildcard *.c)
shim_obj = $(shim_src:.c=.o)

# must be first
all: libshim.a

libshim.a: $(shim_obj)
	$(AR) rcs $@ $^

# general build rules for all targets
src = $(shim_src)
obj = $(src:.c=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) libshim.a
