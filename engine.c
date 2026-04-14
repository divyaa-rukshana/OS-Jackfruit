#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define MAX_CONTAINERS 100

typedef enum {
    CMD_RUN = 1,
    CMD_START,
    CMD_PS,
    CMD_STOP,
    CMD_LOGS
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

typedef struct {
    int status;
    char message[2048]; 
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    char state[32];
    unsigned long soft;
    unsigned long hard;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
} child_config_t;

/* ================= CHILD ================= */

int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;

    char log_path[64];
    snprintf(log_path, sizeof(log_path), "/tmp/%s.log", config->id);
    int log_fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }

    if (sethostname(config->id, strlen(config->id)) < 0) perror("sethostname");

    if (mount(config->rootfs, config->rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) return 1;
    if (chroot(config->rootfs) < 0) return 1;
    if (chdir("/") < 0) return 1;
    
    // Mount proc so 'ps' and other tools work inside
    mount("proc", "/proc", "proc", 0, NULL);

    char *argv[] = {"/bin/sh", "-c", config->command, NULL};
    execv("/bin/sh", argv);
    return 1;
}

/* ================= SIGNAL ================= */

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid == pid) {
                snprintf(containers[i].state, sizeof(containers[i].state), "stopped");
            }
        }
    }
}

/* ================= SUPERVISOR ================= */

static int run_supervisor(const char *base_rootfs) {
    (void)base_rootfs;
    int monitor_fd = open("/dev/container_monitor", O_RDWR);
    
    unlink(CONTROL_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    listen(server_fd, 5);
    signal(SIGCHLD, sigchld_handler);

    printf("Supervisor active on %s\n", CONTROL_PATH);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        control_request_t req;
        if (recv(client_fd, &req, sizeof(req), 0) <= 0) { close(client_fd); continue; }

        control_response_t res = {0};

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            int exists = -1;
            for(int i=0; i<container_count; i++) {
                if(strcmp(containers[i].id, req.container_id) == 0 && strcmp(containers[i].state, "running") == 0)
                    exists = i;
            }

            if (exists != -1) {
                snprintf(res.message, sizeof(res.message), "Error: Container %s is already running", req.container_id);
            } else {
                child_config_t *cfg = calloc(1, sizeof(child_config_t));
                snprintf(cfg->id, sizeof(cfg->id), "%s", req.container_id);
                snprintf(cfg->rootfs, sizeof(cfg->rootfs), "%s", req.rootfs);
                snprintf(cfg->command, sizeof(cfg->command), "%s", req.command);

                char *stack = malloc(STACK_SIZE);
                pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, cfg);

                if (pid > 0) {
                    int idx = container_count++;
                    containers[idx].pid = pid;
                    snprintf(containers[idx].id, sizeof(containers[idx].id), "%s", req.container_id);
                    snprintf(containers[idx].state, sizeof(containers[idx].state), "running");
                    containers[idx].soft = req.soft_limit_bytes;
                    containers[idx].hard = req.hard_limit_bytes;
                    snprintf(res.message, sizeof(res.message), "Started %s (PID %d)", req.container_id, pid);

                    if (monitor_fd >= 0) {
                        struct monitor_request m_req = {0};
                        m_req.pid = pid;
                        m_req.soft_limit_bytes = req.soft_limit_bytes;
                        m_req.hard_limit_bytes = req.hard_limit_bytes;
                        snprintf(m_req.container_id, sizeof(m_req.container_id), "%s", req.container_id);
                        ioctl(monitor_fd, MONITOR_REGISTER, &m_req);
                    }
                } else {
                    snprintf(res.message, sizeof(res.message), "Clone failed");
                    free(stack);
                    free(cfg);
                }
            }
        } else if (req.kind == CMD_PS) {
            int offset = snprintf(res.message, sizeof(res.message), "--- Container List ---\n");
            for (int i = 0; i < container_count; i++) {
                offset += snprintf(res.message + offset, sizeof(res.message) - offset, 
                         "ID: %s | PID: %d | STATE: %s | SOFT: %lu MB\n",
                         containers[i].id, containers[i].pid, containers[i].state, containers[i].soft / (1024*1024));
            }
        } else if (req.kind == CMD_STOP) {
            int found = 0;
            for (int i = 0; i < container_count; i++) {
                if (strcmp(containers[i].id, req.container_id) == 0 && strcmp(containers[i].state, "running") == 0) {
                    kill(containers[i].pid, SIGKILL);
                    snprintf(res.message, sizeof(res.message), "Stopped %s", req.container_id);
                    found = 1;
                    break;
                }
            }
            if (!found) snprintf(res.message, sizeof(res.message), "Container %s not found or not running", req.container_id);
        } else if (req.kind == CMD_LOGS) {
            char log_path[64];
            snprintf(log_path, sizeof(log_path), "/tmp/%s.log", req.container_id);
            int fd = open(log_path, O_RDONLY);
            if (fd >= 0) {
                ssize_t n = read(fd, res.message, sizeof(res.message) - 1);
                if (n >= 0) res.message[n] = '\0';
                close(fd);
            } else {
                snprintf(res.message, sizeof(res.message), "No logs found for %s", req.container_id);
            }
        }

        send(client_fd, &res, sizeof(res), 0);
        close(client_fd);
    }
    return 0;
}

/* ================= CLIENT ================= */

static int send_request(control_request_t *req) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    send(fd, req, sizeof(*req), 0);
    control_response_t res;
    if (recv(fd, &res, sizeof(res), 0) > 0) printf("%s\n", res.message);
    close(fd);
    return 0;
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0) {
        char abs_path[PATH_MAX];
        if (argc < 3 || !realpath(argv[2], abs_path)) {
            fprintf(stderr, "Usage: %s supervisor <base_rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(abs_path);
    }

    control_request_t req = {0};
    if (strcmp(argv[1], "start") == 0 && argc >= 5) {
        req.kind = CMD_START;
        snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
        if (!realpath(argv[3], req.rootfs)) {
            perror("realpath rootfs");
            return 1;
        }
        snprintf(req.command, sizeof(req.command), "%s", argv[4]);
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--soft-mib") == 0 && i+1 < argc) req.soft_limit_bytes = atol(argv[++i]) * 1024 * 1024;
            if (strcmp(argv[i], "--hard-mib") == 0 && i+1 < argc) req.hard_limit_bytes = atol(argv[++i]) * 1024 * 1024;
        }
    } else if (strcmp(argv[1], "ps") == 0) {
        req.kind = CMD_PS;
    } else if (strcmp(argv[1], "stop") == 0 && argc >= 3) {
        req.kind = CMD_STOP;
        snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    } else if (strcmp(argv[1], "logs") == 0 && argc >= 3) {
        req.kind = CMD_LOGS;
        snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    } else {
        fprintf(stderr, "Unknown command\n");
        return 1;
    }

    return send_request(&req);
}
