/* clear - clear the terminal screen */
#include <unistd.h>

int main(void)
{
    write(1, "\033[H\033[2J", 7);
    return 0;
}
