#include <assert.h>
#include <sys/wait.h>
#include "../user/win32/messages.h"
int main(void)
{
    char dir[] = "/tmp/flux-message-test-XXXXXX";
    assert(mkdtemp(dir));
    int pipes[8][2], children[8];
    for (int i = 0; i < 8; i++) {
        assert(pipe(pipes[i]) == 0);
        children[i] = fork(); assert(children[i] >= 0);
        if (!children[i]) {
            uint32_t ids[2];
            /* Reverse registration order to exercise competing allocations. */
            for (int k = 0; k < 2; k++) {
                int n = (i + k) % 2;
                ids[n] = message_register_in(dir, (const uint16_t *)(n ? u"two" : u"one"), 3);
            }
            assert(write(pipes[i][1], ids, sizeof(ids)) == sizeof(ids));
            _exit(0);
        }
        close(pipes[i][1]);
    }
    uint32_t first[2] = {0};
    for (int i = 0; i < 8; i++) {
        uint32_t ids[2]; int status;
        assert(read(pipes[i][0], ids, sizeof(ids)) == sizeof(ids));
        close(pipes[i][0]);
        assert(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) && !WEXITSTATUS(status));
        assert(ids[0] >= 0xc000 && ids[1] <= 0xffff && ids[0] != ids[1]);
        if (!i) memcpy(first, ids, sizeof(ids));
        assert(!memcmp(first, ids, sizeof(ids)));
    }
    for (int i = 0; i < 2; i++) {
        char path[128]; snprintf(path, sizeof(path), "%s/%04x/name", dir, first[i] - 0xc000);
        assert(unlink(path) == 0);
        *strrchr(path, '/') = 0;
        assert(rmdir(path) == 0);
    }
    assert(rmdir(dir) == 0);
    puts("Win32 registered-message process regressions passed");
}
