/* poweroff - switch the machine off (ACPI), or halt it if that fails */
#include <flux.h>
#include <stdio.h>

int main(void)
{
    reboot(REBOOT_POWEROFF);
    perror("poweroff");
    printf("It is now safe to turn off your computer.\n");
    fflush(stdout);
    reboot(REBOOT_HALT);
    return 1;
}
