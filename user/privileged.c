/* privileged: tries to run a kernel-only instruction (disable interrupts).
 * In user mode that raises a general protection fault. */
#include "jarvis.h"

int main(void)
{
    print("privileged: trying to disable interrupts from user mode...\n");
    __asm__ volatile ("cli");
    print("privileged: interrupts disabled. MODE PROTECTION IS BROKEN.\n");
    return 1;
}
