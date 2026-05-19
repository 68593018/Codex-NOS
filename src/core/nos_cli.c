#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "nos_cli.h"
#include "nos_node_priv.h"
#include "nos_node_mgr.h"
#include "nos_manifest.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"

/* --- CLI 历史记录管理 --- */
#define MAX_HISTORY 3
static char g_history[MAX_HISTORY][256];
static int  g_history_count = 0;

static void history_add(const char *cmd) {
    if (!cmd || strlen(cmd) == 0) return;
    if (g_history_count > 0 && strcmp(g_history[0], cmd) == 0) return;
    for (int i = MAX_HISTORY - 1; i > 0; i--) strcpy(g_history[i], g_history[i-1]);
    strncpy(g_history[0], cmd, 255);
    if (g_history_count < MAX_HISTORY) g_history_count++;
}

/* --- 命令处理函数声明 --- */
static void do_help(const char *args);
static void do_quit(const char *args);
static void do_show_components(const char *args);
static void do_show_services(const char *args);
static void do_show_db(const char *args);
static void do_show_memory(const char *args);
static void do_log(const char *args);
static void do_perf_start(const char *args);
static void do_perf_remote_start(const char *args);
static void do_load(const char *args);
static void do_unload(const char *args);
static void do_reload(const char *args);

typedef struct {
    const char *name;
    const char *help;
    void (*handler)(const char *args);
} nos_cli_cmd_t;

static nos_cli_cmd_t g_cli_cmds[] = {
    {"help",            "Show this help message",           do_help},
    {"show components", "List all loaded components",       do_show_components},
    {"show services",   "List all service routes",         do_show_services},
    {"show db",         "List all KV tables status",        do_show_db},
    {"show memory",     "Show process memory consumption",  do_show_memory},
    {"log",             "Manage logging (stats, level)",    do_log},
    {"perf start",      "Start IPC performance test",       do_perf_start},
    {"perf remote",     "Start Cross-Process IPC test",     do_perf_remote_start},
    {"load",            "Load a component by name",         do_load},
    {"unload",          "Unload a component by name",       do_unload},
    {"reload",          "Reload a component (Stateless check)", do_reload},
    {"quit",            "Shutdown the node",                do_quit},
    {NULL, NULL, NULL}
};

static const char* comp_status_str(nos_comp_status_t st) {
    switch(st) {
        case NOS_COMP_ST_LOADED:  return "Loaded";
        case NOS_COMP_ST_INITED:  return "Inited";
        case NOS_COMP_ST_ACTIVE:  return "Active";
        case NOS_COMP_ST_STOPPED: return "Stopped";
        case NOS_COMP_ST_ERROR:   return "Error";
        default: return "Unknown";
    }
}

static void do_help(const char *args) {
    printf("\n--- NOS Node CLI Commands ---\n");
    for (int i = 0; g_cli_cmds[i].name; i++) printf("  %-18s - %s\n", g_cli_cmds[i].name, g_cli_cmds[i].help);
    printf("-----------------------------\n");
}

static void do_quit(const char *args) { g_node_ctx.keep_running = 0; }

static void do_show_components(const char *args) {
    printf("\n%-15s %-4s %-10s %-15s %-12s\n", "Name", "ID", "Status", "Model-Lib", "Thread");
    printf("----------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        loaded_comp_info_t *info = &g_node_ctx.loaded_info[i];
        printf("%-15s %-4u %-10s %-15s %-12s\n", info->comp->name, info->comp->id, comp_status_str(info->comp->status), info->lib_name, info->owner_thread->name);
    }
}

static void do_show_services(const char *args) {
    const nos_node_def_t *node = g_node_ctx.node_def;
    printf("\n%-18s %-8s %-10s %-8s %-10s\n", "Name", "Svc-ID", "Node", "Comp-ID", "Location");
    printf("----------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < node->service_count; i++) {
        const nos_service_def_t *svc = &node->services[i];
        const char *loc = (strcmp(svc->node_name, "Platform") == 0) ? "Embedded" : (strcmp(svc->node_name, node->name) == 0) ? "Local" : "Remote";
        printf("%-18s %-8u %-10s %-8u %-10s\n", svc->service_name, svc->service_id, svc->node_name, svc->provider_comp_id, loc);
    }
}

