# include this Makefile in all subprojects
# define ROOT_PATH before including
ifndef ROOT_PATH
$(error ROOT_PATH is not set)
endif

# load configuration parameters
include $(ROOT_PATH)/build/config

# shared toolchain definitions
INC = -I$(ROOT_PATH)/inc
FLAGS  = -g -Wall -D_GNU_SOURCE $(INC) -m64 -mxsavec -m64 -mxsave -m64 -muintr
LDFLAGS = -T $(ROOT_PATH)/base/base.ld
LD      = gcc
CC      = gcc
LDXX	= g++
CXX	= g++
AR      = ar
SPARSE  = sparse

ifeq ($(CONFIG_CLANG),y)
LD	= clang
CC	= clang
LDXX	= clang++
CXX	= clang++
FLAGS += -Wno-sync-fetch-and-nand-semantics-changed
endif

# libraries to include
RUNTIME_DEPS = $(ROOT_PATH)/libruntime.a $(ROOT_PATH)/libnet.a \
	       $(ROOT_PATH)/libbase.a
RUNTIME_LIBS = $(ROOT_PATH)/libruntime.a $(ROOT_PATH)/libnet.a \
	       $(ROOT_PATH)/libbase.a -lpthread

# PKG_CONFIG_PATH
PKG_CONFIG_PATH := $(ROOT_PATH)/rdma-core/build/lib/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG_PATH := $(ROOT_PATH)/dpdk/build/lib/x86_64-linux-gnu/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG_PATH := $(ROOT_PATH)/spdk/build/lib/pkgconfig:$(PKG_CONFIG_PATH)

# mlx5 build
MLX5_INC = -I$(ROOT_PATH)/rdma-core/build/include
MLX5_LIBS = -L$(ROOT_PATH)/rdma-core/build/lib/
MLX5_LIBS += -L$(ROOT_PATH)/rdma-core/build/lib/statics/
MLX5_LIBS += -L$(ROOT_PATH)/rdma-core/build/util/
MLX5_LIBS += -L$(ROOT_PATH)/rdma-core/build/ccan/
MLX5_LIBS += -l:libmlx5.a -l:libibverbs.a -lnl-3 -lnl-route-3 -lrdmacm -lrdma_util -lccan

# parse configuration options
ifeq ($(CONFIG_DEBUG),y)
FLAGS += -DDEBUG -rdynamic -O0 -ggdb -mssse3
LDFLAGS += -rdynamic
else
FLAGS += -DNDEBUG -O3
ifeq ($(CONFIG_OPTIMIZE),y)
FLAGS += -march=native -flto=auto -ffast-math
ifeq ($(CONFIG_CLANG),y)
LDFLAGS += -flto
endif
else
FLAGS += -mssse3
endif
endif
ifeq ($(CONFIG_MLX5),y)
FLAGS += -DMLX5
else
ifeq ($(CONFIG_MLX4),y)
$(error mlx4 support is not available currently)
FLAGS += -DMLX4
endif
endif
ifeq ($(CONFIG_SPDK),y)
FLAGS += -DDIRECT_STORAGE
RUNTIME_LIBS += $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static libdpdk)
RUNTIME_LIBS += $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_nvme)
RUNTIME_LIBS += $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_env_dpdk)
RUNTIME_LIBS += $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)
INC += -I$(ROOT_PATH)/spdk/include
endif
ifeq ($(CONFIG_DIRECTPATH),y)
RUNTIME_LIBS += $(MLX5_LIBS)
INC += $(MLX5_INC)
FLAGS += -DDIRECTPATH
endif

CFLAGS = -std=gnu11 $(FLAGS)
CXXFLAGS = -std=gnu++20 $(FLAGS)

# handy for debugging
print-%  : ; @echo $* = $($*)
