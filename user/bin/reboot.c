/* reboot - restart the machine */
#include <flux.h>
#include <stdio.h>

int main(void)
{
    reboot(REBOOT_RESTART);
    perror("reboot");
    return 1;
}
