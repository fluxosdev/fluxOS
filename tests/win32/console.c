/* A console program, which runs with no desktop: it exits with 5 if it found its own module. */
#include "winapi.h"

void start(void)
{
    W(ExitProcess)(W(GetModuleHandleA)(0) == (HMODULE)0x400000 ? 5 : 1);
}
