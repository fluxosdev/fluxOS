/*
 * The Llano APUs' 3D engine (Sumo, the Evergreen generation): what
 * radeon_accel.c needs to know of it, as Linux's radeon driver brings it
 * up (evergreen.c) and blits with it (evergreen_blit_kms.c, before Linux
 * 3.12): AMD's recommended register settings and the engine's set-up,
 * the APUs' TLB registers, the Sumo microcode, a clear state the CP
 * keeps and puts back at the start of each batch, an HDP flush that is a
 * register write, and the blit's state, shaders and surfaces, whose
 * registers moved from the R700's. No 3D clients yet: the GL library's
 * shaders and the command buffer check are the R700's. The engine is
 * set up again after suspend to RAM.
 */
#include "kernel.h"
#include "drivers.h"
#include "radeon.h"
#include "radeon_accel.h"
#include "radeon_llano_state.h"

/* Registers */
#define HDP_HOST_PATH_CNTL 0x2C00
#define CGTS_SYS_TCC_DISABLE 0x3F90
#define CGTS_USER_SYS_TCC_DISABLE 0x3F94
#define GRBM_CNTL       0x8000
#define GRBM_READ_TIMEOUT(x) ((x) << 0)
#define CP_QUEUE_THRESHOLDS 0x8760
#define ROQ_IB1_START(x) ((x) << 0)
#define ROQ_IB2_START(x) ((x) << 8)
#define CP_MEQ_THRESHOLDS 0x8764
#define STQ_SPLIT(x)    ((x) << 0)
#define CP_PERFMON_CNTL 0x87FC
#define VGT_CACHE_INVALIDATION 0x88C4
#define CACHE_INVALIDATION(x) ((x) << 0)
#define TC_ONLY         1
#define AUTO_INVLD_EN(x) ((x) << 6)
#define ES_AND_GS_AUTO  3
#define VGT_GS_VERTEX_REUSE 0x88D4
#define CC_GC_SHADER_PIPE_CONFIG 0x8950
#define VGT_NUM_INSTANCES 0x8974
#define PA_CL_ENHANCE   0x8A14
#define CLIP_VTX_REORDER_ENA (1u << 0)
#define NUM_CLIP_SEQ(x) ((x) << 1)
#define PA_SC_LINE_STIPPLE_STATE 0x8B10
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
#define NUM_PS_THREADS(x) ((uint32_t)(x) << 0)
#define NUM_VS_THREADS(x) ((uint32_t)(x) << 8)
#define NUM_GS_THREADS(x) ((uint32_t)(x) << 16)
#define NUM_ES_THREADS(x) ((uint32_t)(x) << 24)
#define NUM_PS_STACK_ENTRIES(x) ((uint32_t)(x) << 0)
#define NUM_VS_STACK_ENTRIES(x) ((uint32_t)(x) << 16)
#define NUM_GS_STACK_ENTRIES(x) ((uint32_t)(x) << 0)
#define NUM_ES_STACK_ENTRIES(x) ((uint32_t)(x) << 16)
#define SQ_MS_FIFO_SIZES 0x8CF0
#define CACHE_FIFO_SIZE(x) ((x) << 0)
#define FETCH_FIFO_HIWATER(x) ((x) << 8)
#define DONE_FIFO_HIWATER(x) ((x) << 16)
#define ALU_UPDATE_FIFO_HIWATER(x) ((uint32_t)(x) << 24)
#define SX_EXPORT_BUFFER_SIZES 0x900C
#define COLOR_BUFFER_SIZE(x) ((x) << 0)
#define POSITION_BUFFER_SIZE(x) ((x) << 8)
#define SMX_BUFFER_SIZE(x) ((x) << 16)
#define SX_DEBUG_1      0x9058
#define ENABLE_NEW_SMX_ADDRESS (1u << 16)
#define SPI_CONFIG_CNTL 0x9100
#define SPI_CONFIG_CNTL_1 0x913C
#define VTX_DONE_DELAY(x) ((x) << 0)
#define CGTS_TCC_DISABLE 0x9148
#define CGTS_USER_TCC_DISABLE 0x914C
#define TA_CNTL_AUX     0x9508
#define DISABLE_CUBE_ANISO (1u << 1)
#define CC_RB_BACKEND_DISABLE 0x98F4
#define SMX_SAR_CTL0    0xA008
#define SMX_DC_CTL0     0xA020
#define CACHE_DEPTH(x)  ((x) << 1)
#define CB_FORMAT(x)    ((x) << 2)
#define CB_ARRAY_MODE(x) ((x) << 8)
#define CB_SF_EXPORT_NORM 1
#define ARRAY_LINEAR_ALIGNED 1
#define SQ_PGM_START_PS 0x28840
#define SQ_PGM_RESOURCES_PS 0x28850
#define SQ_PGM_EXPORTS_PS 0x28854
#define SQ_PGM_RESOURCES_VS 0x28868
#define VGT_VERTEX_REUSE_BLOCK_CNTL 0x28C58
#define VGT_OUT_DEALLOC_CNTL 0x28C5C
#define PACKET3_CONTEXT_CONTROL 0x28
#define PACKET3_SET_RESOURCE 0x6D
#define SQ_TEX_VTX_VALID_TEXTURE 2u
#define SQ_TEX_VTX_VALID_BUFFER 3u
#define PFP_UCODE_DWORDS 1120
#define ME_UCODE_DWORDS  1376
#define SRBM_INT_CNTL   0x0EA0
#define SRBM_INT_ACK    0x0EA8
#define DMIF_ADDR_CONFIG 0x0BD4
#define FUS_MC_VM_MD_L1_TLB0_CNTL 0x265C    /* the APUs' instead of MC_VM_MD_L1_TLB* */
#define FUS_MC_VM_MD_L1_TLB1_CNTL 0x2660
#define FUS_MC_VM_MD_L1_TLB2_CNTL 0x2664
#define HDP_ADDR_CONFIG 0x2F48
#define HDP_MISC_CNTL   0x2F4C
#define HDP_FLUSH_INVALIDATE_CACHE (1u << 0)
#define RLC_CNTL        0x3F00
#define RLC_GFX_INDEX   0x3FC4
#define HDP_MEM_COHERENCY_FLUSH_CNTL 0x5480
#define GRBM_GFX_INDEX  0x802C
#define INSTANCE_BROADCAST_WRITES (1u << 30)
#define SE_BROADCAST_WRITES (1u << 31)
#define SE_INDEX(x)     ((uint32_t)(x) << 16)
#define SOFT_RESET_PA   (1u << 5)
#define SOFT_RESET_SPI  (1u << 8)
#define SOFT_RESET_SH   (1u << 9)
#define SOFT_RESET_SX   (1u << 10)
#define SOFT_RESET_VGT  (1u << 14)
#define CP_SEM_INCOMPLETE_TIMER_CNTL 0x85C8
#define PA_SU_LINE_STIPPLE_VALUE 0x8A60
#define SQ_GPR_RESOURCE_MGMT_3 0x8C0C
#define NUM_HS_GPRS(x)  ((uint32_t)(x) << 0)
#define NUM_LS_GPRS(x)  ((uint32_t)(x) << 16)
#define SQ_THREAD_RESOURCE_MGMT 0x8C18
#define SQ_THREAD_RESOURCE_MGMT_2 0x8C1C
#define NUM_HS_THREADS(x) ((uint32_t)(x) << 0)
#define NUM_LS_THREADS(x) ((uint32_t)(x) << 8)
#define SQ_STACK_RESOURCE_MGMT_1 0x8C20
#define SQ_STACK_RESOURCE_MGMT_2 0x8C24
#define SQ_STACK_RESOURCE_MGMT_3 0x8C28
#define NUM_HS_STACK_ENTRIES(x) ((uint32_t)(x) << 0)
#define NUM_LS_STACK_ENTRIES(x) ((uint32_t)(x) << 16)
#define SQ_DYN_GPR_CNTL_PS_FLUSH_REQ 0x8D8C
#define SQ_LDS_RESOURCE_MGMT 0x8E2C
#define SYNC_GRADIENT   (1u << 24)
#define SYNC_WALKER     (1u << 25)
#define SYNC_ALIGNER    (1u << 26)
#define GB_ADDR_CONFIG  0x98F8
#define GB_BACKEND_MAP  0x98FC
#define SUMO_GB_ADDR_CONFIG_GOLDEN 0x02010002   /* the SUMO2's too */
#define CB_PERF_CTR0_SEL_0 0x9A20               /* eight of them */
#define DMA_TILING_CONFIG 0xD0B8
#define UVD_UDEC_ADDR_CONFIG 0xEF4C
#define UVD_UDEC_DB_ADDR_CONFIG 0xEF50
#define UVD_UDEC_DBW_ADDR_CONFIG 0xEF54
#define SQ_ALU_CONST_BUFFER_SIZE_PS_0 0x28140
#define SQ_ALU_CONST_BUFFER_SIZE_HS_0 0x28F80
#define SQ_PGM_START_VS 0x2885C             /* then RESOURCES, RESOURCES_2 */
#define CB_COLOR0_BASE 0x28C60               /* 15 registers a buffer to 7 */
#define CB_COLOR8_BASE 0x28E40               /* 7 registers a buffer to 11 */
#define CB_SOURCE_FORMAT(x) ((x) << 24)
#define PACKET3_CLEAR_STATE 0x12
#define PACKET3_MODE_CONTROL 0x18
#define PACKET3_PREAMBLE_CNTL 0x4A
#define PREAMBLE_BEGIN_CLEAR_STATE (2u << 28)
#define PREAMBLE_END_CLEAR_STATE (3u << 28)
#define VTX_RESOURCE_0 0x580                 /* the vertex shader's first buffer */

