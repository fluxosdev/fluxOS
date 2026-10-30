/*
 * The kernel's random numbers, for /dev/random and /dev/urandom (the
 * same device): good enough for TLS keys.
 *
 * Entropy comes from timing. Every interrupt mixes the CPU's cycle
 * counter into a 32-byte pool, so keyboard, mouse, disk, network and
 * timer interrupts all contribute their jitter. Output is SHA-256 of the
 * pool, a counter and the cycle counter at that moment; after each read
 * the pool is replaced by a hash of itself, so what was read cannot be
 * worked back out of the pool later.
 */
#include "kernel.h"
#include "drivers.h"
#include <flux/sha256.h>

static uint32_t pool[8];
static uint32_t mix_at;
static uint32_t counter;
static spinlock_t random_lock = SPINLOCK_INIT("random");

static uint64_t cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Called from interrupt handlers: cheap, a rotate and an add. */
void random_add(uint32_t v)
{
    uint32_t c = (uint32_t)cycles() ^ v;
    spin_lock(&random_lock);
    uint32_t i = mix_at++ & 7;
    pool[i] = (pool[i] << 7 | pool[i] >> 25) ^ c;
    pool[(i + 3) & 7] += c * 2654435761u;
    spin_unlock(&random_lock);
}

/* buf may be a process's: each block is hashed under the lock, and copied
 * out after. */
void random_bytes(void *buf, uint32_t n)
{
    uint8_t *out = buf, block[SHA256_SIZE];
    while (n) {
        struct sha256 s;
        uint64_t t = cycles();
        spin_lock(&random_lock);
        uint32_t extra[3] = { counter++, (uint32_t)t, (uint32_t)(t >> 32) ^ ticks };
        sha256_init(&s);
        sha256_update(&s, pool, sizeof(pool));
        sha256_update(&s, extra, sizeof(extra));
        sha256_final(&s, block);
        spin_unlock(&random_lock);
        uint32_t k = n < SHA256_SIZE ? n : SHA256_SIZE;
        memcpy(out, block, k);
        out += k;
        n -= k;
    }
    /* Forward secrecy: the pool moves on. */
    struct sha256 s;
    spin_lock(&random_lock);
    sha256_init(&s);
    sha256_update(&s, pool, sizeof(pool));
    sha256_update(&s, "rekey", 5);
    sha256_final(&s, block);
    memcpy(pool, block, sizeof(pool));
    spin_unlock(&random_lock);
    memset(block, 0, sizeof(block));
}
