// Linux and macOS: one running instance, found through a socket in the app's data folder.
#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "desk.h"

static void (*on_args)(int argc, char **argv);
static int server = -1;

static bool address(const char *data_dir, struct sockaddr_un *addr) {
    SDL_zerop(addr);
    addr->sun_family = AF_UNIX;
    return SDL_snprintf(addr->sun_path, sizeof addr->sun_path, "%s/instance.sock", data_dir) < (int)sizeof addr->sun_path;
}

// Arguments travel as their count, then each one ended by a zero byte.
static int listener(void *user) {
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; return 0; }
        char buf[4096];
        size_t len = 0;
        for (ssize_t n; len < sizeof buf - 1 && (n = read(fd, buf + len, sizeof buf - 1 - len)) > 0;) len += (size_t)n;
        close(fd);
        buf[len] = 0;
        char *argv[32];
        int argc = 0;
        for (char *p = buf; p < buf + len && argc < 32; p += strlen(p) + 1) argv[argc++] = p;
        if (argc) on_args(argc, argv);
    }
}

bool eh_instance_forward(const char *data_dir, int argc, char **argv) {
    struct sockaddr_un addr;
    if (!address(data_dir, &addr)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    bool ok = !connect(fd, (struct sockaddr *)&addr, sizeof addr);
    for (int i = 0; ok && i < argc; i++) ok = write(fd, argv[i], strlen(argv[i]) + 1) == (ssize_t)strlen(argv[i]) + 1;
    close(fd);
    return ok;
}

bool eh_instance_acquire(const char *data_dir, void (*args)(int argc, char **argv)) {
    struct sockaddr_un addr;
    if (!address(data_dir, &addr)) return true;  // a folder too deep for a socket: run without the guard
    // A socket that answers is a running instance. One that does not was left by a crash.
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    bool running = probe >= 0 && !connect(probe, (struct sockaddr *)&addr, sizeof addr);
    if (probe >= 0) close(probe);
    if (running) return false;
    unlink(addr.sun_path);
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof addr) || listen(server, 8)) return true;
    on_args = args;
    SDL_DetachThread(SDL_CreateThread(listener, "instance", NULL));
    return true;
}
