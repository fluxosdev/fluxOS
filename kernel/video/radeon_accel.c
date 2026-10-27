/*
 * The Radeon HD 4000's 3D engine, driven through its command processor
 * (CP), as Linux's radeon driver brings it up on the R700 (rv770.c,
 * r600.c) and blits with it (r600_blit_kms.c, before Linux 3.12).
 *
 * At startup the driver sets the memory controller's client TLBs for
 * plain video memory (no GART: everything the engine reads or writes is
 * in VRAM), configures the engine's pipes, backends and shader resources
 * (rv770_gpu_init), loads AMD's PFP and ME microcode into the CP and
 * starts a 64 KiB ring of PM4 packets in video memory. The CPU writes the
 * ring through BAR0; after each batch the host data path (HDP) is flushed
 * so the CP sees the writes, and the write pointer is moved on.
 *
 * Fills and copies draw one rectangle each, with the static state and
 * shaders in radeon_blit_state.h: a texture (the source surface, or 64
 * texels of the fill color) sampled onto a linear render target. A fence,
 * a scratch register the CP writes after the engine is idle, tells when
 * a batch is done. A self-test fills and copies small surfaces and reads
 * them back; only if it passes does the desktop get DISP_COPYRECTS.
 *
 * The GART (VM context 0) maps ordinary memory into the GPU's address
 * space past video memory, one 4 KiB page at a time, through a page table
 * in video memory. Processes lend it buffers (DISP_MAPBUF) for DISP_BLIT:
 * the desktop has the engine copy its back buffer to the screen, the GL
 * library gets its pictures back that way. A process can take the 3D
 * engine (DISP_MAP3D): it gets 128 MiB of video memory for its buffers
 * and shaders, and runs command buffers it writes itself (DISP_SUBMIT).
 * Everything is synchronous: each call returns with the engine idle, so
 * no buffer is ever in use behind a process's back.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "radeon.h"
#include "radeon_blit_state.h"

/* Registers */
#define SRBM_STATUS     0x0E50
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
#define MC_VM_MD_L1_TLB0_CNTL 0x2654
#define MC_VM_MD_L1_TLB1_CNTL 0x2658
#define MC_VM_MD_L1_TLB2_CNTL 0x265C
#define MC_VM_MD_L1_TLB3_CNTL 0x2698
#define ENABLE_L1_TLB   (1u << 0)
#define ENABLE_L1_FRAGMENT_PROCESSING (1u << 1)
#define SYSTEM_ACCESS_MODE_NOT_IN_SYS (3u << 3)
#define EFFECTIVE_L1_TLB_SIZE(x) ((x) << 15)
#define EFFECTIVE_L1_QUEUE_SIZE(x) ((x) << 18)
#define MC_ARB_RAMCFG   0x2760
#define NOOFBANK_MASK   0x00000003
#define NOOFROWS_MASK   0x00000038
#define NOOFROWS_SHIFT  3
#define BURSTLENGTH_MASK 0x00000200
#define BURSTLENGTH_SHIFT 9
#define HDP_HOST_PATH_CNTL 0x2C00
#define HDP_DEBUG1      0x2F34
#define HDP_TILING_CONFIG 0x2F3C
#define CGTS_SYS_TCC_DISABLE 0x3F90
#define CGTS_USER_SYS_TCC_DISABLE 0x3F94
#define DMA_TILING_CONFIG 0x3EC8
#define DCP_TILING_CONFIG 0x6CA0
#define GRBM_CNTL       0x8000
#define GRBM_READ_TIMEOUT(x) ((x) << 0)
#define GRBM_STATUS     0x8010
#define GUI_ACTIVE      (1u << 31)
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
#define CP_QUEUE_THRESHOLDS 0x8760
#define ROQ_IB1_START(x) ((x) << 0)
#define ROQ_IB2_START(x) ((x) << 8)
#define CP_MEQ_THRESHOLDS 0x8764
#define STQ_SPLIT(x)    ((x) << 0)
#define CP_PERFMON_CNTL 0x87FC
#define VGT_CACHE_INVALIDATION 0x88C4
#define CACHE_INVALIDATION(x) ((x) << 0)
#define VC_AND_TC       2
#define TC_ONLY         1
#define AUTO_INVLD_EN(x) ((x) << 6)
#define ES_AND_GS_AUTO  3
#define VGT_GS_PER_ES   0x88C8
#define VGT_ES_PER_GS   0x88CC
#define VGT_GS_VERTEX_REUSE 0x88D4
#define VGT_GS_PER_VS   0x88E8
#define CC_GC_SHADER_PIPE_CONFIG 0x8950
#define INACTIVE_QD_PIPES_MASK 0x0000FF00
#define INACTIVE_QD_PIPES_SHIFT 8
#define VGT_NUM_INSTANCES 0x8974
#define PA_CL_ENHANCE   0x8A14
#define CLIP_VTX_REORDER_ENA (1u << 0)
#define NUM_CLIP_SEQ(x) ((x) << 1)
#define PA_SC_LINE_STIPPLE_STATE 0x8B10
#define PA_SC_MULTI_CHIP_CNTL 0x8B20
#define PA_SC_FORCE_EOV_MAX_CNTS 0x8B24
#define FORCE_EOV_MAX_CLK_CNT(x) ((x) << 0)
#define FORCE_EOV_MAX_REZ_CNT(x) ((x) << 16)
#define PA_SC_FIFO_SIZE 0x8BCC
#define SC_PRIM_FIFO_SIZE(x) ((x) << 0)
#define SC_HIZ_TILE_FIFO_SIZE(x) ((x) << 12)
#define SC_EARLYZ_TILE_FIFO_SIZE(x) ((x) << 20)
#define SQ_CONFIG       0x8C00
#define VC_ENABLE       (1u << 0)
#define EXPORT_SRC_C    (1u << 1)
#define DX9_CONSTS      (1u << 2)
#define ALU_INST_PREFER_VECTOR (1u << 3)
#define PS_PRIO(x)      ((uint32_t)(x) << 24)
#define VS_PRIO(x)      ((uint32_t)(x) << 26)
#define GS_PRIO(x)      ((uint32_t)(x) << 28)
#define ES_PRIO(x)      ((uint32_t)(x) << 30)
#define SQ_GPR_RESOURCE_MGMT_1 0x8C04
#define NUM_PS_GPRS(x)  ((uint32_t)(x) << 0)
#define NUM_VS_GPRS(x)  ((uint32_t)(x) << 16)
#define NUM_CLAUSE_TEMP_GPRS(x) ((uint32_t)(x) << 28)
#define SQ_GPR_RESOURCE_MGMT_2 0x8C08
#define NUM_GS_GPRS(x)  ((uint32_t)(x) << 0)
#define NUM_ES_GPRS(x)  ((uint32_t)(x) << 16)
#define SQ_THREAD_RESOURCE_MGMT 0x8C0C
#define NUM_PS_THREADS(x) ((uint32_t)(x) << 0)
#define NUM_VS_THREADS(x) ((uint32_t)(x) << 8)
#define NUM_GS_THREADS(x) ((uint32_t)(x) << 16)
#define NUM_ES_THREADS(x) ((uint32_t)(x) << 24)
#define SQ_STACK_RESOURCE_MGMT_1 0x8C10
#define NUM_PS_STACK_ENTRIES(x) ((uint32_t)(x) << 0)
#define NUM_VS_STACK_ENTRIES(x) ((uint32_t)(x) << 16)
#define SQ_STACK_RESOURCE_MGMT_2 0x8C14
#define NUM_GS_STACK_ENTRIES(x) ((uint32_t)(x) << 0)
#define NUM_ES_STACK_ENTRIES(x) ((uint32_t)(x) << 16)
#define SQ_MS_FIFO_SIZES 0x8CF0
#define CACHE_FIFO_SIZE(x) ((x) << 0)
#define FETCH_FIFO_HIWATER(x) ((x) << 8)
#define DONE_FIFO_HIWATER(x) ((x) << 16)
#define ALU_UPDATE_FIFO_HIWATER(x) ((uint32_t)(x) << 24)
#define SQ_DYN_GPR_SIZE_SIMD_AB_0 0x8DB0
#define SX_EXPORT_BUFFER_SIZES 0x900C
#define COLOR_BUFFER_SIZE(x) ((x) << 0)
#define POSITION_BUFFER_SIZE(x) ((x) << 8)
#define SMX_BUFFER_SIZE(x) ((x) << 16)
#define SX_DEBUG_1      0x9058
#define ENABLE_NEW_SMX_ADDRESS (1u << 16)
#define SPI_CONFIG_CNTL 0x9100
#define DISABLE_INTERP_1 (1u << 5)
#define SPI_CONFIG_CNTL_1 0x913C
#define VTX_DONE_DELAY(x) ((x) << 0)
#define CGTS_TCC_DISABLE 0x9148
#define CGTS_USER_TCC_DISABLE 0x914C
#define TA_CNTL_AUX     0x9508
#define DISABLE_CUBE_ANISO (1u << 1)
#define TCP_CNTL        0x9610
#define VC_ENHANCE      0x9714
#define DB_DEBUG3       0x98B0
#define DB_CLK_OFF_DELAY(x) ((x) << 11)
#define GB_TILING_CONFIG 0x98F0
#define PIPE_TILING(x)  ((x) << 1)
#define BANK_TILING(x)  ((x) << 4)
#define GROUP_SIZE(x)   ((x) << 6)
#define ROW_TILING(x)   ((x) << 8)
#define BANK_SWAPS(x)   ((x) << 11)
#define SAMPLE_SPLIT(x) ((x) << 14)
#define CC_RB_BACKEND_DISABLE 0x98F4
#define DB_DEBUG4       0x9B8C
#define DISABLE_TILE_COVERED_FOR_PS_ITER (1u << 6)
#define SMX_SAR_CTL0    0xA008
#define SMX_DC_CTL0     0xA020
#define CACHE_DEPTH(x)  ((x) << 1)
#define SMX_EVENT_CTL   0xA02C
#define ES_FLUSH_CTL(x) ((x) << 0)
#define GS_FLUSH_CTL(x) ((x) << 3)
#define ACK_FLUSH_CTL(x) ((x) << 6)
#define SYNC_FLUSH_CTL  (1u << 8)
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
#define DMA_TILING_CONFIG2 0xD0B8

