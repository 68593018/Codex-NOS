#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "nos_mgr.h"

typedef enum {
    MGR_NODE_STOPPED = 0,
    MGR_NODE_RUNNING,
    MGR_NODE_EXITED
} mgr_node_state_t;

typedef struct {
    const nos_mgr_node_def_t *def;
    pid_t pid;
    mgr_node_state_t state;
    time_t started_at;
    int exit_status;
} mgr_node_t;

#define MGR_MAX_NODES 32

static mgr_node_t g_nodes[MGR_MAX_NODES];
static size_t g_node_count = 0;
static int g_keep_running = 1;

static const char* state_name(mgr_node_state_t state) {
    switch (state) {
        case MGR_NODE_STOPPED: return "STOPPED";
        case MGR_NODE_RUNNING: return "RUNNING";
        case MGR_NODE_EXITED: return "EXITED";
        default: return "UNKNOWN";
    }
}

static void signal_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static void mgr_init_nodes(void) {
    const nos_mgr_node_def_t *defs = nos_mgr_manifest_get_nodes();
    while (defs && defs->name && g_node_count < MGR_MAX_NODES) {
        g_nodes[g_node_count].def = defs;
        g_nodes[g_node_count].pid = -1;
        g_nodes[g_node_count].state = MGR_NODE_STOPPED;
        g_nodes[g_node_count].started_at = 0;
        g_nodes[g_node_count].exit_status = 0;
        g_node_count++;
        defs++;
    }
}

static mgr_node_t* find_node(const char *name) {
    if (!name || name[0] == '\0') return NULL;
    for (size_t i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].def->name, name) == 0) return &g_nodes[i];
    }
    return NULL;
}

static void refresh_node(mgr_node_t *node) {
    if (!node || node->state != MGR_NODE_RUNNING || node->pid <= 0) return;

    int status = 0;
    pid_t ret = waitpid(node->pid, &status, WNOHANG);
    if (ret == node->pid) {
        node->exit_status = status;
        node->pid = -1;
        node->state = MGR_NODE_EXITED;
    }
}

static void refresh_nodes(void) {
    for (size_t i = 0; i < g_node_count; i++) refresh_node(&g_nodes[i]);
}

static void format_uptime(const mgr_node_t *node, char *buf, size_t size) {
    if (!node || node->state != MGR_NODE_RUNNING || node->started_at == 0) {
        snprintf(buf, size, "-");
        return;
    }

    long seconds = (long)(time(NULL) - node->started_at);
    long hours = seconds / 3600;
    long minutes = (seconds % 3600) / 60;
    long secs = seconds % 60;
    snprintf(buf, size, "%02ld:%02ld:%02ld", hours, minutes, secs);
}

static void show_nodes(void) {
    refresh_nodes();
    printf("\n%-10s %-8s %-10s %-10s %-16s %s\n", "Name", "PID", "State", "Uptime", "UDS", "Binary");
    printf("--------------------------------------------------------------------------------\n");
    for (size_t i = 0; i < g_node_count; i++) {
        char pid_buf[32];
        char uptime[32];
        mgr_node_t *node = &g_nodes[i];
        format_uptime(node, uptime, sizeof(uptime));
        if (node->pid > 0) snprintf(pid_buf, sizeof(pid_buf), "%ld", (long)node->pid);
        else snprintf(pid_buf, sizeof(pid_buf), "-");
        printf("%-10s %-8s %-10s %-10s %-16s %s\n",
               node->def->name,
               pid_buf,
               state_name(node->state),
               uptime,
               node->def->uds_path,
               node->def->binary);
    }
}

static int redirect_child_stdio(const char *node_name) {
    if (mkdir("logs", 0755) < 0 && errno != EEXIST) return -1;

    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd < 0) return -1;

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "logs/%s.log", node_name);
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        close(null_fd);
        return -1;
    }

    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(log_fd, STDOUT_FILENO) < 0 ||
        dup2(log_fd, STDERR_FILENO) < 0) {
        close(null_fd);
        close(log_fd);
        return -1;
    }

    close(null_fd);
    close(log_fd);
    return 0;
}

