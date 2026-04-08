#include <fcntl.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"
int main() {
    int fd = open("/dev/container_monitor", O_RDWR);
    struct monitor_request req = {.pid = 5072, .container_id = "test"};
    ioctl(fd, MONITOR_REGISTER, &req);
    return 0;
}