/* Context registers, past BAR2's window */
#define CB_COLOR0_BASE  0x28040
#define CB_COLOR0_SIZE  0x28060
#define CB_COLOR0_VIEW  0x28080
#define CB_COLOR0_INFO  0x280A0
#define CB_FORMAT(x)    ((x) << 2)
#define CB_ARRAY_MODE(x) ((x) << 8)
#define CB_SOURCE_FORMAT(x) ((x) << 27)
#define CB_SF_EXPORT_NORM 1
#define ARRAY_LINEAR_ALIGNED 1
#define CB_COLOR0_TILE  0x280C0
#define CB_COLOR0_FRAG  0x280E0
#define CB_COLOR7_FRAG  0x280FC
#define CB_COLOR0_MASK  0x28100
#define PA_SC_SCREEN_SCISSOR_TL 0x28030
#define PA_SC_WINDOW_SCISSOR_TL 0x28204
#define PA_SC_CLIPRECT_RULE 0x2820C
#define PA_SC_EDGERULE  0x28230
#define PA_SC_GENERIC_SCISSOR_TL 0x28240
#define SX_MISC         0x28350
#define SPI_PS_IN_CONTROL_0 0x286CC
#define NUM_INTERP(x)   ((x) << 0)
#define SPI_INPUT_Z     0x286D8
#define SQ_PGM_START_PS 0x28840
#define SQ_PGM_RESOURCES_PS 0x28850
#define SQ_PGM_EXPORTS_PS 0x28854
#define SQ_PGM_START_VS 0x28858
#define SQ_PGM_RESOURCES_VS 0x28868
#define SQ_PGM_CF_OFFSET_PS 0x288CC
#define SQ_PGM_CF_OFFSET_VS 0x288D0
#define PA_SC_LINE_STIPPLE 0x28A0C
#define PA_SC_MODE_CNTL 0x28A4C
#define VGT_STRMOUT_EN  0x28AB0
#define PA_SC_AA_CONFIG 0x28C04
#define VGT_VERTEX_REUSE_BLOCK_CNTL 0x28C58
#define VGT_OUT_DEALLOC_CNTL 0x28C5C
#define VGT_PRIMITIVE_TYPE 0x8958

/* PM4 packets */
#define PACKET0(reg, n) ((((reg) >> 2) & 0xFFFFu) | ((uint32_t)(n) & 0x3FFF) << 16)
#define PACKET2         0x80000000u
#define PACKET3(op, n)  (3u << 30 | ((op) & 0xFFu) << 8 | ((uint32_t)(n) & 0x3FFF) << 16)
#define PACKET3_CONTEXT_CONTROL 0x28
#define PACKET3_INDEX_TYPE 0x2A
#define PACKET3_DRAW_INDEX_AUTO 0x2D
#define PACKET3_NUM_INSTANCES 0x2F
#define PACKET3_INDIRECT_BUFFER 0x32
#define PACKET3_SURFACE_SYNC 0x43
#define PACKET3_ME_INITIALIZE 0x44
#define PACKET3_EVENT_WRITE 0x46
#define PACKET3_SET_CONFIG_REG 0x68
#define PACKET3_SET_CONTEXT_REG 0x69
#define PACKET3_SET_RESOURCE 0x6D
#define CONFIG_REG(r)   (((r) - 0x8000u) >> 2)
#define CONTEXT_REG(r)  (((r) - 0x28000u) >> 2)
#define PACKET3_CB0_DEST_BASE_ENA (1u << 6)
#define PACKET3_TC_ACTION_ENA (1u << 23)
#define PACKET3_VC_ACTION_ENA (1u << 24)
#define PACKET3_CB_ACTION_ENA (1u << 25)
#define PACKET3_SH_ACTION_ENA (1u << 27)
#define CACHE_FLUSH_AND_INV_EVENT 0x16
#define CP_INT_STATUS   0xC128
#define DI_PT_RECTLIST  0x11
#define DI_INDEX_SIZE_16_BIT 0
#define DI_SRC_SEL_AUTO_INDEX 2
#define SQ_TEX_VTX_VALID_TEXTURE 2u
#define SQ_TEX_VTX_VALID_BUFFER 3u

/* Surface formats; the color buffer and texture codes agree. */
#define FMT_5_6_5       0x08
#define FMT_8_8_8_8     0x1A

#define PFP_UCODE_DWORDS 848
#define ME_UCODE_DWORDS  1360

extern const uint8_t radeon_fw_rv770_pfp[], radeon_fw_rv770_me[], radeon_fw_rv730_me[];
extern const uint8_t radeon_fw_rv710_pfp[], radeon_fw_rv710_me[];

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

/* Per-chip limits (rv770_gpu_init) and the blit's shader resources
 * (r600_blit_kms.c set_default_state). */
struct chip_config {
    uint32_t max_pipes, max_tile_pipes, max_simds, max_backends, max_gprs, max_threads;
    uint32_t max_stack_entries, max_hw_contexts, max_gs_threads;
    uint32_t sx_max_export_size, sx_max_export_pos_size, sx_max_export_smx_size;
    uint32_t sq_num_cf_insts, sx_num_of_sets, sc_prim_fifo_size, sc_hiz_tile_fifo_size;
    uint32_t sc_earlyz_tile_fifo_size;
    uint32_t blit_ps_gprs, blit_vs_gprs, blit_ps_threads, blit_vs_threads, blit_stack;
};

static const struct chip_config configs[] = {
    [CHIP_RV770] = { 4, 8, 10, 4, 256, 248, 512, 8, 32, 128, 16, 112, 2, 7, 0xF9, 0x30, 0x130,
                     192, 56, 188, 60, 256 },
    [CHIP_RV730] = { 2, 4, 8, 2, 128, 248, 256, 8, 32, 256, 16, 240, 2, 7, 0xF9, 0x30, 0x130,
                     84, 36, 188, 60, 128 },
    [CHIP_RV740] = { 4, 4, 8, 4, 256, 248, 512, 8, 32, 256, 16, 240, 2, 7, 0x100, 0x30, 0x130,
                     84, 36, 188, 60, 128 },
    [CHIP_RV710] = { 2, 2, 2, 1, 256, 192, 256, 4, 16, 128, 16, 112, 1, 7, 0x40, 0x30, 0x130,
                     192, 56, 144, 48, 128 },
};

static const struct chip_config *cfg;

/* A surface in video memory: a linear bitmap the engine reads or draws. */
struct surf {
    uint64_t addr;                  /* as the engine sees it */
    uint32_t pitch;                 /* pixels */
    uint32_t w, h;
    uint32_t format;                /* FMT_* */
    uint32_t cpp;                   /* bytes per pixel */
};

static struct {
    uint8_t *cpu;                   /* the engine's area, as the CPU sees it */
    uint64_t gpu;
    uint32_t *ring;
    uint32_t wptr;                  /* dwords */
    uint32_t vs_off, ps_off, state_dwords;
    uint32_t vb_used;
    uint32_t seq;
    int busy;
} cp;

#define RING_MASK (RING_BYTES / 4 - 1)

static struct {
    int on;
    uint64_t start;                 /* the first GART address */
    uint32_t dummy;                 /* the page stray accesses land on */
    volatile uint32_t *pt;
    struct slot {
        int pid;                    /* 0: free */
        uint32_t first, npages;     /* GART pages */
        uint32_t offset, size;      /* the buffer within them */
    } slots[MAX_SLOTS];
} gart;

static struct {
    int owner;                      /* pid with the 3D engine, or 0 */
    uint32_t size;
} gl3d;

