/*
 * Shared by the Radeon driver's 3D engine core (radeon_accel.c) and its
 * two generations: radeon_r700_accel.c (the HD 4000s) and
 * radeon_llano_accel.c (the Llano APUs, Evergreen). Registers whose
 * place the two agree on, the engine's memory, the ring.
 */
#ifndef RADEON_ACCEL_H
#define RADEON_ACCEL_H

#include <stdint.h>

/* Registers and PM4 packets */
#define VM_L2_CNTL      0x1400
#define ENABLE_L2_CACHE (1u << 0)
#define ENABLE_L2_FRAGMENT_PROCESSING (1u << 1)
#define ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE (1u << 9)
#define EFFECTIVE_L2_QUEUE_SIZE(x) (((x) & 7u) << 14)
#define VM_L2_CNTL2     0x1404
#define VM_L2_CNTL3     0x1408
#define BANK_SELECT(x)  ((x) << 0)
#define CACHE_UPDATE_MODE(x) ((x) << 6)
#define VM_CONTEXT0_CNTL 0x1410
#define ENABLE_CONTEXT  (1u << 0)
#define PAGE_TABLE_DEPTH(x) (((x) & 3u) << 1)
#define RANGE_PROTECTION_FAULT_ENABLE_DEFAULT (1u << 4)
#define VM_CONTEXT0_REQUEST_RESPONSE 0x1470
#define REQUEST_TYPE(x) (((x) & 0xFu) << 0)
#define RESPONSE_TYPE_MASK 0x000000F0
#define RESPONSE_TYPE_SHIFT 4
#define VM_CONTEXT0_INVALIDATION_LOW_ADDR 0x1490
#define VM_CONTEXT0_INVALIDATION_HIGH_ADDR 0x14B0
#define VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR 0x1518
#define VM_CONTEXT0_PAGE_TABLE_BASE_ADDR 0x153C
#define VM_CONTEXT0_PAGE_TABLE_START_ADDR 0x155C
#define VM_CONTEXT0_PAGE_TABLE_END_ADDR 0x157C
#define MC_VM_FB_LOCATION 0x2024
#define MC_VM_AGP_TOP   0x2028
#define MC_VM_AGP_BOT   0x202C
#define MC_VM_AGP_BASE  0x2030
#define MC_VM_SYSTEM_APERTURE_LOW_ADDR 0x2034
#define MC_VM_SYSTEM_APERTURE_HIGH_ADDR 0x2038
#define MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR 0x203C
#define MC_VM_MB_L1_TLB0_CNTL 0x2234
#define MC_VM_MB_L1_TLB1_CNTL 0x2238
#define MC_VM_MB_L1_TLB2_CNTL 0x223C
#define MC_VM_MB_L1_TLB3_CNTL 0x2240
#define ENABLE_L1_TLB   (1u << 0)
#define ENABLE_L1_FRAGMENT_PROCESSING (1u << 1)
#define SYSTEM_ACCESS_MODE_NOT_IN_SYS (3u << 3)
#define EFFECTIVE_L1_TLB_SIZE(x) ((x) << 15)
#define EFFECTIVE_L1_QUEUE_SIZE(x) ((x) << 18)
#define GRBM_STATUS     0x8010
#define GRBM_SOFT_RESET 0x8020
#define SOFT_RESET_CP   (1u << 0)
#define WAIT_UNTIL      0x8040
#define WAIT_3D_IDLE_bit      (1u << 15)
#define WAIT_3D_IDLECLEAN_bit (1u << 17)
#define SCRATCH_REG0    0x8500
#define SCRATCH_REG1    0x8504
#define SCRATCH_UMSK    0x8540
#define SCRATCH_ADDR    0x8544
#define CP_SEM_WAIT_TIMER 0x85BC
#define CP_ME_CNTL      0x86D8
#define CP_ME_HALT      (1u << 28)
#define CP_PFP_HALT     (1u << 26)
#define CP_RB_RPTR      0x8700
#define CP_RB_WPTR_DELAY 0x8704
#define CP_RB_BASE      0xC100
#define CP_RB_CNTL      0xC104
#define RB_BUFSZ(x)     ((x) << 0)
#define RB_BLKSZ(x)     ((x) << 8)
#define RB_NO_UPDATE    (1u << 27)
#define RB_RPTR_WR_ENA  (1u << 31)
#define CP_RB_RPTR_WR   0xC108
#define CP_RB_RPTR_ADDR 0xC10C
#define CP_RB_RPTR_ADDR_HI 0xC110
#define CP_RB_WPTR      0xC114
#define CP_PFP_UCODE_ADDR 0xC150
#define CP_PFP_UCODE_DATA 0xC154
#define CP_ME_RAM_RADDR 0xC158
#define CP_ME_RAM_WADDR 0xC15C
#define CP_ME_RAM_DATA  0xC160
#define CP_DEBUG        0xC1FC
#define PA_SC_SCREEN_SCISSOR_TL 0x28030
#define PA_SC_WINDOW_SCISSOR_TL 0x28204
#define PA_SC_GENERIC_SCISSOR_TL 0x28240
#define VGT_PRIMITIVE_TYPE 0x8958
#define PACKET2         0x80000000u
#define PACKET3(op, n)  (3u << 30 | ((op) & 0xFFu) << 8 | ((uint32_t)(n) & 0x3FFF) << 16)
#define PACKET3_INDEX_TYPE 0x2A
#define PACKET3_DRAW_INDEX_AUTO 0x2D
#define PACKET3_NUM_INSTANCES 0x2F
#define PACKET3_INDIRECT_BUFFER 0x32
#define PACKET3_SURFACE_SYNC 0x43
#define PACKET3_ME_INITIALIZE 0x44
#define PACKET3_EVENT_WRITE 0x46
#define PACKET3_SET_CONFIG_REG 0x68
#define PACKET3_SET_CONTEXT_REG 0x69
#define CONFIG_REG(r)   (((r) - 0x8000u) >> 2)
#define CONTEXT_REG(r)  (((r) - 0x28000u) >> 2)
#define PACKET3_CB0_DEST_BASE_ENA (1u << 6)
#define PACKET3_TC_ACTION_ENA (1u << 23)
#define PACKET3_VC_ACTION_ENA (1u << 24)
#define PACKET3_CB_ACTION_ENA (1u << 25)
#define PACKET3_SH_ACTION_ENA (1u << 27)
#define CACHE_FLUSH_AND_INV_EVENT 0x16
#define DI_PT_RECTLIST  0x11
#define DI_INDEX_SIZE_16_BIT 0
#define DI_SRC_SEL_AUTO_INDEX 2
#define FMT_5_6_5       0x08
#define FMT_8_8_8_8     0x1A

