/* ask: reads lines you type in the Terminal and answers them.
 * Shows keyboard input reaching a user-mode program (read on fd 0). */
#include "jarvis.h"

int main(void)
{
    char name[64], line[128];
    print("What is your name? ");
    int64_t n = read(0, name, sizeof(name) - 1);
    if (n <= 0) return 1;
    name[n] = 0;
    if (n && name[n - 1] == '\n') name[--n] = 0;
    print("Hello, "); print(name); print(". Type lines to count their letters, 'quit' to stop.\n");
    for (;;) {
        print("> ");
        n = read(0, line, sizeof(line) - 1);
        if (n <= 0) break;
        line[n] = 0;
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (!strcmp(line, "quit")) break;
        print("  "); print_num((uint64_t)n); print(" characters\n");
    }
    print("Goodbye, "); print(name); print(".\n");
    return 0;
}