static uint32_t popcount(uint32_t v)
{
    uint32_t n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

/* Sleep a little in a process; at boot, spin. */
static void pause_ms(void)
{
    if (current && current->pid > 0)
        sleep_ticks(1);
    else
        udelay(1000);
}

/* ---- memory controller and engine set-up (rv770.c) ---- */

/* The clients' TLBs pass addresses straight through to video memory
 * (rv770_agp_enable: no GART). The system aperture must cover video
 * memory first, as rv770_mc_program sets it; otherwise the display
 * controller's own reads stop reaching it once the TLBs are on. */
static int vm_init(void)
{
    uint32_t loc = rreg(MC_VM_FB_LOCATION);
    uint64_t vram_start = (uint64_t)(loc & 0xFFFF) << 24;
    uint64_t vram_end = (uint64_t)(loc >> 16) << 24 | 0xFFFFFF;
    klog("radeon: BIOS memory controller: FB_LOCATION %08x, system aperture %08x-%08x "
         "default %08x, AGP %08x-%08x base %08x, L2 %08x, TLB %08x/%08x\n", loc,
         rreg(MC_VM_SYSTEM_APERTURE_LOW_ADDR), rreg(MC_VM_SYSTEM_APERTURE_HIGH_ADDR),
         rreg(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR), rreg(MC_VM_AGP_BOT), rreg(MC_VM_AGP_TOP),
         rreg(MC_VM_AGP_BASE), rreg(VM_L2_CNTL), rreg(MC_VM_MD_L1_TLB0_CNTL),
         rreg(MC_VM_MB_L1_TLB0_CNTL));
    if (vram_end <= vram_start || rv.mc_base < vram_start ||
        rv.mc_base + VRAM_MAP_END > vram_end + 1) {
        klog("radeon: the screen (%08x%08x) is not where FB_LOCATION puts video memory\n",
             (uint32_t)(rv.mc_base >> 32), (uint32_t)rv.mc_base);
        return -1;
    }
    wreg(MC_VM_SYSTEM_APERTURE_LOW_ADDR, (uint32_t)(vram_start >> 12));
    wreg(MC_VM_SYSTEM_APERTURE_HIGH_ADDR, (uint32_t)(vram_end >> 12));
    wreg(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, (uint32_t)((cp.gpu + SCRATCH_OFF) >> 12));
    /* no AGP aperture: the GART's addresses go through VM context 0 */
    wreg(MC_VM_AGP_BASE, 0);
    wreg(MC_VM_AGP_TOP, 0x0FFFFFFF);
    wreg(MC_VM_AGP_BOT, 0x0FFFFFFF);

    wreg(VM_L2_CNTL, ENABLE_L2_CACHE | ENABLE_L2_FRAGMENT_PROCESSING |
                     ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE | EFFECTIVE_L2_QUEUE_SIZE(7));
    wreg(VM_L2_CNTL2, 0);
    wreg(VM_L2_CNTL3, BANK_SELECT(0) | CACHE_UPDATE_MODE(2));
    uint32_t tlb = ENABLE_L1_TLB | ENABLE_L1_FRAGMENT_PROCESSING | SYSTEM_ACCESS_MODE_NOT_IN_SYS |
                   EFFECTIVE_L1_TLB_SIZE(5) | EFFECTIVE_L1_QUEUE_SIZE(5);
    wreg(MC_VM_MD_L1_TLB0_CNTL, tlb);
    wreg(MC_VM_MD_L1_TLB1_CNTL, tlb);
    wreg(MC_VM_MD_L1_TLB2_CNTL, tlb);
    if (rv.family == CHIP_RV740)
        wreg(MC_VM_MD_L1_TLB3_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB0_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB1_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB2_CNTL, tlb);
    wreg(MC_VM_MB_L1_TLB3_CNTL, tlb);
    for (int i = 0; i < 7; i++)
        wreg(VM_CONTEXT0_CNTL + 4u * (uint32_t)i, 0);
    gart.start = ALIGN_UP(vram_end + 1, (uint64_t)4 << 20);
    return 0;
}

/* ---- the GART (rv770_pcie_gart_enable) ---- */

static void gart_set(uint32_t page, uint32_t phys)
{
    gart.pt[2 * page] = phys ? (phys & ~0xFFFu) | PTE_VALID | PTE_SYSTEM | PTE_SNOOPED |
                                   PTE_READABLE | PTE_WRITEABLE
                             : gart.dummy | PTE_SYSTEM;
    gart.pt[2 * page + 1] = 0;
}

static void hdp_flush(void);

/* Make the GPU forget the page table entries it has cached. */
static void gart_flush(void)
{
    hdp_flush();
    (void)gart.pt[0];
    wreg(VM_CONTEXT0_INVALIDATION_LOW_ADDR, (uint32_t)(gart.start >> 12));
    wreg(VM_CONTEXT0_INVALIDATION_HIGH_ADDR, (uint32_t)((gart.start + GTT_PAGES * 4096ull - 1) >> 12));
    wreg(VM_CONTEXT0_REQUEST_RESPONSE, REQUEST_TYPE(1));
    for (int i = 0; i < 100000; i++) {
        uint32_t r = (rreg(VM_CONTEXT0_REQUEST_RESPONSE) & RESPONSE_TYPE_MASK) >> RESPONSE_TYPE_SHIFT;
        if (r == 2) {
            klog("radeon: GART TLB flush failed\n");
            return;
        }
        if (r)
            return;
        udelay(1);
    }
}

static int gart_init(void)
{
    gart.dummy = pmm_alloc();
    if (!gart.dummy)
        return -1;
    memset((void *)gart.dummy, 0, PAGE_SIZE);
    gart.pt = (volatile uint32_t *)(cp.cpu + PT_OFF);
    for (uint32_t i = 0; i < GTT_PAGES; i++)
        gart_set(i, 0);
    wreg(VM_CONTEXT0_PAGE_TABLE_START_ADDR, (uint32_t)(gart.start >> 12));
    wreg(VM_CONTEXT0_PAGE_TABLE_END_ADDR, (uint32_t)((gart.start + GTT_PAGES * 4096ull - 1) >> 12));
    wreg(VM_CONTEXT0_PAGE_TABLE_BASE_ADDR, (uint32_t)((cp.gpu + PT_OFF) >> 12));
    wreg(VM_CONTEXT0_CNTL, ENABLE_CONTEXT | PAGE_TABLE_DEPTH(0) | RANGE_PROTECTION_FAULT_ENABLE_DEFAULT);
    wreg(VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR, gart.dummy >> 12);
    gart_flush();
    klog("radeon: GART of %u MiB at %08x%08x\n", GTT_PAGES / 256, (uint32_t)(gart.start >> 32),
         (uint32_t)gart.start);
    return 0;
}

/* Which render backend serves each tiling pipe (r6xx_remap_render_backend). */
static uint32_t remap_render_backend(uint32_t tiling_pipe_num, uint32_t max_rb_num,
                                     uint32_t total_max_rb_num, uint32_t disabled_rb_mask)
{
    uint32_t data = 0, mask = 1u << (max_rb_num - 1);
    uint32_t tmp = disabled_rb_mask | ((0xFFu << max_rb_num) & 0xFF);
    if ((tmp & 0xFF) != 0xFF)
        disabled_rb_mask = tmp;
    uint32_t rendering_pipe_num = 1u << tiling_pipe_num;
    uint32_t req_rb_num = total_max_rb_num - popcount(disabled_rb_mask);
    if (!req_rb_num)
        req_rb_num = 1;
    uint32_t pipe_rb_ratio = rendering_pipe_num / req_rb_num;
    uint32_t pipe_rb_remain = rendering_pipe_num - pipe_rb_ratio * req_rb_num;
    for (uint32_t i = 0; i < max_rb_num; i++) {
        if (!(mask & disabled_rb_mask)) {
            for (uint32_t j = 0; j < pipe_rb_ratio; j++)
                data = data << 2 | (max_rb_num - i - 1);
            if (pipe_rb_remain) {
                data = data << 2 | (max_rb_num - i - 1);
                pipe_rb_remain--;
            }
        }
        mask >>= 1;
    }
    return data;
}

static void gpu_init(void)
{
    /* the HDP's surface registers */
    for (uint32_t i = 0, j = 0; i < 32; i++, j += 0x18)
        for (uint32_t k = 0; k < 5; k++)
            wreg(0x2C14 + j + 4 * k, 0);

    wreg(GRBM_CNTL, GRBM_READ_TIMEOUT(0xFF));

    uint32_t mc_arb_ramcfg = rreg(MC_ARB_RAMCFG);
    uint32_t shader_pipe_config = rreg(CC_GC_SHADER_PIPE_CONFIG);
    uint32_t inactive_pipes = (shader_pipe_config & INACTIVE_QD_PIPES_MASK) >> INACTIVE_QD_PIPES_SHIFT;
    uint32_t active_pipes = 8 - popcount(inactive_pipes & 0xFF);
    wreg(SPI_CONFIG_CNTL, active_pipes == 1 ? DISABLE_INTERP_1 : 0);

    uint32_t gb_tiling_config;
    switch (cfg->max_tile_pipes) {
    case 2: gb_tiling_config = PIPE_TILING(1); break;
    case 4: gb_tiling_config = PIPE_TILING(2); break;
    case 8: gb_tiling_config = PIPE_TILING(3); break;
    default: gb_tiling_config = PIPE_TILING(0); break;
    }
    uint32_t disabled_rb_mask = (rreg(CC_RB_BACKEND_DISABLE) >> 16) & 0xFF;
    uint32_t all = (1u << cfg->max_backends) - 1;
    if ((disabled_rb_mask & all) == all)
        disabled_rb_mask &= ~all;
    gb_tiling_config |= remap_render_backend((gb_tiling_config >> 1) & 7, cfg->max_backends, 8,
                                             disabled_rb_mask) << 16;
    if (rv.family == CHIP_RV770 || (mc_arb_ramcfg & NOOFBANK_MASK))
        gb_tiling_config |= BANK_TILING(1);
    gb_tiling_config |= GROUP_SIZE((mc_arb_ramcfg & BURSTLENGTH_MASK) >> BURSTLENGTH_SHIFT);
    uint32_t rows = (mc_arb_ramcfg & NOOFROWS_MASK) >> NOOFROWS_SHIFT;
    if (rows > 3)
        rows = 3;
    gb_tiling_config |= ROW_TILING(rows) | SAMPLE_SPLIT(rows) | BANK_SWAPS(1);
    wreg(GB_TILING_CONFIG, gb_tiling_config);
    wreg(DCP_TILING_CONFIG, gb_tiling_config & 0xFFFF);
    wreg(HDP_TILING_CONFIG, gb_tiling_config & 0xFFFF);
    wreg(DMA_TILING_CONFIG, gb_tiling_config & 0xFFFF);
    wreg(DMA_TILING_CONFIG2, gb_tiling_config & 0xFFFF);

    wreg(CGTS_SYS_TCC_DISABLE, 0);
    wreg(CGTS_TCC_DISABLE, 0);
    wreg(CGTS_USER_SYS_TCC_DISABLE, 0);
    wreg(CGTS_USER_TCC_DISABLE, 0);

    uint32_t num_qd_pipes = 8 - popcount((shader_pipe_config & INACTIVE_QD_PIPES_MASK) >> 8);
    wreg(VGT_OUT_DEALLOC_CNTL, (num_qd_pipes * 4) & 0x7F);
    wreg(VGT_VERTEX_REUSE_BLOCK_CNTL, ((num_qd_pipes * 4) - 2) & 0xFF);

    wreg(CP_QUEUE_THRESHOLDS, ROQ_IB1_START(0x16) | ROQ_IB2_START(0x2B));
    wreg(CP_MEQ_THRESHOLDS, STQ_SPLIT(0x30));
    wreg(TA_CNTL_AUX, rreg(TA_CNTL_AUX) | DISABLE_CUBE_ANISO);
    wreg(SX_DEBUG_1, rreg(SX_DEBUG_1) | ENABLE_NEW_SMX_ADDRESS);
    uint32_t smx_dc_ctl0 = rreg(SMX_DC_CTL0) & ~CACHE_DEPTH(0x1FFu);
    wreg(SMX_DC_CTL0, smx_dc_ctl0 | CACHE_DEPTH(cfg->sx_num_of_sets * 64 - 1));
    if (rv.family != CHIP_RV740)
        wreg(SMX_EVENT_CTL, ES_FLUSH_CTL(4) | GS_FLUSH_CTL(4) | ACK_FLUSH_CTL(3) | SYNC_FLUSH_CTL);
    if (rv.family != CHIP_RV770)
        wreg(SMX_SAR_CTL0, 0x00003F3F);

    uint32_t db_debug3 = rreg(DB_DEBUG3) & ~DB_CLK_OFF_DELAY(0x1Fu);
    if (rv.family == CHIP_RV770 || rv.family == CHIP_RV740)
        db_debug3 |= DB_CLK_OFF_DELAY(0x1F);
    else
        db_debug3 |= DB_CLK_OFF_DELAY(2);
    wreg(DB_DEBUG3, db_debug3);
    if (rv.family != CHIP_RV770)
        wreg(DB_DEBUG4, rreg(DB_DEBUG4) | DISABLE_TILE_COVERED_FOR_PS_ITER);

    wreg(SX_EXPORT_BUFFER_SIZES, COLOR_BUFFER_SIZE(cfg->sx_max_export_size / 4 - 1) |
                                 POSITION_BUFFER_SIZE(cfg->sx_max_export_pos_size / 4 - 1) |
                                 SMX_BUFFER_SIZE(cfg->sx_max_export_smx_size / 4 - 1));
    wreg(PA_SC_FIFO_SIZE, SC_PRIM_FIFO_SIZE(cfg->sc_prim_fifo_size) |
                          SC_HIZ_TILE_FIFO_SIZE(cfg->sc_hiz_tile_fifo_size) |
                          SC_EARLYZ_TILE_FIFO_SIZE(cfg->sc_earlyz_tile_fifo_size));
    wreg(PA_SC_MULTI_CHIP_CNTL, 0);
    wreg(VGT_NUM_INSTANCES, 1);
    wreg(SPI_CONFIG_CNTL_1, VTX_DONE_DELAY(4));
    wreg(CP_PERFMON_CNTL, 0);

    wreg(SQ_MS_FIFO_SIZES, CACHE_FIFO_SIZE(16 * cfg->sq_num_cf_insts) | DONE_FIFO_HIWATER(0xE0) |
                           ALU_UPDATE_FIFO_HIWATER(0x8) |
                           FETCH_FIFO_HIWATER(rv.family == CHIP_RV740 ? 0x4 : 0x1));

    uint32_t sq_config = rreg(SQ_CONFIG) & ~(PS_PRIO(3) | VS_PRIO(3) | GS_PRIO(3) | ES_PRIO(3));
    sq_config |= DX9_CONSTS | VC_ENABLE | EXPORT_SRC_C | PS_PRIO(0) | VS_PRIO(1) | GS_PRIO(2) | ES_PRIO(3);
    if (rv.family == CHIP_RV710)
        sq_config &= ~VC_ENABLE;    /* no vertex cache */
    wreg(SQ_CONFIG, sq_config);
    wreg(SQ_GPR_RESOURCE_MGMT_1, NUM_PS_GPRS(cfg->max_gprs * 24 / 64) |
                                 NUM_VS_GPRS(cfg->max_gprs * 24 / 64) |
                                 NUM_CLAUSE_TEMP_GPRS(cfg->max_gprs * 24 / 64 / 2));
    wreg(SQ_GPR_RESOURCE_MGMT_2, NUM_GS_GPRS(cfg->max_gprs * 7 / 64) | NUM_ES_GPRS(cfg->max_gprs * 7 / 64));
    uint32_t threads = NUM_PS_THREADS(cfg->max_threads * 4 / 8) | NUM_VS_THREADS(cfg->max_threads * 2 / 8) |
                       NUM_ES_THREADS(cfg->max_threads * 1 / 8);
    if (cfg->max_threads / 8 > cfg->max_gs_threads)
        threads |= NUM_GS_THREADS(cfg->max_gs_threads);
    else
        threads |= NUM_GS_THREADS(cfg->max_gs_threads / 8);
    wreg(SQ_THREAD_RESOURCE_MGMT, threads);
    wreg(SQ_STACK_RESOURCE_MGMT_1, NUM_PS_STACK_ENTRIES(cfg->max_stack_entries / 4) |
                                   NUM_VS_STACK_ENTRIES(cfg->max_stack_entries / 4));
    wreg(SQ_STACK_RESOURCE_MGMT_2, NUM_GS_STACK_ENTRIES(cfg->max_stack_entries / 4) |
                                   NUM_ES_STACK_ENTRIES(cfg->max_stack_entries / 4));
    uint32_t ring = cfg->max_gprs * 38 / 64;
    uint32_t dyn = ring | ring << 8 | ring << 16 | ring << 24;
    for (uint32_t i = 0; i < 8; i++)
        wreg(SQ_DYN_GPR_SIZE_SIMD_AB_0 + 4 * i, dyn);

    wreg(PA_SC_FORCE_EOV_MAX_CNTS, FORCE_EOV_MAX_CLK_CNT(4095) | FORCE_EOV_MAX_REZ_CNT(255));
    wreg(VGT_CACHE_INVALIDATION, CACHE_INVALIDATION(rv.family == CHIP_RV710 ? TC_ONLY : VC_AND_TC) |
                                 AUTO_INVLD_EN(ES_AND_GS_AUTO));
    uint32_t gs_prim_buffer_depth = rv.family == CHIP_RV710 ? 128 : 384;
    uint32_t vgt_gs_per_es = MIN(gs_prim_buffer_depth + cfg->max_pipes * 16, 256u);
    wreg(VGT_ES_PER_GS, 128);
    wreg(VGT_GS_PER_ES, vgt_gs_per_es);
    wreg(VGT_GS_PER_VS, 2);
    wreg(VGT_GS_VERTEX_REUSE, 16);
    wreg(PA_SC_LINE_STIPPLE_STATE, 0);
    wreg(VGT_STRMOUT_EN, 0);
    wreg(SX_MISC, 0);
    wreg(PA_SC_MODE_CNTL, 0);
    wreg(PA_SC_EDGERULE, 0xAAAAAAAA);
    wreg(PA_SC_AA_CONFIG, 0);
    wreg(PA_SC_CLIPRECT_RULE, 0xFFFF);
    wreg(PA_SC_LINE_STIPPLE, 0);
    wreg(SPI_INPUT_Z, 0);
    wreg(SPI_PS_IN_CONTROL_0, NUM_INTERP(2));
    wreg(CB_COLOR7_FRAG, 0);
    for (uint32_t i = 0; i < 8; i++)
        wreg(CB_COLOR0_BASE + 4 * i, 0);
    wreg(TCP_CNTL, 0);
    wreg(HDP_HOST_PATH_CNTL, rreg(HDP_HOST_PATH_CNTL));
    wreg(PA_SC_MULTI_CHIP_CNTL, 0);
    wreg(PA_CL_ENHANCE, CLIP_VTX_REORDER_ENA | NUM_CLIP_SEQ(3));
    wreg(VC_ENHANCE, 0);
}

/* ---- the command processor ---- */

static void cp_halt(void)
{
    wreg(CP_ME_CNTL, CP_ME_HALT | CP_PFP_HALT);
    wreg(SCRATCH_UMSK, 0);
}

static void cp_soft_reset(void)
{
    wreg(GRBM_SOFT_RESET, SOFT_RESET_CP);
    (void)rreg(GRBM_SOFT_RESET);
    mdelay(15);
    wreg(GRBM_SOFT_RESET, 0);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void load_microcode(void)
{
    const uint8_t *pfp = radeon_fw_rv770_pfp, *me = radeon_fw_rv770_me;
    if (rv.family == CHIP_RV730 || rv.family == CHIP_RV740) {
        pfp = radeon_fw_rv710_pfp;  /* the RV730's PFP is the RV710's */
        me = radeon_fw_rv730_me;
    } else if (rv.family == CHIP_RV710) {
        pfp = radeon_fw_rv710_pfp;
        me = radeon_fw_rv710_me;
    }
    cp_halt();
    wreg(CP_RB_CNTL, RB_NO_UPDATE | RB_BLKSZ(15) | RB_BUFSZ(3));
    cp_soft_reset();
    wreg(CP_PFP_UCODE_ADDR, 0);
    for (int i = 0; i < PFP_UCODE_DWORDS; i++)
        wreg(CP_PFP_UCODE_DATA, be32(pfp + 4 * i));
    wreg(CP_PFP_UCODE_ADDR, 0);
    wreg(CP_ME_RAM_WADDR, 0);
    for (int i = 0; i < ME_UCODE_DWORDS; i++)
        wreg(CP_ME_RAM_DATA, be32(me + 4 * i));
    wreg(CP_PFP_UCODE_ADDR, 0);
    wreg(CP_ME_RAM_WADDR, 0);
    wreg(CP_ME_RAM_RADDR, 0);
}

/* The engine reads what the CPU wrote to video memory only after the HDP
 * is flushed; on the R700, by writing HDP_DEBUG1 and reading memory back. */
static void hdp_flush(void)
{
    wreg(HDP_DEBUG1, 0);
    (void)*(volatile uint32_t *)(cp.cpu + SCRATCH_OFF);
}

static void hang(const char *what)
{
    klog("radeon: the 3D engine hung (%s); GRBM_STATUS %08x, ring read %u, written %u\n", what,
         rreg(GRBM_STATUS), rreg(CP_RB_RPTR), cp.wptr);
    cp_halt();
    rv.accel = 0;
}

/* Wait for room for n dwords, and the padding a commit adds. */
static int ring_space(uint32_t n)
{
    if (!rv.accel)
        return -1;                  /* hung earlier */
    n += 16;
    for (int waited = 0; waited < 1000; waited++) {
        uint32_t free = (rreg(CP_RB_RPTR) - cp.wptr - 1) & RING_MASK;
        if (free >= n)
            return 0;
        if (waited < 100)
            udelay(10);
        else
            pause_ms();
    }
    hang("ring full");
    return -1;
}

static void emit(uint32_t v)
{
    cp.ring[cp.wptr] = v;
    cp.wptr = (cp.wptr + 1) & RING_MASK;
}

static void commit(void)
{
    while (cp.wptr & 15)
        emit(PACKET2);
    hdp_flush();
    wreg(CP_RB_WPTR, cp.wptr);
    (void)rreg(CP_RB_WPTR);
}

static void set_config_reg(uint32_t reg, uint32_t v)
{
    emit(PACKET3(PACKET3_SET_CONFIG_REG, 1));
    emit(CONFIG_REG(reg));
    emit(v);
}

static void set_context_reg(uint32_t reg, uint32_t v)
{
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 1));
    emit(CONTEXT_REG(reg));
    emit(v);
}