/* The engine's video memory, from ACCEL_VRAM_OFF */
#define RING_OFF    0x00000
#define RING_BYTES  0x10000
#define STATE_OFF   0x10000         /* the default state, then the shaders */
#define VB_OFF      0x12000         /* vertex data, 48 bytes a rectangle */
#define VB_BYTES    0x4000
#define FILL_OFF    0x16000         /* 64 texels of the fill color */
#define SCRATCH_OFF 0x17000         /* read back to flush the HDP */
#define TEST_OFF    0x20000         /* the self-test's surfaces */
#define PT_OFF      0x40000         /* the GART's page table: 8 bytes a page */
#define UIB_OFF     0x80000         /* a 3D client's command buffer, checked */
#define UIB_DWORDS  (64 * 1024)     /* the most one may have */

/* The GART: 64 MiB of GPU addresses right after video memory. */
#define GTT_PAGES   16384
#define PTE_VALID     (1u << 0)
#define PTE_SYSTEM    (1u << 1)
#define PTE_SNOOPED   (1u << 2)
#define PTE_READABLE  (1u << 5)
#define PTE_WRITEABLE (1u << 6)
#define MAX_SLOTS   16

/* The 3D client's video memory, after the engine's area. */
#define GL_VRAM_OFF  (64u << 20)
#define GL_VRAM_MAX  (128u << 20)
#define USER_GL_BASE (USER_MMIO_BASE + 0x04000000u)

