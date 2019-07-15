# include this Makefile in all subprojects
# define ROOT_PATH before including
ifndef ROOT_PATH
$(error ROOT_PATH is not set)
endif

# build configuration options (set to y for "yes", n for "no")
CONFIG_MLX5=n
CONFIG_MLX4=n
CONFIG_SPDK=n
CONFIG_DEBUG=n
CONFIG_NATIVE=n
CONFIG_DIRECTPATH=n

# shared toolchain definitions
INC = -I$(ROOT_PATH)/inc
FLAGS  = -g -Wall -D_GNU_SOURCE $(INC)
LDFLAGS = -T $(ROOT_PATH)/base/base.ld
LD      = gcc
CC      = gcc
LDXX	= g++
CXX	= g++
AR      = ar
SPARSE  = sparse

# libraries to include
RUNTIME_DEPS = $(ROOT_PATH)/libruntime.a $(ROOT_PATH)/libnet.a \
	       $(ROOT_PATH)/libbase.a
RUNTIME_LIBS = $(ROOT_PATH)/libruntime.a $(ROOT_PATH)/libnet.a \
	       $(ROOT_PATH)/libbase.a -lpthread

# parse configuration options
ifeq ($(CONFIG_DEBUG),y)
FLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
FLAGS += -DNDEBUG -O3
ifeq ($(CONFIG_NATIVE),y)
FLAGS += -march=native
else
FLAGS += -mssse3
endif
endif
ifeq ($(CONFIG_MLX5),y)
FLAGS += -DMLX5
else
ifeq ($(CONFIG_MLX4),y)
FLAGS += -DMLX4
endif
endif
ifeq ($(CONFIG_SPDK),y)
RUNTIME_LIBS += -L$(ROOT_PATH)/spdk/build/lib -L$(ROOT_PATH)/spdk/dpdk/build/lib
RUNTIME_LIBS += -lspdk_nvme -lspdk_util -lspdk_env_dpdk -lspdk_log -lspdk_sock \
		-ldpdk -lpthread -lrt -luuid -lcrypto -lnuma -ldl
INC += -I$(ROOT_PATH)/spdk/include
endif
ifeq ($(CONFIG_DIRECTPATH),y)
RUNTIME_LIBS += -L$(ROOT_PATH)/rdma-core/build/lib/statics/
RUNTIME_LIBS += -lmlx5 -libverbs -lnl-3 -lnl-route-3
INC += -I$(ROOT_PATH)/rdma-core/build/include
FLAGS += -DDIRECTPATH
endif

CFLAGS = -std=gnu11 $(FLAGS)
CXXFLAGS = -std=gnu++11 $(FLAGS)

# handy for debugging
print-%  : ; @echo $* = $($*) 
