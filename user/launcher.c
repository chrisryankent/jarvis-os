/* launcher: starts another program, waits for it and reports how it ended.
 * Exercises spawn, wait4, getppid, time, clock_gettime, listdir, filesize,
 * rename and notify. Usage: run launcher (it starts /jarvis/bin/hello). */
#include "jarvis.h"

int main(void)
{
    print("launcher: my PID "); print_num((uint64_t)getpid());
    print(", parent "); print_num((uint64_t)getppid()); print("\n");
    print("launcher: clock says "); print_num((uint64_t)time_now()); print(" seconds since 1970\n");

    char list[1024];
    int64_t n = listdir("/jarvis/bin", list, sizeof(list));
    if (n >= 0) { print("launcher: /jarvis/bin holds:\n"); print(list); }

    print("launcher: starting /jarvis/bin/hello\n");
    uint64_t t0 = uptime_ms();
    int64_t pid = spawn("/jarvis/bin/hello");
    if (pid < 0) { print("launcher: could not start it\n"); return 1; }
    int code = -1;
    if (waitpid((int)pid, &code) < 0) { print("launcher: wait failed\n"); return 1; }
    print("launcher: PID "); print_num((uint64_t)pid); print(" ended with code "); print_num((uint64_t)code);
    print(" after "); print_num(uptime_ms() - t0); print(" ms\n");

    /* the file hello writes: rename it and back, check its size */
    int64_t sz = filesize("/jarvis/data/hello.txt");
    if (sz >= 0) {
        print("launcher: hello.txt is "); print_num((uint64_t)sz); print(" bytes\n");
        if (rename("/jarvis/data/hello.txt", "/jarvis/data/hello-renamed.txt") == 0 &&
            rename("/jarvis/data/hello-renamed.txt", "/jarvis/data/hello.txt") == 0)
            print("launcher: renamed it and back\n");
    }
    print("launcher: trying to end PID 1 (not mine): ");
    print(kill(1) < 0 ? "refused\n" : "ALLOWED (bug)\n");
    notify("Launcher", "hello finished; see the Terminal for details");
    return 0;
}