static void do_show_db(const char *args) { extern void nos_kv_dump_all(void); nos_kv_dump_all(); }

static void do_show_memory(const char *args) {
    extern size_t nos_log_get_mem_usage(void);
    extern size_t nos_kv_get_total_mem_usage(void);
    extern size_t nos_buffer_get_total_mem_usage(void);
    extern size_t nos_scheduler_get_total_mem_usage(void);

    size_t log_mem = nos_log_get_mem_usage();
    size_t kv_mem  = nos_kv_get_total_mem_usage();
    size_t buf_mem = nos_buffer_get_total_mem_usage();
    size_t sch_mem = nos_scheduler_get_total_mem_usage();

    printf("\n--- NOS Process Memory Consumption ---\n");
    printf("%-20s %-15s %-15s\n", "Module/Component", "Usage (Bytes)", "Type");
    printf("------------------------------------------------------------\n");

    /* 1. 嵌入式服务与基础设施 */
    printf("%-20s %-15zu %-15s\n", "SVC_LOG", log_mem, "Embedded Service");
    printf("%-20s %-15zu %-15s\n", "SVC_KV_DB", kv_mem, "Embedded Service");
    printf("%-20s %-15zu %-15s\n", "Buffer Pool", buf_mem, "Infrastructure");
    printf("%-20s %-15zu %-15s\n", "Scheduler/Threads", sch_mem, "Infrastructure");

    /* 2. 组件维度 */
    size_t total_comp_mem = 0;
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        loaded_comp_info_t *info = &g_node_ctx.loaded_info[i];
        size_t comp_obj_mem = sizeof(nos_component_t);
        size_t priv_mem = 0;
        if (strstr(info->lib_name, "libcomp-1.so")) priv_mem = 16;
        else if (strstr(info->lib_name, "libcomp-2.so")) priv_mem = 32;
        printf("%-20s %-15zu %-15s\n", info->comp->name, comp_obj_mem + priv_mem, "Component");
        total_comp_mem += (comp_obj_mem + priv_mem);
    }
    printf("------------------------------------------------------------\n");
    printf("%-20s %-15zu\n\n", "Total Accounted", log_mem + kv_mem + buf_mem + sch_mem + total_comp_mem);
}

static nos_log_level_t str_to_level(const char *str) {
    if (strcasecmp(str, "debug") == 0) return NOS_LOG_LEVEL_DEBUG;
    if (strcasecmp(str, "info") == 0)  return NOS_LOG_LEVEL_INFO;
    if (strcasecmp(str, "warn") == 0)  return NOS_LOG_LEVEL_WARN;
    if (strcasecmp(str, "error") == 0) return NOS_LOG_LEVEL_ERROR;
    return NOS_LOG_LEVEL_INFO;
}

static void do_log(const char *args) {
    if (!args) {
        printf("Usage: log stats | log level <DEBUG|INFO|WARN|ERROR> | log level <comp_id> <level>\n");
        return;
    }

    if (strncmp(args, "stats", 5) == 0) {
        unsigned long dropped = 0;
        extern void nos_log_get_stats(unsigned long *total_dropped);
        nos_log_get_stats(&dropped);
        printf("\n--- NOS Logging Statistics ---\n");
        printf("Dropped Messages: %lu\n", dropped);
        if (dropped > 0) printf("Warning: High log volume or slow consumer. Consider increasing LOG_BUFFER_SIZE.\n");
        printf("------------------------------\n");
    } else if (strncmp(args, "spam", 4) == 0) {
        uint32_t count = (args[4] != '\0') ? (uint32_t)atoi(args + 5) : 2000;
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + sizeof(uint32_t), 0);
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC;
            msg->dst_service = 110; // SVC_PERF_RX (Comp-3)
            msg->msg_code = 3002; // SPAM_LOG
            msg->payload_len = sizeof(uint32_t);
            memcpy(msg + 1, &count, sizeof(uint32_t));
            printf("[CLI] Triggering spam logging test with %u messages...\n", count);
            nos_service_msg_send(buf);
            nos_buffer_release(buf);
        }
    } else if (strncmp(args, "level", 5) == 0) {
        char sub[64], val[64];
        int count = sscanf(args + 6, "%s %s", sub, val);
        nos_log_ops_t *log_ops = (nos_log_ops_t *)nos_embedded_service_get("SVC_LOG");
        if (!log_ops) { printf("Log service not found.\n"); return; }

        if (count == 1) {
            log_ops->set_filter_level(str_to_level(sub));
        } else if (count == 2) {
            uint32_t comp_id = (uint32_t)atoi(sub);
            log_ops->set_comp_level(comp_id, str_to_level(val));
        } else {
            printf("Usage: log level <level> | log level <comp_id> <level>\n");
        }
    }
}