extern const uint8_t radeon_fw_sumo_pfp[], radeon_fw_sumo_me[], radeon_fw_sumo2_pfp[];

/* Per-chip limits (evergreen_gpu_init) and the blit's shader resources
 * (evergreen_blit_kms.c set_default_state). */
struct chip_config {
    uint32_t max_backends, max_gprs, max_threads, max_stack_entries, max_hw_contexts;
    uint32_t sx_max_export_size, sx_max_export_pos_size, sx_max_export_smx_size;
    uint32_t sq_num_cf_insts, sx_num_of_sets, sc_prim_fifo_size, sc_hiz_tile_fifo_size;
    uint32_t sc_earlyz_tile_fifo_size;
    uint32_t blit_ps_gprs, blit_vs_gprs, blit_ps_threads, blit_vs_threads, blit_stack;
};

static const struct chip_config configs[] = {
    [CHIP_SUMO]  = { 2, 256, 248, 256, 8, 256, 64, 192, 2, 4, 0x40, 0x30, 0x130, 93, 46, 96, 25, 42 },
    [CHIP_SUMO2] = { 1, 256, 248, 512, 4, 256, 64, 192, 2, 4, 0x40, 0x30, 0x130, 93, 46, 96, 25, 85 },
};

static const struct chip_config *cfg;

