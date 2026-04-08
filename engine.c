#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define CHILD_COMMAND_LEN 256

typedef enum { CMD_SUPERVISOR = 0, CMD_RUN } command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

typedef struct {
    int status;
    char message[256];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
} child_config_t;

int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;
    
    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname failed");
    }
    
    if (mount(config->rootfs, config->rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) perror("mount");
    if (chroot(config->rootfs) < 0) { perror("chroot"); exit(1); }
    if (chdir("/") < 0) perror("chdir");
    mount("proc", "/proc", "proc", 0, NULL);

    char *argv[] = {"/bin/sh", "-c", config->command, NULL};
    execv("/bin/sh", argv);
    return 1;
}

static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static int run_supervisor() {
    int monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0) perror("Warning: Monitor device not found");

    unlink(CONTROL_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }
    listen(server_fd, 5);
    signal(SIGCHLD, sigchld_handler);

    printf("Supervisor active on %s\n", CONTROL_PATH);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        control_request_t req;
        if (recv(client_fd, &req, sizeof(req), 0) > 0) {
            child_config_t *c_cfg = calloc(1, sizeof(child_config_t));
            snprintf(c_cfg->id, sizeof(c_cfg->id), "%s", req.container_id);
            snprintf(c_cfg->rootfs, sizeof(c_cfg->rootfs), "%s", req.rootfs);
            snprintf(c_cfg->command, sizeof(c_cfg->command), "%s", req.command);

            char *stack = malloc(STACK_SIZE);
            pid_t child_pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, c_cfg);

            if (child_pid > 0 && monitor_fd >= 0) {
                struct monitor_request m_req;
                memset(&m_req, 0, sizeof(m_req));
                m_req.pid = child_pid;
                m_req.soft_limit_bytes = req.soft_limit_bytes;
                m_req.hard_limit_bytes = req.hard_limit_bytes;
                snprintf(m_req.container_id, sizeof(m_req.container_id), "%s", req.container_id);
                ioctl(monitor_fd, MONITOR_REGISTER, &m_req);
                printf("Registered PID %d with kernel monitor\n", child_pid);
            }
        }
        control_response_t res = {0, "Success"};
        send(client_fd, &res, sizeof(res), 0);
        close(client_fd);
    }
    return 0;
}

static int send_request(control_request_t *req) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return 1;
    send(fd, req, sizeof(*req), 0);
    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor();
    if (strcmp(argv[1], "run") == 0 && argc >= 5) {
        control_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = CMD_RUN;
        snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
        snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
        snprintf(req.command, sizeof(req.command), "%s", argv[4]);
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--hard-mib") == 0) req.hard_limit_bytes = atol(argv[++i]) * 1024 * 1024;
            if (strcmp(argv[i], "--soft-mib") == 0) req.soft_limit_bytes = atol(argv[++i]) * 1024 * 1024;
        }
        return send_request(&req);
    }
    return 0;
}