static void do_perf_start(const char *args) {
    uint32_t count = args ? (uint32_t)atoi(args) : 10000;
    if (count == 0) count = 10000;
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + sizeof(uint32_t), 0);
    if (buf) {
        nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
        msg->magic = NOS_IPC_MAGIC;
        msg->dst_service = 111; // SVC_PING (Comp-Ping)
        msg->msg_code = 3001; // START_TEST
        msg->payload_len = sizeof(uint32_t);
        memcpy(msg + 1, &count, sizeof(uint32_t));
        printf("[CLI] Triggering specialized performance test with %u iterations...\n", count);
        nos_service_msg_send(buf);
        nos_buffer_release(buf);
    }
}
static void do_perf_remote_start(const char *args) {
    uint32_t count = args ? (uint32_t)atoi(args) : 10000;
    if (count == 0) count = 10000;
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + sizeof(uint32_t), 0);
    if (buf) {
        nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
        msg->magic = NOS_IPC_MAGIC;
        msg->dst_service = 113; // SVC_REMOTE_PING (Comp-RemotePing)
        msg->msg_code = 3001; // START_TEST
        msg->payload_len = sizeof(uint32_t);
        memcpy(msg + 1, &count, sizeof(uint32_t));
        printf("[CLI] Triggering cross-process performance test with %u iterations...\n", count);
        nos_service_msg_send(buf);
        nos_buffer_release(buf);
    }
}
static void do_load(const char *args) { if (args) node_reload_component(args); }
static void do_reload(const char *args) { if (args) node_reload_component(args); }
static void do_unload(const char *args) { if (args) node_unload_component(args); }

static void handle_tab_completion(char *buf, int *pos, const char *prompt) {
    buf[*pos] = '\0';
    const char *suggestions[64];
    int sug_count = 0;
    const char *match_start = buf;
    int match_len = *pos;

    /* 1. 尝试匹配顶层命令 (来自 g_cli_cmds) */
    for (int i = 0; g_cli_cmds[i].name; i++) {
        if (strncmp(g_cli_cmds[i].name, buf, *pos) == 0) {
            suggestions[sug_count++] = g_cli_cmds[i].name;
        }
    }

    /* 2. 如果没有顶层匹配，或者已经输入了空格，尝试匹配子命令/参数 */
    char *last_space = strrchr(buf, ' ');
    if (sug_count == 0 && last_space) {
        match_start = last_space + 1;
        match_len = (int)(buf + *pos - match_start);

        if (strncmp(buf, "log ", 4) == 0) {
            if (strncmp(buf, "log level ", 10) == 0) {
                const char *levels[] = {"DEBUG", "INFO", "WARN", "ERROR", NULL};
                for (int i = 0; levels[i]; i++) if (strncmp(levels[i], match_start, match_len) == 0) suggestions[sug_count++] = levels[i];
            } else if (last_space == buf + 3) {
                const char *log_args[] = {"stats", "level", "spam", NULL};
                for (int i = 0; log_args[i]; i++) if (strncmp(log_args[i], match_start, match_len) == 0) suggestions[sug_count++] = log_args[i];
            }
        } else if (strncmp(buf, "perf ", 5) == 0) {
            const char *perf_args[] = {"start", "remote", NULL};
            for (int i = 0; perf_args[i]; i++) if (strncmp(perf_args[i], match_start, match_len) == 0) suggestions[sug_count++] = perf_args[i];
        } else if (strncmp(buf, "show ", 5) == 0) {
            const char *show_args[] = {"components", "services", "db", "memory", NULL};
            for (int i = 0; show_args[i]; i++) if (strncmp(show_args[i], match_start, match_len) == 0) suggestions[sug_count++] = show_args[i];
        } else if (strncmp(buf, "load ", 5) == 0 || strncmp(buf, "unload ", 7) == 0 || strncmp(buf, "reload ", 7) == 0) {
            for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
                if (strncmp(g_node_ctx.loaded_info[i].comp->name, match_start, match_len) == 0) {
                    suggestions[sug_count++] = g_node_ctx.loaded_info[i].comp->name;
                }
            }
        }
    }

    if (sug_count == 1) {
        printf("%s", suggestions[0] + match_len);
        strcpy(buf + *pos, suggestions[0] + match_len);
        *pos = (int)strlen(buf);
    } else if (sug_count > 1) {
        printf("\n");
        for (int i = 0; i < sug_count; i++) printf("%s  ", suggestions[i]);
        printf("\n%s%s", prompt, buf);
    }
}