static void start_node(const char *name) {
    mgr_node_t *node = find_node(name);
    if (!node) {
        printf("Unknown node: %s\n", name ? name : "");
        return;
    }

    refresh_node(node);
    if (node->state == MGR_NODE_RUNNING) {
        printf("Node %s is already running (PID %ld).\n", node->def->name, (long)node->pid);
        return;
    }
    if (access(node->def->binary, X_OK) != 0) {
        printf("Node binary is not executable: %s\n", node->def->binary);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("Failed to start %s: %s\n", node->def->name, strerror(errno));
        return;
    }
    if (pid == 0) {
        if (redirect_child_stdio(node->def->name) != 0) _exit(126);
        setenv("NOS_BUSY_POLL_CYCLES", "0", 0);
        execl(node->def->binary, node->def->binary, (char *)NULL);
        _exit(127);
    }

    node->pid = pid;
    node->state = MGR_NODE_RUNNING;
    node->started_at = time(NULL);
    node->exit_status = 0;
    printf("Node %s started (PID %ld).\n", node->def->name, (long)pid);
}

static void stop_node(const char *name) {
    mgr_node_t *node = find_node(name);
    if (!node) {
        printf("Unknown node: %s\n", name ? name : "");
        return;
    }

    refresh_node(node);
    if (node->state != MGR_NODE_RUNNING || node->pid <= 0) {
        printf("Node %s is not running.\n", node->def->name);
        return;
    }

    pid_t pid = node->pid;
    if (kill(pid, SIGTERM) != 0) {
        printf("Failed to stop %s: %s\n", node->def->name, strerror(errno));
        return;
    }

    for (int i = 0; i < 30; i++) {
        int status = 0;
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            node->pid = -1;
            node->state = MGR_NODE_STOPPED;
            node->exit_status = status;
            printf("Node %s stopped.\n", node->def->name);
            return;
        }
        usleep(100000);
    }

    if (kill(pid, SIGKILL) == 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        node->pid = -1;
        node->state = MGR_NODE_STOPPED;
        node->exit_status = status;
        printf("Node %s killed after stop timeout.\n", node->def->name);
    }
}

static void restart_node(const char *name) {
    mgr_node_t *node = find_node(name);
    if (!node) {
        printf("Unknown node: %s\n", name ? name : "");
        return;
    }
    if (node->state == MGR_NODE_RUNNING) stop_node(name);
    start_node(name);
}

static void print_help(void) {
    printf("\n--- NOS Manager Commands ---\n");
    printf("  help             - Show this help message\n");
    printf("  show nodes       - List manageable nodes\n");
    printf("  start <node>     - Start a node process\n");
    printf("  stop <node>      - Stop a node process started by this manager\n");
    printf("  restart <node>   - Restart a node process\n");
    printf("  quit             - Exit manager without stopping running nodes\n");
    printf("----------------------------\n");
}

static char* trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return s;
}

static void handle_command(char *line) {
    char *cmd = trim(line);
    refresh_nodes();

    if (cmd[0] == '\0') return;
    if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "show nodes") == 0) {
        show_nodes();
    } else if (strncmp(cmd, "start ", 6) == 0) {
        start_node(trim(cmd + 6));
    } else if (strncmp(cmd, "stop ", 5) == 0) {
        stop_node(trim(cmd + 5));
    } else if (strncmp(cmd, "restart ", 8) == 0) {
        restart_node(trim(cmd + 8));
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        g_keep_running = 0;
    } else {
        printf("Unknown command: %s\n", cmd);
    }
}

int main(void) {
    char line[256];

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    mgr_init_nodes();

    printf("[NOS Manager] Management interface started. Type 'help' for commands.\n");
    while (g_keep_running) {
        printf("nos-mgr> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        handle_command(line);
    }

    refresh_nodes();
    printf("NOS Manager exiting.\n");
    return 0;
}