/* Have the CP write a sequence number once everything before it is done
 * and flushed, and wait for it. */
static int finish(void)
{
    if (ring_space(20) < 0)
        return -1;
    emit(PACKET3(PACKET3_SURFACE_SYNC, 3));
    emit(PACKET3_TC_ACTION_ENA | PACKET3_VC_ACTION_ENA | PACKET3_SH_ACTION_ENA);
    emit(0xFFFFFFFF);
    emit(0);
    emit(10);
    emit(PACKET3(PACKET3_EVENT_WRITE, 0));
    emit(CACHE_FLUSH_AND_INV_EVENT);
    set_config_reg(WAIT_UNTIL, WAIT_3D_IDLE_bit | WAIT_3D_IDLECLEAN_bit);
    uint32_t seq = ++cp.seq;
    set_config_reg(SCRATCH_REG1, seq);
    commit();
    for (int waited = 0; waited < 1000; waited++) {
        if ((int32_t)(rreg(SCRATCH_REG1) - seq) >= 0)
            return 0;
        if (waited < 200)
            udelay(10);
        else
            pause_ms();
    }
    hang("fence not reached");
    return -1;
}

static int cp_start(void)
{
    cp_soft_reset();
    uint32_t bufsz = 13, cntl = 9u << 8 | bufsz;    /* log2 of 64 KiB / 8, of 4 KiB / 8 */
    wreg(CP_RB_CNTL, cntl);
    wreg(CP_SEM_WAIT_TIMER, 0);
    wreg(CP_RB_WPTR_DELAY, 0);
    wreg(CP_RB_CNTL, cntl | RB_RPTR_WR_ENA);
    wreg(CP_RB_RPTR_WR, 0);
    cp.wptr = 0;
    wreg(CP_RB_WPTR, 0);
    uint64_t scratch = cp.gpu + SCRATCH_OFF;
    wreg(CP_RB_RPTR_ADDR, (uint32_t)scratch & ~3u);
    wreg(CP_RB_RPTR_ADDR_HI, (uint32_t)(scratch >> 32) & 0xFF);
    wreg(SCRATCH_ADDR, (uint32_t)(scratch >> 8));
    wreg(SCRATCH_UMSK, 0);          /* no write-back: the CPU reads the registers */
    wreg(SCRATCH_REG1, 0);          /* the fence, before its first number */
    cp.seq = 0;
    mdelay(1);
    wreg(CP_RB_CNTL, cntl | RB_NO_UPDATE);
    wreg(CP_RB_BASE, (uint32_t)((cp.gpu + RING_OFF) >> 8));
    wreg(CP_DEBUG, (1u << 27) | (1u << 28));

    emit(PACKET3(PACKET3_ME_INITIALIZE, 5));
    emit(0x1);
    emit(0x0);
    emit(cfg->max_hw_contexts - 1);
    emit(1u << 16);                 /* PACKET3_ME_INITIALIZE_DEVICE_ID(1) */
    emit(0);
    emit(0);
    commit();
    wreg(CP_ME_CNTL, 0xFF);

    /* The ring test: the CP writes a scratch register. */
    wreg(SCRATCH_REG0, 0xCAFEDEAD);
    if (ring_space(3) < 0)
        return -1;
    set_config_reg(SCRATCH_REG0, 0xDEADBEEF);
    commit();
    for (int i = 0; i < 100000; i++) {
        if (rreg(SCRATCH_REG0) == 0xDEADBEEF)
            return 0;
        udelay(1);
    }
    klog("radeon: CP ring test failed (scratch %08x)\n", rreg(SCRATCH_REG0));
    return -1;
}

