CC = gcc
CFLAGS = -Iinclude -Wall -fPIC -ffunction-sections -fdata-sections
LDFLAGS = -lpthread -ldl -rdynamic -Wl,--gc-sections
PYTHON = python3
GEN_SCRIPT = scripts/gen_manifest.py
CONFIG_DIR = conf

# 核心通用源码
CORE_SRCS = src/core/nos_scheduler.c \
            src/core/nos_buffer.c \
            src/core/nos_service.c \
            src/core/nos_node_mgr.c \
            src/core/nos_cli.c \
            src/infra/nos_ipc_p2p.c \
            src/infra/log/nos_log.c \
            src/infra/db/nos_kv.c \
            src/infra/timer/nos_timer.c \
            src/core/nos_node_main.c

# 将核心源码转换为 .o
CORE_OBJS = $(patsubst %.c, %.o, $(CORE_SRCS))

# 组件定义
COMP_LIBS = libcomp-1.so libcomp-2.so libcomp-3.so libcomp-4.so libcomp-5.so \
            libcomp-ping.so libcomp-pong.so \
            libcomp-rping.so libcomp-rpong.so

all: include/nos_ids.h $(COMP_LIBS) nos_ProcA nos_ProcB

include/nos_ids.h: $(shell find $(CONFIG_DIR) -name "*.yaml") $(GEN_SCRIPT)
	@echo "Generating manifests and ID headers..."
	@$(PYTHON) $(GEN_SCRIPT) $(CONFIG_DIR) include/nos_ids.h

# 通用编译规则：.c -> .o
%.o: %.c include/nos_ids.h
	@echo "Compiling $<..."
	@$(CC) $(CFLAGS) -c $< -o $@

# 编译各节点二进制
nos_ProcA: $(CORE_OBJS) src/core/manifest_ProcA.o
	@echo "Linking binary $@..."
	@$(CC) $^ -o $@ $(LDFLAGS)

nos_ProcB: $(CORE_OBJS) src/core/manifest_ProcB.o
	@echo "Linking binary $@..."
	@$(CC) $^ -o $@ $(LDFLAGS)

# 组件编译规则：适配 src/components/model_N/model_N.c 结构
libcomp-1.so: src/components/model_1/model_1.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-2.so: src/components/model_2/model_2.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-3.so: src/components/model_3/model_3.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-4.so: src/components/model_4/model_4.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-5.so: src/components/model_5/model_5.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-ping.so: src/components/perf/ping.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-pong.so: src/components/perf/pong.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-rping.so: src/components/perf/remote_ping.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-rpong.so: src/components/perf/remote_pong.c include/nos_ids.h
	@$(CC) $(CFLAGS) -shared $< -o $@

clean:
	rm -f nos_Proc* libcomp-*.so include/nos_ids.h src/core/*.o src/infra/*.o
