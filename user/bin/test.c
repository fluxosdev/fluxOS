/*
 * test, [ - evaluate a condition (the shell has it as a builtin too; this
 * one is for make and find, which run it as a program)
 */
#include "../fsh/testexpr.c"

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    argv[0] = (char *)(base ? base + 1 : argv[0]);
    return test_eval(argc, argv);
}
