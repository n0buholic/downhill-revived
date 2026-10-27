/* After build_host64_linux.py:
   clang -Wl,--gc-sections port/host/test_posix_pacing.c \
     build/host64-linux/obj/port_hle_host.c.o \
     build/host64-linux/obj/port_host_host_posix.c.o \
     -pthread -lm -ldl -o /tmp/test_posix_pacing && /tmp/test_posix_pacing */
#define _POSIX_C_SOURCE 200809L
#include "host_api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void host_settings_load(void);
void hle_log(const char *format, ...) { (void)format; }

int main(void)
{
    setenv("DHD_SETTINGS", "none", 1);
    unsetenv("DHD_LATENCY");
    host_settings_load();
    assert(getenv("DHD_LATENCY") && !strcmp(getenv("DHD_LATENCY"), "0"));

    setenv("DHD_LATENCY", "1", 1);
    host_settings_load();
    assert(!strcmp(getenv("DHD_LATENCY"), "1"));

    char path[] = "/tmp/dhd-pacing-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char setting[] = "DHD_LATENCY = 1\n";
    assert(write(fd, setting, sizeof setting - 1) == sizeof setting - 1);
    close(fd);
    setenv("DHD_SETTINGS", path, 1);
    unsetenv("DHD_LATENCY");
    host_settings_load();
    assert(!strcmp(getenv("DHD_LATENCY"), "1"));
    setenv("DHD_LATENCY", "0", 1);
    host_settings_load();
    assert(!strcmp(getenv("DHD_LATENCY"), "0"));
    unlink(path);

    for (int i = 0; i < 100; i++) {
        double due = host_wall_seconds() + 0.002;
        host_wait_wall(due);
        assert(host_wall_seconds() >= due);
    }
    host_wait_wall(host_wall_seconds() - 1);
    puts("PASS: Linux pacing default, INI/environment overrides, and wall deadlines");
    return 0;
}
