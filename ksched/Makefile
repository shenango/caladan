KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(PWD)

default:
	make -C $(KDIR) M=$(BUILD_DIR) src=$(PWD) modules

clean:
	make -C $(KDIR) M=$(BUILD_DIR) src=$(PWD) clean