/* ---- blits (r600_blit_kms.c) ---- */

/* An integer as an IEEE single, exact up to 2^24 (int2float). */
static uint32_t int2float(uint32_t x)
{
    if (!x)
        return 0;
    int msb = 31 - __builtin_clz(x);
    uint32_t shift = (uint32_t)(msb - 23) & 31;
    uint32_t fraction = ((x >> shift) | (x << ((32 - shift) & 31))) & ((1u << 23) - 1);
    return fraction + ((uint32_t)(127 + msb) << 23);
}

static void surface_sync(uint32_t action, uint32_t size, uint64_t addr)
{
    emit(PACKET3(PACKET3_SURFACE_SYNC, 3));
    emit(action);
    emit((size + 255) >> 8);
    emit((uint32_t)(addr >> 8));
    emit(10);                       /* poll interval */
}

/* The default state as an indirect buffer and the shader resources: what
 * every batch starts with, blits and a 3D client's command buffers. */
static int begin_state(void)
{
    if (ring_space(16) < 0)
        return -1;
    uint64_t state = cp.gpu + STATE_OFF;
    emit(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
    emit((uint32_t)state & ~3u);
    emit((uint32_t)(state >> 32) & 0xFF);
    emit(cp.state_dwords);

    uint32_t sq_config = (rv.family == CHIP_RV710 ? 0 : VC_ENABLE) | DX9_CONSTS |
                         ALU_INST_PREFER_VECTOR | PS_PRIO(0) | VS_PRIO(1) | GS_PRIO(2) | ES_PRIO(3);
    emit(PACKET3(PACKET3_SET_CONFIG_REG, 6));
    emit(CONFIG_REG(SQ_CONFIG));
    emit(sq_config);
    emit(NUM_PS_GPRS(cfg->blit_ps_gprs) | NUM_VS_GPRS(cfg->blit_vs_gprs) | NUM_CLAUSE_TEMP_GPRS(4));
    emit(0);
    emit(NUM_PS_THREADS(cfg->blit_ps_threads) | NUM_VS_THREADS(cfg->blit_vs_threads));
    emit(NUM_PS_STACK_ENTRIES(cfg->blit_stack) | NUM_VS_STACK_ENTRIES(cfg->blit_stack));
    emit(0);
    return 0;
}

/* The default state and the two blit shaders. */
static int begin_blits(void)
{
    if (begin_state() < 0 || ring_space(32) < 0)
        return -1;
    uint64_t vs = cp.gpu + cp.vs_off, ps = cp.gpu + cp.ps_off;
    set_context_reg(SQ_PGM_START_VS, (uint32_t)(vs >> 8));
    set_context_reg(SQ_PGM_RESOURCES_VS, 1);
    set_context_reg(SQ_PGM_CF_OFFSET_VS, 0);
    set_context_reg(SQ_PGM_START_PS, (uint32_t)(ps >> 8));
    set_context_reg(SQ_PGM_RESOURCES_PS, 1 | (1u << 28));
    set_context_reg(SQ_PGM_EXPORTS_PS, 2);
    set_context_reg(SQ_PGM_CF_OFFSET_PS, 0);
    surface_sync(PACKET3_SH_ACTION_ENA, 512, vs);
    cp.vb_used = 0;
    return 0;
}

static void set_render_target(const struct surf *s)
{
    uint32_t h = MAX(ALIGN_UP(s->h, 8u), 8u);
    set_context_reg(CB_COLOR0_BASE, (uint32_t)(s->addr >> 8));
    set_context_reg(CB_COLOR0_SIZE, (s->pitch / 8 - 1) | (s->pitch * h / 64 - 1) << 10);
    set_context_reg(CB_COLOR0_VIEW, 0);
    set_context_reg(CB_COLOR0_INFO, CB_FORMAT(s->format) | CB_SOURCE_FORMAT(CB_SF_EXPORT_NORM) |
                                    CB_ARRAY_MODE(ARRAY_LINEAR_ALIGNED));
    set_context_reg(CB_COLOR0_TILE, 0);
    set_context_reg(CB_COLOR0_FRAG, 0);
    set_context_reg(CB_COLOR0_MASK, 0);
}

static void set_texture(const struct surf *s)
{
    uint32_t word0 = 1u /* 2D */ | (uint32_t)ARRAY_LINEAR_ALIGNED << 3 |
                     ((s->pitch / 8 - 1) & 0x7FF) << 8 | ((s->w - 1) & 0x1FFF) << 19;
    uint32_t word1 = (s->format & 0x3F) << 26 | ((MAX(s->h, 1u) - 1) & 0x1FFF);
    uint32_t word4 = 1u << 14 /* request size */ | 0u << 16 | 1u << 19 | 2u << 22 | 3u << 25;
    surface_sync(PACKET3_TC_ACTION_ENA, s->pitch * s->h * s->cpp, s->addr);
    emit(PACKET3(PACKET3_SET_RESOURCE, 7));
    emit(0);
    emit(word0);
    emit(word1);
    emit((uint32_t)(s->addr >> 8));
    emit((uint32_t)(s->addr >> 8));
    emit(word4);
    emit(0);
    emit(SQ_TEX_VTX_VALID_TEXTURE << 30);
}

static void set_scissors(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(PA_SC_SCREEN_SCISSOR_TL));
    emit(x1 | y1 << 16);
    emit(x2 | y2 << 16);
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(PA_SC_GENERIC_SCISSOR_TL));
    emit(x1 | y1 << 16 | 1u << 31);
    emit(x2 | y2 << 16);
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(PA_SC_WINDOW_SCISSOR_TL));
    emit(x1 | y1 << 16 | 1u << 31);
    emit(x2 | y2 << 16);
}

