/* poweroff - switch the machine off (ACPI on QEMU/Bochs/VirtualBox) */
#include <flux.h>
#include <stdio.h>

int main(void)
{
    reboot(REBOOT_POWEROFF);
    perror("poweroff");
    return 1;
}