/* A surface in video memory: a linear bitmap the engine reads or draws. */
struct surf {
    uint64_t addr;                  /* as the engine sees it */
    uint32_t pitch;                 /* pixels */
    uint32_t w, h;
    uint32_t format;                /* FMT_* */
    uint32_t cpp;                   /* bytes per pixel */
};

/* The ring, the default state and the blit shaders (radeon_accel.c) */
struct radeon_cp {
    uint8_t *cpu;                   /* the engine's area, as the CPU sees it */
    uint64_t gpu;
    uint32_t *ring;
    uint32_t wptr;                  /* dwords */
    uint32_t vs_off, ps_off, state_dwords;
    uint32_t vb_used;
    uint32_t seq;
    int busy;
};
extern struct radeon_cp rcp;

#define RING_MASK (RING_BYTES / 4 - 1)

int radeon_ring_space(uint32_t n);  /* room for n dwords, or -1: hung */
uint32_t radeon_popcount(uint32_t v);
uint32_t radeon_remap_render_backend(uint32_t tiling_pipe_num, uint32_t max_rb_num,
                                     uint32_t total_max_rb_num, uint32_t disabled_rb_mask);
int radeon_caller_owns(uint64_t addr, uint64_t bytes);  /* a 3D client's memory? */

/*
 * What differs between the generations: radeon_r700_accel.c (the HD
 * 4000s) and radeon_llano_accel.c (the Llano APUs).
 */
struct accel_gen {
    void (*setup)(void);                    /* the chip's limits, first */
    void (*md_tlbs)(uint32_t tlb);          /* the memory controller's MD client TLBs */
    uint32_t md_tlb0;                       /* the first of them, for the log */
    uint32_t vm_contexts;                   /* VM contexts to turn off */
    int gart_range_flush;                   /* a TLB flush names the GART's range */
    void (*gpu_init)(void);                 /* the engine's set-up, logged */
    void (*microcode)(const uint8_t **pfp, int *pfp_dwords, const uint8_t **me, int *me_dwords);
    uint32_t soft_reset;                    /* GRBM_SOFT_RESET bits besides the CP's */
    int reset_with_microcode;               /* reset before loading the microcode too */
    void (*hdp_flush)(void);
    void (*cp_regs)(void);                  /* before the ring starts, or NULL */
    uint32_t (*hw_contexts)(void);          /* for ME_INITIALIZE */
    int (*cp_preamble)(void);               /* after ME_INITIALIZE, or NULL */
    int (*begin_state)(void);               /* the start of every batch */
    void (*set_shaders)(uint64_t vs, uint64_t ps);
    void (*set_render_target)(const struct surf *s);
    void (*set_texture)(const struct surf *s);
    void (*set_vertices)(uint64_t addr);
    const uint32_t *state, *vs, *ps;        /* the default state, the blit shaders */
    uint32_t state_dwords, vs_bytes, ps_bytes;
    /* check a 3D client's command buffer; NULL: no 3D clients */
    int (*check)(const uint32_t *ib, uint32_t n, uint32_t *at, const char **why);
    int resumable;                          /* set up again after suspend to RAM */
};

extern const struct accel_gen radeon_r700_accel, radeon_llano_accel;

/* Writing the ring, after radeon_ring_space */
static inline void emit(uint32_t v)
{
    rcp.ring[rcp.wptr] = v;
    rcp.wptr = (rcp.wptr + 1) & RING_MASK;
}

static inline void set_config_reg(uint32_t reg, uint32_t v)
{
    emit(PACKET3(PACKET3_SET_CONFIG_REG, 1));
    emit(CONFIG_REG(reg));
    emit(v);
}

static inline void set_context_reg(uint32_t reg, uint32_t v)
{
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 1));
    emit(CONTEXT_REG(reg));
    emit(v);
}

static inline void surface_sync(uint32_t action, uint32_t size, uint64_t addr)
{
    emit(PACKET3(PACKET3_SURFACE_SYNC, 3));
    emit(action);
    emit((size + 255) >> 8);
    emit((uint32_t)(addr >> 8));
    emit(10);                       /* poll interval */
}

#endif