static void set_vertices(uint64_t addr)
{
    emit(PACKET3(PACKET3_SET_RESOURCE, 7));
    emit(0x460);
    emit((uint32_t)addr);
    emit(48 - 1);
    emit(((uint32_t)(addr >> 32) & 0xFF) | 16u << 8);      /* stride 16 */
    emit(1);
    emit(0);
    emit(0);
    emit(SQ_TEX_VTX_VALID_BUFFER << 30);
    surface_sync(rv.family == CHIP_RV710 ? PACKET3_TC_ACTION_ENA : PACKET3_VC_ACTION_ENA, 48, addr);
}

static void draw_auto(void)
{
    set_config_reg(VGT_PRIMITIVE_TYPE, DI_PT_RECTLIST);
    emit(PACKET3(PACKET3_INDEX_TYPE, 0));
    emit(DI_INDEX_SIZE_16_BIT);
    emit(PACKET3(PACKET3_NUM_INSTANCES, 0));
    emit(1);
    emit(PACKET3(PACKET3_DRAW_INDEX_AUTO, 1));
    emit(3);
    emit(DI_SRC_SEL_AUTO_INDEX);
}

/* Draw src's w x h at (sx, sy) onto dst at (dx, dy). Returns 1 when the
 * vertex area is full: fence, wait and begin again. */
static int blit(const struct surf *src, const struct surf *dst, uint32_t sx, uint32_t sy,
                uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    if (cp.vb_used + 48 > VB_BYTES)
        return 1;
    uint32_t *v = (uint32_t *)(cp.cpu + VB_OFF + cp.vb_used);
    uint64_t vb = cp.gpu + VB_OFF + cp.vb_used;
    cp.vb_used += 48;
    /* a RECTLIST: three corners, each position then texture coordinate */
    v[0] = int2float(dx);     v[1] = int2float(dy);     v[2] = int2float(sx);     v[3] = int2float(sy);
    v[4] = int2float(dx);     v[5] = int2float(dy + h); v[6] = int2float(sx);     v[7] = int2float(sy + h);
    v[8] = int2float(dx + w); v[9] = int2float(dy + h); v[10] = int2float(sx + w); v[11] = int2float(sy + h);
    if (ring_space(90) < 0)
        return -1;
    set_texture(src);
    set_render_target(dst);
    set_scissors(dx, dy, dx + w, dy + h);
    set_vertices(vb);
    draw_auto();
    surface_sync(PACKET3_CB_ACTION_ENA | PACKET3_CB0_DEST_BASE_ENA, dst->pitch * dst->h * dst->cpp,
                 dst->addr);
    return 0;
}

/* Blit, starting over when the vertex area fills up. */
static int blit_or_flush(const struct surf *src, const struct surf *dst, uint32_t sx, uint32_t sy,
                         uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    int r = blit(src, dst, sx, sy, dx, dy, w, h);
    if (r == 1) {
        if (finish() < 0 || begin_blits() < 0)
            return -1;
        r = blit(src, dst, sx, sy, dx, dy, w, h);
    }
    return r;
}

/* The 64 texels the fill shader samples, all of one color. */
static struct surf fill_surface(uint32_t color, uint32_t format, uint32_t cpp)
{
    struct surf s = { cp.gpu + FILL_OFF, 64, 64, 1, format, cpp };
    if (cpp == 2) {
        uint16_t *t = (uint16_t *)(cp.cpu + FILL_OFF);
        for (int i = 0; i < 64; i++)
            t[i] = (uint16_t)color;
    } else {
        uint32_t *t = (uint32_t *)(cp.cpu + FILL_OFF);
        for (int i = 0; i < 64; i++)
            t[i] = color;
    }
    return s;
}

/* ---- the self-test ---- */

#define TW 256                      /* the test surfaces: 256 x 64, 32 bits */
#define TH 64

static uint32_t pattern(uint32_t i)
{
    return i * 0x9E3779B1u ^ 0x5A5A5A5Au;
}