static void setup(void)
{
    cfg = &configs[rv.family];
}

static uint32_t hw_contexts(void)
{
    return cfg->max_hw_contexts;
}

/* The register settings AMD recommends for the Llano graphics, as
 * (register, mask, value), from evergreen.c: supersumo_golden_registers
 * for both, then sumo_golden_registers for the SUMO2. */
static const uint32_t supersumo_golden[] = {
    0x5eb4, 0xffffffff, 0x00000002, 0x5c4, 0xffffffff, 0x00000001,
    0x7030, 0xffffffff, 0x00000011, 0x7c30, 0xffffffff, 0x00000011,
    0x6104, 0x01000300, 0x00000000, 0x5bc0, 0x00300000, 0x00000000,
    0x8c04, 0xffffffff, 0x40600060, 0x8c08, 0xffffffff, 0x001c001c,
    0x8c20, 0xffffffff, 0x00800080, 0x8c24, 0xffffffff, 0x00800080,
    0x8c18, 0xffffffff, 0x20202078, 0x8c1c, 0xffffffff, 0x00001010,
    0x918c, 0xffffffff, 0x00010006, 0x91a8, 0xffffffff, 0x00010006,
    0x91c4, 0xffffffff, 0x00010006, 0x91e0, 0xffffffff, 0x00010006,
    0x9200, 0xffffffff, 0x00010006, 0x9150, 0xffffffff, 0x6e944040,
    0x917c, 0xffffffff, 0x00030002, 0x9180, 0xffffffff, 0x00050004,
    0x9198, 0xffffffff, 0x00030002, 0x919c, 0xffffffff, 0x00050004,
    0x91b4, 0xffffffff, 0x00030002, 0x91b8, 0xffffffff, 0x00050004,
    0x91d0, 0xffffffff, 0x00030002, 0x91d4, 0xffffffff, 0x00050004,
    0x91f0, 0xffffffff, 0x00030002, 0x91f4, 0xffffffff, 0x00050004,
    0x915c, 0xffffffff, 0x00010000, 0x9160, 0xffffffff, 0x00030002,
    0x3f90, 0xffff0000, 0xff000000, 0x9178, 0xffffffff, 0x00070000,
    0x9194, 0xffffffff, 0x00070000, 0x91b0, 0xffffffff, 0x00070000,
    0x91cc, 0xffffffff, 0x00070000, 0x91ec, 0xffffffff, 0x00070000,
    0x9148, 0xffff0000, 0xff000000, 0x9190, 0xffffffff, 0x00090008,
    0x91ac, 0xffffffff, 0x00090008, 0x91c8, 0xffffffff, 0x00090008,
    0x91e4, 0xffffffff, 0x00090008, 0x9204, 0xffffffff, 0x00090008,
    0x3f94, 0xffff0000, 0xff000000, 0x914c, 0xffff0000, 0xff000000,
    0x929c, 0xffffffff, 0x00000001, 0x8a18, 0xffffffff, 0x00000100,
    0x8b28, 0xffffffff, 0x00000100, 0x9144, 0xffffffff, 0x00000100,
    0x5644, 0xffffffff, 0x00000100, 0x9b7c, 0xffffffff, 0x00000000,
    0x8030, 0xffffffff, 0x0000100a, 0x8a14, 0xffffffff, 0x00000007,
    0x8b24, 0xffffffff, 0x00ff0fff, 0x8b10, 0xffffffff, 0x00000000,
    0x28a4c, 0x06000000, 0x06000000, 0x4d8, 0xffffffff, 0x00000100,
    0x913c, 0xffff000f, 0x0100000a, 0x960c, 0xffffffff, 0x54763210,
    0x88c4, 0xffffffff, 0x000000c2, 0x88d4, 0xffffffff, 0x00000010,
    0x8974, 0xffffffff, 0x00000000, 0xc78, 0x00000080, 0x00000080,
    0x5e78, 0xffffffff, 0x001000f0, 0xd02c, 0xffffffff, 0x08421000,
    0xa008, 0xffffffff, 0x00010000, 0x8d00, 0xffffffff, 0x100e4848,
    0x8d04, 0xffffffff, 0x00164745, 0x8c00, 0xffffffff, 0xe4000003,
    0x8cf0, 0x1fffffff, 0x08e00620, 0x28350, 0xffffffff, 0x00000000,
    0x9508, 0xffffffff, 0x00000002,
};

