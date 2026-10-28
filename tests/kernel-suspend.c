/* Runs as init in the disposable GRUB/QEMU suspend regression. */
#include <flux.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    struct powerinfo before, after;
    for (int i = 0; i < 3; i++) {
        if (power(POWER_INFO, (unsigned int)&before) < 0 ||
            !(before.flags & PWR_CAN_SLEEP))
            goto failed;
        printf("suspend: cycle %d\n", i + 1);
        fflush(stdout);
        if (power(POWER_SLEEP, 0) < 0)
            goto failed;
        /* Exercise the restored timer, scheduler and user-mode return. */
        msleep(100);
        if (power(POWER_INFO, (unsigned int)&after) < 0 ||
            after.sleeps != before.sleeps + 1)
            goto failed;
    }
    puts("SUSPEND REGRESSIONS PASSED");
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;) msleep(1000);
failed:
    puts("SUSPEND REGRESSIONS FAILED");
    fflush(stdout);
    reboot(REBOOT_POWEROFF);
    for (;;) msleep(1000);
}
