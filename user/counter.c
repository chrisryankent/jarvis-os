/* counter: counts once a second for 20 seconds. Useful for trying Suspend,
 * Resume and End task on a user program from Task Manager. */
#include "jarvis.h"

int main(void)
{
    for (int i = 1; i <= 20; i++) {
        print("counter: ");
        print_num((uint64_t)i);
        print("\n");
        sleep_ms(1000);
    }
    return 0;
}