static const uint32_t sumo_golden[] = {
    0x900c, 0x00ffffff, 0x0017071f, 0x8c18, 0xffffffff, 0x10101060,
    0x8c1c, 0xffffffff, 0x00001010, 0x8c30, 0x0000000f, 0x00000005,
    0x9688, 0x0000000f, 0x00000007,
};

/* The APUs' MD client TLBs are elsewhere (evergreen_pcie_gart_enable). */
static void md_tlbs(uint32_t tlb)
{
    wreg(FUS_MC_VM_MD_L1_TLB0_CNTL, tlb);
    wreg(FUS_MC_VM_MD_L1_TLB1_CNTL, tlb);
    wreg(FUS_MC_VM_MD_L1_TLB2_CNTL, tlb);
}

/* AMD's recommended settings: (register, mask, value) triples. */
static void program_golden(const uint32_t *t, uint32_t n)
{
    for (uint32_t i = 0; i + 3 <= n; i += 3) {
        uint32_t v = t[i + 2];
        if (t[i + 1] != 0xFFFFFFFF)
            v |= rreg(t[i]) & ~t[i + 1];
        wreg(t[i], v);
    }
}

/* The Evergreen engine's set-up, for the Llano APUs (evergreen_gpu_init
 * and evergreen_init_golden_registers). */
