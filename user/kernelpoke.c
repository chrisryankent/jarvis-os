/* kernelpoke: tries to overwrite kernel memory directly. The CPU refuses
 * (user mode cannot touch kernel pages) and the kernel ends this program. */
#include "jarvis.h"

int main(void)
{
    print("kernelpoke: writing to kernel memory at 0xFFFFFFFF80000000...\n");
    *(volatile uint64_t *)0xFFFFFFFF80000000UL = 0;
    print("kernelpoke: the write succeeded. PROTECTION IS BROKEN.\n");
    return 1;
}
