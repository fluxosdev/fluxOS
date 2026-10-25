#include "kernel-test.h"
#include "../kernel/heap.c"
void klog(const char *fmt, ...) { (void)fmt; }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
int main(void)
{
    static uint8_t pool[4096] __attribute__((aligned(16)));
    heap_head = (struct block *)pool;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = sizeof(pool) - sizeof(*heap_head);
    heap_head->free = 1;
    heap_size = sizeof(pool);
    uint8_t before[sizeof(pool)];
    memcpy(before, pool, sizeof(pool));
    CHECK(!kmalloc(UINT32_MAX));
    CHECK(!kmalloc(UINT32_MAX - 14));
    CHECK(!kzalloc(UINT32_MAX));
    CHECK(!kmalloc(sizeof(pool) + 1));
    CHECK(!memcmp(before, pool, sizeof(pool)));
    void *zero = kmalloc(0);
    CHECK(zero); kfree(zero);
    uint8_t *p = kmalloc(32);
    CHECK(p); memset(p, 0x5a, 32);
    memcpy(before, pool, sizeof(pool));
    CHECK(!krealloc(p, UINT32_MAX));
    CHECK(!memcmp(before, pool, sizeof(pool)));
    p = krealloc(p, 128);
    CHECK(p);
    for (int i = 0; i < 32; i++) CHECK(p[i] == 0x5a);
    kfree(p);
    CHECK(heap_used == 0 && !heap_head->next);
    puts("kernel heap: overflow rejection, realloc preservation and normal allocation passed");
}