static void gpu_init(void)
{
    klog("radeon: before set-up: GB_ADDR_CONFIG %08x, GB_BACKEND_MAP %08x, GRBM_STATUS %08x, "
         "RLC_CNTL %08x, CP_ME_CNTL %08x\n", rreg(GB_ADDR_CONFIG), rreg(GB_BACKEND_MAP),
         rreg(GRBM_STATUS), rreg(RLC_CNTL), rreg(CP_ME_CNTL));
    program_golden(supersumo_golden, ARRAY_SIZE(supersumo_golden));
    if (rv.family == CHIP_SUMO2)
        program_golden(sumo_golden, ARRAY_SIZE(sumo_golden));

    /* the HDP's surface registers */
    for (uint32_t i = 0, j = 0; i < 32; i++, j += 0x18)
        for (uint32_t k = 0; k < 5; k++)
            wreg(0x2C14 + j + 4 * k, 0);

    wreg(GRBM_CNTL, GRBM_READ_TIMEOUT(0xFF));
    wreg(SRBM_INT_CNTL, 1);
    wreg(SRBM_INT_ACK, 1);

    /* one shader engine: its disabled render backends */
    wreg(GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_INDEX(0));
    wreg(RLC_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_INDEX(0));
    uint32_t disabled_rb_mask = (rreg(CC_RB_BACKEND_DISABLE) >> 16) & 0xFF;
    wreg(GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_BROADCAST_WRITES);
    wreg(RLC_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_BROADCAST_WRITES);
    uint32_t all = (1u << cfg->max_backends) - 1;
    if ((disabled_rb_mask & all) == all)
        disabled_rb_mask &= ~all;

    uint32_t gb_addr_config = SUMO_GB_ADDR_CONFIG_GOLDEN;
    wreg(GB_ADDR_CONFIG, gb_addr_config);
    wreg(DMIF_ADDR_CONFIG, gb_addr_config);
    wreg(HDP_ADDR_CONFIG, gb_addr_config);
    wreg(DMA_TILING_CONFIG, gb_addr_config);
    wreg(UVD_UDEC_ADDR_CONFIG, gb_addr_config);
    wreg(UVD_UDEC_DB_ADDR_CONFIG, gb_addr_config);
    wreg(UVD_UDEC_DBW_ADDR_CONFIG, gb_addr_config);
    uint32_t backend_map;
    if (cfg->max_backends == 1)     /* the APU with one: which of the two it is */
        backend_map = (disabled_rb_mask & 3) == 1 ? 0x11111111 : 0;
    else
        backend_map = radeon_remap_render_backend(gb_addr_config & 0xF, cfg->max_backends, 8,
                                           disabled_rb_mask);
    wreg(GB_BACKEND_MAP, backend_map);

    wreg(CGTS_SYS_TCC_DISABLE, 0);
    wreg(CGTS_TCC_DISABLE, 0);
    wreg(CGTS_USER_SYS_TCC_DISABLE, 0);
    wreg(CGTS_USER_TCC_DISABLE, 0);

    wreg(CP_QUEUE_THRESHOLDS, ROQ_IB1_START(0x16) | ROQ_IB2_START(0x2B));
    wreg(CP_MEQ_THRESHOLDS, STQ_SPLIT(0x30));
    wreg(TA_CNTL_AUX, DISABLE_CUBE_ANISO | SYNC_GRADIENT | SYNC_WALKER | SYNC_ALIGNER);
    wreg(SX_DEBUG_1, rreg(SX_DEBUG_1) | ENABLE_NEW_SMX_ADDRESS);
    uint32_t smx_dc_ctl0 = rreg(SMX_DC_CTL0) & ~CACHE_DEPTH(0x1FFu);   /* NUMBER_OF_SETS */
    wreg(SMX_DC_CTL0, smx_dc_ctl0 | CACHE_DEPTH(cfg->sx_num_of_sets));
    wreg(SMX_SAR_CTL0, 0x00010000);
    wreg(SX_EXPORT_BUFFER_SIZES, COLOR_BUFFER_SIZE(cfg->sx_max_export_size / 4 - 1) |
                                 POSITION_BUFFER_SIZE(cfg->sx_max_export_pos_size / 4 - 1) |
                                 SMX_BUFFER_SIZE(cfg->sx_max_export_smx_size / 4 - 1));
    wreg(PA_SC_FIFO_SIZE, SC_PRIM_FIFO_SIZE(cfg->sc_prim_fifo_size) |
                          SC_HIZ_TILE_FIFO_SIZE(cfg->sc_hiz_tile_fifo_size) |
                          SC_EARLYZ_TILE_FIFO_SIZE(cfg->sc_earlyz_tile_fifo_size));
    wreg(VGT_NUM_INSTANCES, 1);
    wreg(SPI_CONFIG_CNTL, 0);
    wreg(SPI_CONFIG_CNTL_1, VTX_DONE_DELAY(4));
    wreg(CP_PERFMON_CNTL, 0);
    wreg(SQ_MS_FIFO_SIZES, CACHE_FIFO_SIZE(16 * cfg->sq_num_cf_insts) | FETCH_FIFO_HIWATER(0x4) |
                           DONE_FIFO_HIWATER(0xE0) | ALU_UPDATE_FIFO_HIWATER(0x8));

    /* no vertex cache on the APUs */
    uint32_t sq_config = rreg(SQ_CONFIG) & ~(PS_PRIO(3) | VS_PRIO(3) | GS_PRIO(3) | ES_PRIO(3) | VC_ENABLE);
    sq_config |= EXPORT_SRC_C | PS_PRIO(0) | VS_PRIO(1) | GS_PRIO(2) | ES_PRIO(3);
    uint32_t gprs = cfg->max_gprs - 4 * 2;
    uint32_t ps_threads = 96, threads = (cfg->max_threads - ps_threads) / 6 / 8 * 8;
    uint32_t stack = cfg->max_stack_entries / 6;
    uint32_t lds = rreg(SQ_LDS_RESOURCE_MGMT);
    wreg(SQ_CONFIG, sq_config);
    wreg(SQ_GPR_RESOURCE_MGMT_1, NUM_PS_GPRS(gprs * 12 / 32) | NUM_VS_GPRS(gprs * 6 / 32) |
                                 NUM_CLAUSE_TEMP_GPRS(4));
    wreg(SQ_GPR_RESOURCE_MGMT_2, NUM_GS_GPRS(gprs * 4 / 32) | NUM_ES_GPRS(gprs * 4 / 32));
    wreg(SQ_GPR_RESOURCE_MGMT_3, NUM_HS_GPRS(gprs * 3 / 32) | NUM_LS_GPRS(gprs * 3 / 32));
    wreg(SQ_THREAD_RESOURCE_MGMT, NUM_PS_THREADS(ps_threads) | NUM_VS_THREADS(threads) |
                                     NUM_GS_THREADS(threads) | NUM_ES_THREADS(threads));
    wreg(SQ_THREAD_RESOURCE_MGMT_2, NUM_HS_THREADS(threads) | NUM_LS_THREADS(threads));
    wreg(SQ_STACK_RESOURCE_MGMT_1, NUM_PS_STACK_ENTRIES(stack) | NUM_VS_STACK_ENTRIES(stack));
    wreg(SQ_STACK_RESOURCE_MGMT_2, NUM_GS_STACK_ENTRIES(stack) | NUM_ES_STACK_ENTRIES(stack));
    wreg(SQ_STACK_RESOURCE_MGMT_3, NUM_HS_STACK_ENTRIES(stack) | NUM_LS_STACK_ENTRIES(stack));
    wreg(SQ_DYN_GPR_CNTL_PS_FLUSH_REQ, 0);
    wreg(SQ_LDS_RESOURCE_MGMT, lds);

    wreg(PA_SC_FORCE_EOV_MAX_CNTS, FORCE_EOV_MAX_CLK_CNT(4095) | FORCE_EOV_MAX_REZ_CNT(255));
    wreg(VGT_CACHE_INVALIDATION, CACHE_INVALIDATION(TC_ONLY) | AUTO_INVLD_EN(ES_AND_GS_AUTO));
    wreg(VGT_GS_VERTEX_REUSE, 16);
    wreg(PA_SU_LINE_STIPPLE_VALUE, 0);
    wreg(PA_SC_LINE_STIPPLE_STATE, 0);
    wreg(VGT_VERTEX_REUSE_BLOCK_CNTL, 14);
    wreg(VGT_OUT_DEALLOC_CNTL, 16);
    for (uint32_t i = 0; i < 8; i++)
        wreg(CB_PERF_CTR0_SEL_0 + 4 * i, 0);
    for (uint32_t i = 0; i < 8; i++)
        wreg(CB_COLOR0_BASE + 0x3C * i, 0);
    for (uint32_t i = 0; i < 4; i++)
        wreg(CB_COLOR8_BASE + 0x1C * i, 0);
    /* the shaders' constant cache sizes */
    for (uint32_t r = SQ_ALU_CONST_BUFFER_SIZE_PS_0; r < 0x28200; r += 4)
        wreg(r, 0);
    for (uint32_t r = SQ_ALU_CONST_BUFFER_SIZE_HS_0; r < 0x29000; r += 4)
        wreg(r, 0);
    wreg(HDP_MISC_CNTL, rreg(HDP_MISC_CNTL) | HDP_FLUSH_INVALIDATE_CACHE);
    wreg(HDP_HOST_PATH_CNTL, rreg(HDP_HOST_PATH_CNTL));
    wreg(PA_CL_ENHANCE, CLIP_VTX_REORDER_ENA | NUM_CLIP_SEQ(3));
    udelay(50);
    klog("radeon: engine configured: GB_ADDR_CONFIG %08x, GB_BACKEND_MAP %08x, "
         "CC_RB_BACKEND_DISABLE %08x, CC_GC_SHADER_PIPE_CONFIG %08x\n", rreg(GB_ADDR_CONFIG),
         rreg(GB_BACKEND_MAP), rreg(CC_RB_BACKEND_DISABLE), rreg(CC_GC_SHADER_PIPE_CONFIG));
}

