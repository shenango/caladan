# Makefile for benchmarks
ROOT_PATH=../..
include $(ROOT_PATH)/build/shared.mk

fake_worker_src = fake_worker.cc
fake_worker_obj = $(fake_worker_src:.cc=.o)

tbench_src = tbench.cc
tbench_obj = $(tbench_src:.cc=.o)

callibrate_src = callibrate.cc
callibrate_obj = $(callibrate_src:.cc=.o)

stress_src = stress.cc
stress_obj = $(stress_src:.cc=.o)

stress_linux_src = stress_linux.cc
stress_linux_obj = $(stress_linux_src:.cc=.o)

efficiency_src = efficiency.cc
efficiency_obj = $(efficiency_src:.cc=.o)

efficiency_linux_src = efficiency_linux.cc
efficiency_linux_obj = $(efficiency_linux_src:.cc=.o)

rpclib_src = RpcManager.cc
rpclib_obj = $(rpclib_src:.cc=.o)

memcached_router_src = memcached_router.cc
memcached_router_obj = $(memcached_router_src:.cc=.o)

flash_client_src = flash_client.cc
flash_client_obj = $(flash_client_src:.cc=.o)

netbench_src = netbench.cc
netbench_obj = $(netbench_src:.cc=.o)

storage_bench_src = storage_bench.cc
storage_bench_obj = $(storage_bench_src:.cc=.o)

netbench2_src = netbench2.cc
netbench2_obj = $(netbench2_src:.cc=.o)

netbench_udp_src = netbench_udp.cc
netbench_udp_obj = $(netbench_udp_src:.cc=.o)

netbench_linux_src = netbench_linux.cc
netbench_linux_obj = $(netbench_linux_src:.cc=.o)

netperf_src = netperf.cc
netperf_obj = $(netperf_src:.cc=.o)

linux_mech_bench_src = linux_mech_bench.cc
linux_mech_bench_obj = $(linux_mech_bench_src:.cc=.o)

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

# must be first
all: tbench callibrate stress efficiency efficiency_linux \
     netbench netbench2 netbench_udp netbench_linux netperf linux_mech_bench \
     stress_linux memcached_router flash_client storage_bench

tbench: $(tbench_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(tbench_obj) $(librt_libs) $(RUNTIME_LIBS)

callibrate: $(fake_worker_obj) $(callibrate_obj)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(callibrate_obj) -lpthread

stress: $(fake_worker_obj) $(stress_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(stress_obj) $(librt_libs) \
	 $(RUNTIME_LIBS)

stress_linux: $(fake_worker_obj) $(stress_linux_obj)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(stress_linux_obj) \
	-lpthread

efficiency: $(fake_worker_obj) $(efficiency_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(efficiency_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

efficiency_linux: $(fake_worker_obj) $(efficiency_linux_obj)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(efficiency_linux_obj) \
	-lpthread

memcached_router: $(rpclib_obj) $(memcached_router_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX)  -o $@ $(LDFLAGS) $(rpclib_obj) $(memcached_router_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

flash_client: $(rpclib_obj) $(flash_client_obj) $(librt_libs) $(RUNTIME_DEPS) $(fake_worker_obj)
	$(LDXX)  -o $@ $(LDFLAGS) $(rpclib_obj) $(flash_client_obj) $(fake_worker_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench: $(netbench_obj) $(fake_worker_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(netbench_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

storage_bench: $(storage_bench_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(storage_bench_obj) $(librt_libs) \
	$(RUNTIME_LIBS)

netbench2: $(netbench2_obj) $(fake_worker_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(netbench2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench_udp: $(netbench_udp_obj) $(fake_worker_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(netbench_udp_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench_linux: $(netbench_linux_obj) $(fake_worker_obj) $(ROOT_PATH)/libbase.a
	$(LDXX) -o $@ $(LDFLAGS) $(fake_worker_obj) $(netbench_linux_obj)  $(ROOT_PATH)/libbase.a  -lpthread

netperf: $(netperf_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(netperf_obj) $(librt_libs) $(RUNTIME_LIBS)

linux_mech_bench: $(linux_mech_bench_obj) $(librt_libs)
	$(LDXX) -o $@ $(LDFLAGS) $(linux_mech_bench_obj) $(librt_libs) \
	$(RUNTIME_LIBS) -lpthread

# general build rules for all targets
src = $(fake_worker_src) $(tbench_src) $(callibrate_src) $(memcached_router_src) $(rpclib_src)
src += $(stress_src) $(efficiency_src) $(efficiency_linux_src) $(netbench_src) $(flash_client_src)
src += $(netbench2_src) $(netbench_udp_src) $(netbench_linux_src) $(netperf_src)
src += $(linux_mech_bench_src) $(storage_bench_src)
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
	rm -f $(obj) $(dep) tbench callibrate stress efficiency \
	efficiency_linux netbench netbench2 netbench_udp netbench_linux \
	netperf linux_mech_bench stress_linux memcached_router flash_client \
	storage_bench
