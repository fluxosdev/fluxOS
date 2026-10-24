/*
 * The two 8237 DMA controllers of the ISA bus: channels 0-3 move bytes,
 * channels 4-7 (on the second controller, cascaded through 4) move 16-bit
 * words. A transfer must stay below 16 MiB and inside one 64 KiB page
 * (128 KiB for the 16-bit channels, which count in words). The floppy
 * driver programs channel 2 itself; this is for the sound card.
 */
#include "kernel.h"
#include "drivers.h"
#include "x86.h"

struct dma_ports {
    uint8_t addr, count, page;
};

static const struct dma_ports ports[8] = {
    { 0x00, 0x01, 0x87 }, { 0x02, 0x03, 0x83 }, { 0x04, 0x05, 0x81 }, { 0x06, 0x07, 0x82 },
    { 0xC0, 0xC2, 0x8F }, { 0xC4, 0xC6, 0x8B }, { 0xC8, 0xCA, 0x89 }, { 0xCC, 0xCE, 0x8A },
};

/* Each controller's single-mask, mode and flip-flop registers. */
#define MASK_REG(ch)     ((ch) < 4 ? 0x0A : 0xD4)
#define MODE_REG(ch)     ((ch) < 4 ? 0x0B : 0xD6)
#define FLIPFLOP_REG(ch) ((ch) < 4 ? 0x0C : 0xD8)

void isa_dma_stop(int ch)
{
    outb(MASK_REG(ch), (uint8_t)(0x04 | (ch & 3)));
}

int isa_dma_start(int ch, uint32_t phys, uint32_t len, uint8_t mode)
{
    if (ch < 0 || ch > 7 || ch == 4 || !len || phys + len > 0x1000000u)
        return -EINVAL;
    int wide = ch >= 4;
    uint32_t span = wide ? 0x20000u : 0x10000u;
    if ((phys & ~(span - 1)) != ((phys + len - 1) & ~(span - 1)))
        return -EINVAL;                 /* crosses a page */
    uint32_t addr = wide ? (phys >> 1) & 0xFFFF : phys & 0xFFFF;
    uint32_t count = (wide ? len / 2 : len) - 1;
    const struct dma_ports *p = &ports[ch];

    isa_dma_stop(ch);
    outb(FLIPFLOP_REG(ch), 0xFF);
    outb(p->addr, (uint8_t)addr);
    outb(p->addr, (uint8_t)(addr >> 8));
    outb(p->page, (uint8_t)((phys >> 16) & (wide ? 0xFE : 0xFF)));
    outb(FLIPFLOP_REG(ch), 0xFF);
    outb(p->count, (uint8_t)count);
    outb(p->count, (uint8_t)(count >> 8));
    outb(MODE_REG(ch), (uint8_t)(mode | (ch & 3)));
    outb(MASK_REG(ch), (uint8_t)(ch & 3));       /* unmask */
    return 0;
}