/* AMD's microcode: SUMO2_me is the same file as SUMO_me. */
static void microcode(const uint8_t **pfp, int *pfp_dwords, const uint8_t **me, int *me_dwords)
{
    *pfp = rv.family == CHIP_SUMO2 ? radeon_fw_sumo2_pfp : radeon_fw_sumo_pfp;
    *me = radeon_fw_sumo_me;
    *pfp_dwords = PFP_UCODE_DWORDS;
    *me_dwords = ME_UCODE_DWORDS;
}

/* The engine reads what the CPU wrote to its memory only after the HDP
 * is flushed, which on Evergreen is a register write. The flush also
 * empties the HDP's read cache (HDP_MISC_CNTL), so the CPU then sees
 * what the engine drew. */
static void hdp_flush(void)
{
    __asm__ volatile("sfence" ::: "memory");   /* the CPU's write-combined writes first */
    wreg(HDP_MEM_COHERENCY_FLUSH_CNTL, 1);
    (void)rreg(HDP_MEM_COHERENCY_FLUSH_CNTL);
}

/* Before the ring starts (evergreen_cp_resume). */
static void cp_regs(void)
{
    wreg(CP_SEM_INCOMPLETE_TIMER_CNTL, 0);
}

/* Evergreen's clear state, the default state, which CLEAR_STATE puts
 * back at the start of every batch (evergreen_cp_start). */
