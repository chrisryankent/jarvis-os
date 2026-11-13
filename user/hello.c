/* hello: a normal user-mode program. Prints, uses the heap-free file API,
 * and exits. Run it from the Terminal with:  run hello */
#include "jarvis.h"

static char status[128];

int main(void)
{
    print("Hello from user mode! I am process ");
    print_num((uint64_t)getpid());
    print(".\n");

    int fd = open("/jarvis/data/hello.txt", O_WRITE | O_CREATE | O_TRUNC);
    if (fd >= 0) {
        const char *text = "Written by the hello program, running in user mode.\n";
        write(fd, text, strlen(text));
        close(fd);
        print("Wrote /jarvis/data/hello.txt\n");
    }

    if (syscall3(SYS_jarvis_status, (uint64_t)status, sizeof(status), 0) > 0) {
        print("Kernel status: ");
        print(status);
        print("\n");
    }

    /* A pointer into the kernel is refused by the system call, not obeyed. */
    int64_t rc = write(1, (const void *)0xFFFFFFFF80000000UL, 16);
    print("write() with a kernel address returned ");
    print(rc < 0 ? "an error (good).\n" : "success (BAD).\n");
    return 0;
}