static int check(const char *what, const uint32_t *px, uint32_t x0, uint32_t y0, uint32_t w,
                 uint32_t h, int copy, uint32_t sx, uint32_t sy, uint32_t color)
{
    for (uint32_t y = 0; y < TH; y++) {
        for (uint32_t x = 0; x < TW; x++) {
            int inside = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
            uint32_t want = !inside ? 0 : copy ? pattern((y - y0 + sy) * TW + (x - x0 + sx)) : color;
            if (px[y * TW + x] != want) {
                klog("radeon: %s test: pixel (%u,%u) is %08x, not %08x\n", what, x, y,
                     px[y * TW + x], want);
                return -1;
            }
        }
    }
    return 0;
}

static int self_test(void)
{
    uint32_t *a = (uint32_t *)(cp.cpu + TEST_OFF), *b = (uint32_t *)(cp.cpu + TEST_OFF + TW * TH * 4);
    struct surf sa = { cp.gpu + TEST_OFF, TW, TW, TH, FMT_8_8_8_8, 4 };
    struct surf sb = { cp.gpu + TEST_OFF + TW * TH * 4, TW, TW, TH, FMT_8_8_8_8, 4 };

    memset(a, 0, TW * TH * 4);
    struct surf fill = fill_surface(0xFF336699, FMT_8_8_8_8, 4);
    if (begin_blits() < 0 || blit(&fill, &sa, 0, 0, 8, 4, 100, 20) != 0 || finish() < 0)
        return -1;
    hdp_flush();
    if (check("fill", a, 8, 4, 100, 20, 0, 0, 0, 0xFF336699) < 0)
        return -1;

    for (uint32_t i = 0; i < TW * TH; i++)
        a[i] = pattern(i);
    memset(b, 0, TW * TH * 4);
    if (begin_blits() < 0 || blit(&sa, &sb, 3, 2, 40, 9, 120, 30) != 0 || finish() < 0)
        return -1;
    hdp_flush();
    return check("copy", b, 40, 9, 120, 30, 1, 3, 2, 0);
}

/* The engine reads a page of ordinary memory through the GART, then draws
 * into it. */
static int gart_test(void)
{
    uint32_t page = pmm_alloc();
    if (!page)
        return -1;
    uint32_t *p = (uint32_t *)page, *a = (uint32_t *)(cp.cpu + TEST_OFF);
    for (uint32_t i = 0; i < 1024; i++)
        p[i] = pattern(i);
    memset(a, 0, TW * TH * 4);
    gart_set(GTT_PAGES - 1, page);
    gart_flush();
    struct surf g = { gart.start + (GTT_PAGES - 1) * 4096ull, 64, 64, 16, FMT_8_8_8_8, 4 };
    struct surf sa = { cp.gpu + TEST_OFF, TW, TW, TH, FMT_8_8_8_8, 4 };
    int r = -1;
    if (begin_blits() < 0 || blit(&g, &sa, 0, 0, 10, 3, 64, 16) != 0 || finish() < 0)
        goto out;
    hdp_flush();
    for (uint32_t y = 0; y < 16; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            if (a[(y + 3) * TW + x + 10] != pattern(y * 64 + x)) {
                klog("radeon: GART read test: pixel (%u,%u) is %08x, not %08x\n", x, y,
                     a[(y + 3) * TW + x + 10], pattern(y * 64 + x));
                goto out;
            }
        }
    }
    struct surf fill = fill_surface(0xFF13579B, FMT_8_8_8_8, 4);
    if (begin_blits() < 0 || blit(&fill, &g, 0, 0, 0, 0, 64, 16) != 0 || finish() < 0)
        goto out;
    for (uint32_t i = 0; i < 1024; i++) {
        if (p[i] != 0xFF13579B) {
            klog("radeon: GART write test: word %u is %08x\n", i, p[i]);
            goto out;
        }
    }
    r = 0;
out:
    gart_set(GTT_PAGES - 1, 0);
    gart_flush();
    pmm_free(page);
    return r;
}

/* ---- the interface ---- */

/* Can the engine draw on the screen's pages? */
static int screen_ok(void)
{
    return (fb.bpp == 16 || fb.bpp == 32) && (fb.pitch / fb.bytespp) % 64 == 0;
}

int radeon_accel_ok(void)
{
    return rv.accel && rv.flip && screen_ok();
}

int radeon_gart_ok(void)
{
    return rv.accel && gart.on;
}

int radeon_3d_ok(void)
{
    return rv.accel && rv.aper_size > GL_VRAM_OFF && rv.vram_size > GL_VRAM_OFF;
}

/* ---- buffers lent to the GPU ---- */

static struct slot *find_slot(uint32_t handle)
{
    if (handle < 1 || handle > MAX_SLOTS)
        return NULL;
    struct slot *sl = &gart.slots[handle - 1];
    return sl->pid && sl->pid == current->pid ? sl : NULL;
}

static uint64_t slot_gpu(const struct slot *sl)
{
    return gart.start + (uint64_t)sl->first * 4096 + sl->offset;
}

static void unmap_slot(struct slot *sl)
{
    for (uint32_t i = 0; i < sl->npages; i++)
        gart_set(sl->first + i, 0);
    gart_flush();
    sl->pid = 0;
}

int radeon_map_buffer(struct dispbuf *b)
{
    if (!radeon_gart_ok())
        return -ENODEV;
    if (!b->size || b->size > (32u << 20) || b->addr + b->size < b->addr)
        return -EINVAL;
    uint32_t first_va = b->addr & ~0xFFFu;
    uint32_t npages = (b->addr + b->size - first_va + 4095) >> 12;
    int free_idx = -1;
    for (int i = 0; i < MAX_SLOTS && free_idx < 0; i++)
        if (!gart.slots[i].pid)
            free_idx = i;
    if (free_idx < 0)
        return -ENOMEM;
    /* the lowest run of GART pages no slot uses */
    uint32_t first = 0;
    for (int moved = 1; moved;) {
        moved = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            struct slot *o = &gart.slots[i];
            if (o->pid && o->first < first + npages && first < o->first + o->npages) {
                first = o->first + o->npages;
                moved = 1;
            }
        }
    }
    if (first + npages > GTT_PAGES)
        return -ENOMEM;
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t phys = vmm_translate(current->pd, first_va + i * 4096);
        if (!phys) {
            for (uint32_t j = 0; j < i; j++)
                gart_set(first + j, 0);
            return -EFAULT;
        }
        gart_set(first + i, phys);
    }
    gart_flush();
    struct slot *sl = &gart.slots[free_idx];
    sl->pid = current->pid;
    sl->first = first;
    sl->npages = npages;
    sl->offset = b->addr & 0xFFF;
    sl->size = b->size;
    b->handle = (uint32_t)free_idx + 1;
    uint64_t gpu = slot_gpu(sl);
    b->gpu_lo = (uint32_t)gpu;
    b->gpu_hi = (uint32_t)(gpu >> 32);
    return 0;
}

int radeon_unmap_buffer(const struct dispbuf *b)
{
    struct slot *sl = find_slot(b->handle);
    if (!sl)
        return -EINVAL;
    unmap_slot(sl);
    return 0;
}

/* ---- the 3D client ---- */

int radeon_map3d(struct disp3d *d)
{
    if (!radeon_3d_ok())
        return -ENODEV;
    if (gl3d.owner && gl3d.owner != current->pid) {
        struct proc *p = proc_find(gl3d.owner);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    uint32_t size = MIN(GL_VRAM_MAX, MIN(rv.aper_size, rv.vram_size) - GL_VRAM_OFF);
    int r = vmm_map_phys(current->pd, USER_GL_BASE, rv.aper_phys + GL_VRAM_OFF, size, 0);
    if (r < 0)
        return r;
    if (gl3d.owner != current->pid)
        klog("radeon: pid %d (%s) has the 3D engine\n", current->pid, current->name);
    gl3d.owner = current->pid;
    gl3d.size = size;
    uint64_t gpu = rv.mc_base + GL_VRAM_OFF;
    memset(d, 0, sizeof(*d));
    d->vram = USER_GL_BASE;
    d->vram_size = size;
    d->pci_id = VENDOR_ATI | (uint32_t)rv.device << 16;
    d->gpu_lo = (uint32_t)gpu;
    d->gpu_hi = (uint32_t)(gpu >> 32);
    return 0;
}

/* Does [addr, addr + bytes) lie in the caller's video memory or buffers? */
static int caller_owns(uint64_t addr, uint64_t bytes)
{
    uint64_t gl = rv.mc_base + GL_VRAM_OFF;
    if (gl3d.owner == current->pid && addr >= gl && addr + bytes <= gl + gl3d.size)
        return 1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        struct slot *sl = &gart.slots[i];
        if (sl->pid == current->pid && addr >= slot_gpu(sl) && addr + bytes <= slot_gpu(sl) + sl->size)
            return 1;
    }
    return 0;
}