static int cp_preamble(void)
{
    if (radeon_ring_space(ARRAY_SIZE(evergreen_default_state) + 19) < 0)
        return -1;
    emit(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
    emit(PREAMBLE_BEGIN_CLEAR_STATE);
    for (uint32_t i = 0; i < ARRAY_SIZE(evergreen_default_state); i++)
        emit(evergreen_default_state[i]);
    emit(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
    emit(PREAMBLE_END_CLEAR_STATE);
    emit(PACKET3(PACKET3_CLEAR_STATE, 0));
    emit(0);
    emit(0xC0026F00);           /* SQ_VTX_BASE_VTX_LOC */
    emit(0);
    emit(0);
    emit(0);
    emit(0xC0036F00);           /* the constants */
    emit(0x00000BC4);
    emit(0xFFFFFFFF);
    emit(0xFFFFFFFF);
    emit(0xFFFFFFFF);
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
    emit(CONTEXT_REG(VGT_VERTEX_REUSE_BLOCK_CNTL));
    emit(0x0E);
    emit(0x10);                 /* VGT_OUT_DEALLOC_CNTL */
    return 0;
}

/* ---- blits (evergreen_blit_kms.c) ---- */

/* Evergreen's: the clear state, the shader resources, then the default
 * state as an indirect buffer (evergreen_blit_kms.c set_default_state). */
static int begin_state(void)
{
    if (radeon_ring_space(48) < 0)
        return -1;
    emit(PACKET3(PACKET3_CLEAR_STATE, 0));
    emit(0);
    set_config_reg(SQ_DYN_GPR_CNTL_PS_FLUSH_REQ, 0);    /* no dynamic GPRs */
    set_config_reg(SQ_LDS_RESOURCE_MGMT, 0x10001000);
    uint32_t threads = cfg->blit_vs_threads, stack = cfg->blit_stack;
    emit(PACKET3(PACKET3_SET_CONFIG_REG, 11));
    emit(CONFIG_REG(SQ_CONFIG));
    emit(EXPORT_SRC_C | PS_PRIO(0) | VS_PRIO(1) | GS_PRIO(2) | ES_PRIO(3));  /* no vertex cache */
    emit(NUM_PS_GPRS(cfg->blit_ps_gprs) | NUM_VS_GPRS(cfg->blit_vs_gprs) | NUM_CLAUSE_TEMP_GPRS(4));
    emit(NUM_GS_GPRS(31) | NUM_ES_GPRS(31));
    emit(NUM_HS_GPRS(23) | NUM_LS_GPRS(23));
    emit(0);
    emit(0);
    emit(NUM_PS_THREADS(cfg->blit_ps_threads) | NUM_VS_THREADS(threads) | NUM_GS_THREADS(threads) |
         NUM_ES_THREADS(threads));
    emit(NUM_HS_THREADS(threads) | NUM_LS_THREADS(threads));
    emit(NUM_PS_STACK_ENTRIES(stack) | NUM_VS_STACK_ENTRIES(stack));
    emit(NUM_GS_STACK_ENTRIES(stack) | NUM_ES_STACK_ENTRIES(stack));
    emit(NUM_HS_STACK_ENTRIES(stack) | NUM_LS_STACK_ENTRIES(stack));
    emit(PACKET3(PACKET3_CONTEXT_CONTROL, 1));
    emit(0x80000000);
    emit(0x80000000);
    emit(0xC0026F00);               /* SQ_VTX_BASE_VTX_LOC */
    emit(0);
    emit(0);
    emit(0);
    emit(0xC0036E00);               /* SET_SAMPLER 0: clamped, nearest */
    emit(0);
    emit(0x12);
    emit(0);
    emit(0);
    emit(PACKET3(PACKET3_MODE_CONTROL, 0));
    emit(1);                        /* DX10/11 */
    uint64_t state = rcp.gpu + STATE_OFF;
    emit(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
    emit((uint32_t)state & ~3u);
    emit((uint32_t)(state >> 32) & 0xFF);
    emit(rcp.state_dwords);
    return 0;
}

/* The two blit shaders. */
static void set_shaders(uint64_t vs, uint64_t ps)
{
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 3));
    emit(CONTEXT_REG(SQ_PGM_START_VS));
    emit((uint32_t)(vs >> 8));
    emit(2);                    /* SQ_PGM_RESOURCES_VS: 2 GPRs */
    emit(0);
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 4));
    emit(CONTEXT_REG(SQ_PGM_START_PS));
    emit((uint32_t)(ps >> 8));
    emit(1);                    /* SQ_PGM_RESOURCES_PS: 1 GPR */
    emit(0);
    emit(2);                    /* SQ_PGM_EXPORTS_PS */
}