static int advanced_get_line(char *buf, int size, const char *prompt) {
    struct termios oldt, newt;
    int pos = 0; char c; int h_idx = -1;
    printf("%s", prompt); fflush(stdout);
    tcgetattr(STDIN_FILENO, &oldt); newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    while (pos < size - 1) {
        if (read(STDIN_FILENO, &c, 1) <= 0) { pos = -1; break; }
        if (c == '\n' || c == '\r') { buf[pos] = '\0'; printf("\n"); break; }
        else if (c == 27) {
            char seq[3]; if (read(STDIN_FILENO, &seq[0], 1) <= 0) break; if (read(STDIN_FILENO, &seq[1], 1) <= 0) break;
            if (seq[0] == '[' && seq[1] == 'A') {
                if (g_history_count > 0 && h_idx < g_history_count - 1) {
                    h_idx++; printf("\r%s\033[K%s", prompt, g_history[h_idx]);
                    strcpy(buf, g_history[h_idx]); pos = (int)strlen(buf);
                }
            }
        } else if (c == 127 || c == 8) {
            if (pos > 0) { pos--; printf("\b \b"); }
        } else if (c == '\t') {
            handle_tab_completion(buf, &pos, prompt);
        } else { buf[pos++] = c; printf("%c", c); }
        fflush(stdout);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); return pos;
}

static void* node_cli_thread_entry(void *arg) {
    char buf[256], prompt[64]; int is_tty = isatty(STDIN_FILENO);
    sprintf(prompt, "nos-node(%s)> ", (char*)arg);
    if (is_tty) printf("[CLI] Management interface started. (Tab for completion, Up for history)\n");
    while (g_node_ctx.keep_running) {
        int len = is_tty ? advanced_get_line(buf, sizeof(buf), prompt) : (fgets(buf, sizeof(buf), stdin) ? (int)strlen(buf) : -1);
        if (len < 0) break; 
        if (len == 0) continue;
        if (!is_tty) buf[strcspn(buf, "\n")] = 0;
        history_add(buf);
        int found = 0;
        for (int i = 0; g_cli_cmds[i].name; i++) {
            int nlen = (int)strlen(g_cli_cmds[i].name);
            if (strcmp(buf, g_cli_cmds[i].name) == 0 || (strncmp(buf, g_cli_cmds[i].name, nlen) == 0 && buf[nlen] == ' ')) {
                g_cli_cmds[i].handler((buf[nlen] == ' ') ? (buf + nlen + 1) : NULL);
                found = 1; break;
            }
        }
        if (!found && is_tty) printf("Unknown command: %s\n", buf);
    }
    return NULL;
}

void node_cli_start(const char *node_name) {
    pthread_create(&g_node_ctx.cli_tid, NULL, node_cli_thread_entry, (void*)node_name);
}