int radeon_submit(const struct dispsubmit *sub)
{
    if (!rv.accel)
        return -ENODEV;
    if (gl3d.owner != current->pid)
        return -EPERM;
    uint64_t addr = (uint64_t)sub->gpu_hi << 32 | sub->gpu_lo;
    if (!sub->dwords || sub->dwords > (1u << 20) || (addr & 3) || !caller_owns(addr, sub->dwords * 4ull))
        return -EINVAL;
    if (cp.busy)
        return -EBUSY;
    cp.busy = 1;
    int r = begin_state();
    if (r == 0 && (r = ring_space(4)) == 0) {
        emit(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
        emit((uint32_t)addr);
        emit((uint32_t)(addr >> 32) & 0xFF);
        emit(sub->dwords);
        r = finish();
    }
    cp.busy = 0;
    return r < 0 ? -EIO : 0;
}

void radeon_accel_proc_exit(int pid)
{
    if (!rv.present)
        return;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (gart.slots[i].pid == pid)
            unmap_slot(&gart.slots[i]);
    if (gl3d.owner == pid)
        gl3d.owner = 0;
}

/* ---- DISP_BLIT ---- */

static int resolve(const struct dispsurf *d, int screen_owner, struct surf *s)
{
    uint64_t base, limit;
    switch (d->kind) {
    case DISP_SURF_PAGE:
        if (!screen_owner)
            return -EPERM;
        if (d->handle > 1 || (d->handle && !rv.flip) || !screen_ok())
            return -EINVAL;
        s->addr = rv.mc_base + rv.page0 + d->handle * rv.page_size;
        s->pitch = fb.pitch / fb.bytespp;
        s->w = fb.width;
        s->h = fb.height;
        s->format = fb.bpp == 16 ? FMT_5_6_5 : FMT_8_8_8_8;
        s->cpp = fb.bytespp;
        return 0;
    case DISP_SURF_ARENA:
        if (gl3d.owner != current->pid)
            return -EPERM;
        base = rv.mc_base + GL_VRAM_OFF;
        limit = gl3d.size;
        break;
    case DISP_SURF_BUFFER: {
        struct slot *sl = find_slot(d->handle);
        if (!sl)
            return -EINVAL;
        base = slot_gpu(sl);
        limit = sl->size;
        break;
    }
    default:
        return -EINVAL;
    }
    if (!d->width || !d->height || d->width > 8192 || d->height > 8192 || d->pitch % 256 ||
        d->width * 4 > d->pitch || (uint64_t)d->offset + (uint64_t)d->pitch * d->height > limit ||
        ((base + d->offset) & 255))
        return -EINVAL;
    s->addr = base + d->offset;
    s->pitch = d->pitch / 4;
    s->w = d->width;
    s->h = d->height;
    s->format = FMT_8_8_8_8;
    s->cpp = 4;
    return 0;
}

int radeon_accel_blit(const struct dispblit *b, int screen_owner)
{
    if (!rv.accel)
        return -ENODEV;
    if (b->count > DISP_MAX_RECTS)
        return -EINVAL;
    struct surf src, dst;
    int r = resolve(&b->src, screen_owner, &src);
    if (r == 0)
        r = resolve(&b->dst, screen_owner, &dst);
    if (r < 0)
        return r;
    if (cp.busy)
        return -EBUSY;
    cp.busy = 1;
    r = begin_blits();
    for (uint32_t i = 0; i < b->count && r == 0; i++) {
        const struct dispblitrect *q = &b->rects[i];
        int sx = q->sx, sy = q->sy, dx = q->dx, dy = q->dy, w = q->w, h = q->h;
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }
        if (dx < 0) { w += dx; sx -= dx; dx = 0; }
        if (dy < 0) { h += dy; sy -= dy; dy = 0; }
        w = MIN(w, MIN((int)src.w - sx, (int)dst.w - dx));
        h = MIN(h, MIN((int)src.h - sy, (int)dst.h - dy));
        if (w <= 0 || h <= 0)
            continue;
        r = blit_or_flush(&src, &dst, (uint32_t)sx, (uint32_t)sy, (uint32_t)dx, (uint32_t)dy,
                          (uint32_t)w, (uint32_t)h);
    }
    if (r == 0)
        r = finish();
    cp.busy = 0;
    return r < 0 ? -EIO : 0;
}

int radeon_accel_copy(const struct dispcopy *c)
{
    if (!radeon_accel_ok())
        return -ENODEV;
    if (c->from_page > 1 || c->count > DISP_MAX_RECTS)
        return -EINVAL;
    if (cp.busy)
        return -EBUSY;
    cp.busy = 1;
    uint32_t pitch = fb.pitch / fb.bytespp, fmt = fb.bpp == 16 ? FMT_5_6_5 : FMT_8_8_8_8;
    uint64_t base = rv.mc_base + rv.page0;
    struct surf src = { base + c->from_page * rv.page_size, pitch, fb.width, fb.height, fmt, fb.bytespp };
    struct surf dst = src;
    dst.addr = base + (c->from_page ^ 1) * rv.page_size;
    int r = begin_blits();
    for (uint32_t i = 0; i < c->count && r == 0; i++) {
        int x = c->rects[i].x, y = c->rects[i].y, w = c->rects[i].w, h = c->rects[i].h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        w = MIN(w, (int)fb.width - x);
        h = MIN(h, (int)fb.height - y);
        if (w <= 0 || h <= 0)
            continue;
        r = blit_or_flush(&src, &dst, (uint32_t)x, (uint32_t)y, (uint32_t)x, (uint32_t)y,
                          (uint32_t)w, (uint32_t)h);
    }
    if (r == 0)
        r = finish();
    cp.busy = 0;
    return r < 0 ? -EIO : 0;
}

/* radeon.accel=vm, =gpu or =cp stops the bring-up after that step, to
 * find a step that upsets the card; radeon.accel=0 skips it all. */
static int stop_after(const char *step)
{
    char opt[8];
    if (!cmdline_value("radeon.accel", opt, sizeof(opt)) || strcmp(opt, step) != 0)
        return 0;
    kprintf("radeon: 3D engine bring-up stopped after \"%s\" (radeon.accel=%s)\n", step, opt);
    return 1;
}

void radeon_accel_init(void)
{
    if (cmdline_has("radeon.accel=0") || !rv.vram || !rv.crtcs)
        return;
    cfg = &configs[rv.family];
    cp.cpu = rv.vram + (ACCEL_VRAM_OFF - rv.page0);
    cp.gpu = rv.mc_base + ACCEL_VRAM_OFF;
    cp.ring = (uint32_t *)(cp.cpu + RING_OFF);
    memset(cp.cpu, 0, TEST_OFF);

    /* the default state, padded to 16 dwords, then the shaders */
    uint32_t *state = (uint32_t *)(cp.cpu + STATE_OFF);
    uint32_t n = ARRAY_SIZE(r7xx_default_state);
    memcpy(state, r7xx_default_state, sizeof(r7xx_default_state));
    while (n & 15)
        state[n++] = PACKET2;
    cp.state_dwords = n;
    cp.vs_off = STATE_OFF + ALIGN_UP(n * 4, 256u);
    cp.ps_off = cp.vs_off + ALIGN_UP((uint32_t)sizeof(blit_vs), 256u);
    memcpy(cp.cpu + cp.vs_off, blit_vs, sizeof(blit_vs));
    memcpy(cp.cpu + cp.ps_off, blit_ps, sizeof(blit_ps));

    klog("radeon: MC_VM_FB_LOCATION %08x, aperture at %08x%08x as the engine sees it\n",
         rreg(MC_VM_FB_LOCATION), (uint32_t)(rv.mc_base >> 32), (uint32_t)rv.mc_base);

    if (vm_init() < 0) {
        kprintf("radeon: video memory is not where expected, no acceleration\n");
        return;
    }
    int want_gart = !cmdline_has("radeon.gart=0");
    if (want_gart && gart_init() < 0)
        want_gart = 0;
    if (stop_after("vm"))
        return;
    gpu_init();
    klog("radeon: engine configured: GB_TILING_CONFIG %08x, CC_RB_BACKEND_DISABLE %08x, "
         "CC_GC_SHADER_PIPE_CONFIG %08x\n", rreg(GB_TILING_CONFIG), rreg(CC_RB_BACKEND_DISABLE),
         rreg(CC_GC_SHADER_PIPE_CONFIG));
    if (stop_after("gpu"))
        return;
    load_microcode();
    rv.accel = 1;
    if (cp_start() < 0) {
        cp_halt();
        rv.accel = 0;
        kprintf("radeon: the command processor did not start, no acceleration\n");
        return;
    }
    if (stop_after("cp")) {
        cp_halt();
        rv.accel = 0;
        return;
    }
    if (self_test() < 0) {
        cp_halt();
        rv.accel = 0;
        kprintf("radeon: the 3D engine failed its self-test, no acceleration\n");
        return;
    }
    if (want_gart) {
        gart.on = gart_test() == 0;
        if (!gart.on) {
            wreg(VM_CONTEXT0_CNTL, 0);
            kprintf("radeon: the GART failed its self-test, the GPU keeps to video memory\n");
        }
    }
    boot_ok("%s: 3D engine running (command processor, fill and copy tested%s)", rv.name,
            gart.on ? ", GART" : "");
}