static void set_render_target(const struct surf *s)
{
    uint32_t h = MAX(ALIGN_UP(s->h, 8u), 8u);
    /* BASE, PITCH, SLICE, VIEW, INFO, ATTRIB, DIM, then CMASK, FMASK
     * and the clear color, unused */
    emit(PACKET3(PACKET3_SET_CONTEXT_REG, 15));
    emit(CONTEXT_REG(CB_COLOR0_BASE));
    emit((uint32_t)(s->addr >> 8));
    emit(s->pitch / 8 - 1);
    emit(s->pitch * h / 64 - 1);
    emit(0);
    emit(CB_FORMAT(s->format) | CB_SOURCE_FORMAT(CB_SF_EXPORT_NORM) |
         CB_ARRAY_MODE(ARRAY_LINEAR_ALIGNED));
    emit(0);
    emit((s->pitch - 1) | (h - 1) << 16);
    for (int i = 0; i < 8; i++)
        emit(0);
}

static void set_texture(const struct surf *s)
{
    uint32_t word0 = 1u /* 2D */ | ((s->pitch / 8 - 1) & 0xFFF) << 6 | ((s->w - 1) & 0x3FFF) << 18;
    uint32_t word1 = ((MAX(s->h, 1u) - 1) & 0x3FFF) | (uint32_t)ARRAY_LINEAR_ALIGNED << 28;
    uint32_t word4 = 0u << 16 | 1u << 19 | 2u << 22 | 3u << 25;    /* X, Y, Z, W */
    surface_sync(PACKET3_TC_ACTION_ENA, s->pitch * s->h * s->cpp, s->addr);
    emit(PACKET3(PACKET3_SET_RESOURCE, 8));
    emit(0);
    emit(word0);
    emit(word1);
    emit((uint32_t)(s->addr >> 8));
    emit((uint32_t)(s->addr >> 8));
    emit(word4);
    emit(0);
    emit(0);
    emit((s->format & 0x3F) | SQ_TEX_VTX_VALID_TEXTURE << 30);
}

static void set_vertices(uint64_t addr)
{
    emit(PACKET3(PACKET3_SET_RESOURCE, 8));
    emit(VTX_RESOURCE_0);
    emit((uint32_t)addr);
    emit(48 - 1);
    emit(((uint32_t)(addr >> 32) & 0xFF) | 16u << 8);  /* stride 16 */
    emit(0u << 3 | 1u << 6 | 2u << 9 | 3u << 12);      /* X, Y, Z, W */
    emit(0);
    emit(0);
    emit(0);
    emit(SQ_TEX_VTX_VALID_BUFFER << 30);
    surface_sync(PACKET3_TC_ACTION_ENA, 48, addr);      /* no vertex cache */
}

const struct accel_gen radeon_llano_accel = {
    .setup = setup,
    .md_tlbs = md_tlbs,
    .md_tlb0 = FUS_MC_VM_MD_L1_TLB0_CNTL,
    .vm_contexts = 2,
    .gart_range_flush = 0,
    .gpu_init = gpu_init,
    .microcode = microcode,
    .soft_reset = SOFT_RESET_PA | SOFT_RESET_SH | SOFT_RESET_VGT | SOFT_RESET_SPI | SOFT_RESET_SX,
    .reset_with_microcode = 0,
    .hdp_flush = hdp_flush,
    .cp_regs = cp_regs,
    .hw_contexts = hw_contexts,
    .cp_preamble = cp_preamble,
    .begin_state = begin_state,
    .set_shaders = set_shaders,
    .set_render_target = set_render_target,
    .set_texture = set_texture,
    .set_vertices = set_vertices,
    .state = evergreen_default_state,
    .state_dwords = ARRAY_SIZE(evergreen_default_state),
    .vs = evergreen_vs,
    .vs_bytes = sizeof(evergreen_vs),
    .ps = evergreen_ps,
    .ps_bytes = sizeof(evergreen_ps),
    .check = NULL,                  /* the GL library's 3D is the R700's */
    .resumable = 1,
};
