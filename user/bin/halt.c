/* halt - stop the CPU */
#include <flux.h>
#include <stdio.h>

int main(void)
{
    reboot(REBOOT_HALT);
    perror("halt");
    return 1;
}
