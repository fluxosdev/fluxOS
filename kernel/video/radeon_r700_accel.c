/*
 * The Radeon HD 4000s' (R700) 3D engine: what radeon_accel.c needs to
 * know of it, as Linux's radeon driver brings it up (rv770.c, r600.c)
 * and blits with it (r600_blit_kms.c, before Linux 3.12): the engine's
 * set-up (rv770_gpu_init), AMD's microcode, the HDP flush, the blit's
 * state, shaders and surfaces, and the check of a 3D client's command
 * buffer, which the GL library (user/gl/radeon.c) writes for this
 * generation.
 */
#include "kernel.h"
#include "drivers.h"
#include "radeon.h"
#include "radeon_accel.h"
#include "radeon_blit_state.h"

/* Registers */
#define MC_VM_MD_L1_TLB0_CNTL 0x2654
#define MC_VM_MD_L1_TLB1_CNTL 0x2658
#define MC_VM_MD_L1_TLB2_CNTL 0x265C
#define MC_VM_MD_L1_TLB3_CNTL 0x2698
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
#define DMA_TILING_CONFIG2 0xD0B8
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
#define PA_SC_CLIPRECT_RULE 0x2820C
#define PA_SC_EDGERULE  0x28230
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
#define PACKET3_SET_RESOURCE 0x6D
#define SQ_TEX_VTX_VALID_TEXTURE 2u
#define SQ_TEX_VTX_VALID_BUFFER 3u

extern const uint8_t radeon_fw_rv770_pfp[], radeon_fw_rv770_me[], radeon_fw_rv730_me[];
extern const uint8_t radeon_fw_rv710_pfp[], radeon_fw_rv710_me[];

#define PFP_UCODE_DWORDS 848
#define ME_UCODE_DWORDS  1360

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

static void setup(void)
{
    cfg = &configs[rv.family];
}

static uint32_t hw_contexts(void)
{
    return cfg->max_hw_contexts;
}

/* The memory controller's MD client TLBs (rv770_agp_enable). */
static void md_tlbs(uint32_t tlb)
{
    wreg(MC_VM_MD_L1_TLB0_CNTL, tlb);
    wreg(MC_VM_MD_L1_TLB1_CNTL, tlb);
    wreg(MC_VM_MD_L1_TLB2_CNTL, tlb);
    if (rv.family == CHIP_RV740)
        wreg(MC_VM_MD_L1_TLB3_CNTL, tlb);
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
    uint32_t active_pipes = 8 - radeon_popcount(inactive_pipes & 0xFF);
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
    gb_tiling_config |= radeon_remap_render_backend((gb_tiling_config >> 1) & 7, cfg->max_backends, 8,
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

    uint32_t num_qd_pipes = 8 - radeon_popcount((shader_pipe_config & INACTIVE_QD_PIPES_MASK) >> 8);
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
    klog("radeon: engine configured: GB_TILING_CONFIG %08x, CC_RB_BACKEND_DISABLE %08x, "
         "CC_GC_SHADER_PIPE_CONFIG %08x\n", rreg(GB_TILING_CONFIG), rreg(CC_RB_BACKEND_DISABLE),
         rreg(CC_GC_SHADER_PIPE_CONFIG));
}

/* AMD's microcode: the RV740 runs the RV730's, whose PFP is the RV710's. */
static void microcode(const uint8_t **pfp, int *pfp_dwords, const uint8_t **me, int *me_dwords)
{
    *pfp = radeon_fw_rv770_pfp;
    *me = radeon_fw_rv770_me;
    if (rv.family == CHIP_RV730 || rv.family == CHIP_RV740) {
        *pfp = radeon_fw_rv710_pfp;
        *me = radeon_fw_rv730_me;
    } else if (rv.family == CHIP_RV710) {
        *pfp = radeon_fw_rv710_pfp;
        *me = radeon_fw_rv710_me;
    }
    *pfp_dwords = PFP_UCODE_DWORDS;
    *me_dwords = ME_UCODE_DWORDS;
}

/* The engine reads what the CPU wrote to video memory only after the HDP
 * is flushed: on the R700, by writing HDP_DEBUG1 and reading memory back
 * (a hardware bug: r600_mmio_hdp_flush). */
static void hdp_flush(void)
{
    wreg(HDP_DEBUG1, 0);
    (void)*(volatile uint32_t *)(rcp.cpu + SCRATCH_OFF);
}

/* ---- blits (r600_blit_kms.c) ---- */

static int begin_state(void)
{
    if (radeon_ring_space(16) < 0)
        return -1;
    uint64_t state = rcp.gpu + STATE_OFF;
    emit(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
    emit((uint32_t)state & ~3u);
    emit((uint32_t)(state >> 32) & 0xFF);
    emit(rcp.state_dwords);

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

/* The two blit shaders. */
static void set_shaders(uint64_t vs, uint64_t ps)
{
    set_context_reg(SQ_PGM_START_VS, (uint32_t)(vs >> 8));
    set_context_reg(SQ_PGM_RESOURCES_VS, 1);
    set_context_reg(SQ_PGM_CF_OFFSET_VS, 0);
    set_context_reg(SQ_PGM_START_PS, (uint32_t)(ps >> 8));
    set_context_reg(SQ_PGM_RESOURCES_PS, 1 | (1u << 28));
    set_context_reg(SQ_PGM_EXPORTS_PS, 2);
    set_context_reg(SQ_PGM_CF_OFFSET_PS, 0);
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

/* ---- checking a 3D client's command buffer ----
 *
 * A command buffer runs only as the kernel's copy, in the engine's own
 * video memory, which nothing a process draws can reach; the process's
 * buffer is read once, so changing it afterwards changes nothing. The
 * copy is checked packet by packet, as Linux's r600_cs.c does: only the
 * packets and registers the GL library writes are let through, every
 * address in them must be the caller's (caller_owns), and each draw is
 * checked against the state before it. The color and depth buffers and
 * the shaders must have been set by this buffer, which cannot know what
 * the kernel or another process left in them; only color buffer 0 may
 * be drawn to. What is drawn then stays in the caller's memory.
 */

/* Registers and fields the check looks at */
#define DB_DEPTH_SIZE   0x28000
#define DB_DEPTH_VIEW   0x28004
#define DB_DEPTH_BASE   0x2800C
#define DB_DEPTH_INFO   0x28010
#define DB_FORMAT(v)    ((v) & 7)
#define DB_ARRAY_MODE(v) ((v) >> 15 & 0xF)
#define DB_TILE_SURFACE_ENABLE (1u << 25)   /* HTILE, at an address of its own */
#define CB_COLOR0_FORMAT(v) ((v) >> 2 & 0x3F)
#define CB_COLOR0_ARRAY_MODE(v) ((v) >> 8 & 0xF)
#define CB_COLOR0_TILE_MODE(v) ((v) >> 18 & 3)  /* CMASK and FMASK */
#define ARRAY_1D_TILED_THIN1 2
#define CB_TARGET_MASK  0x28238
#define CB_SHADER_MASK  0x2823C
#define VGT_MAX_VTX_INDX 0x28400
#define SPI_VS_OUT_ID_0 0x28614
#define SPI_VS_OUT_ID_9 0x28638
#define SPI_PS_INPUT_CNTL_0 0x28644
#define SPI_PS_INPUT_CNTL_31 0x286C0
#define SPI_VS_OUT_CONFIG 0x286C4
#define DB_DEPTH_CONTROL 0x28800
#define PA_CL_VTE_CNTL  0x28818
#define PACKET3_SET_SAMPLER 0x6E
#define SET_CONTEXT_REG_END 0x29000
#define SET_RESOURCE_START 0x38000
#define SET_RESOURCE_END 0x3C000
#define SET_SAMPLER_START 0x3C000
#define SET_SAMPLER_END 0x3CFF0

/* What a context register may hold */
enum { REG_ANY, REG_ZERO, REG_KEPT, REG_SHADER };

static const struct {
    uint32_t first, last;
    uint8_t kind;
} context_regs[] = {
    { DB_DEPTH_SIZE, DB_DEPTH_VIEW, REG_KEPT },
    { DB_DEPTH_BASE, DB_DEPTH_INFO, REG_KEPT },
    { PA_SC_SCREEN_SCISSOR_TL, PA_SC_SCREEN_SCISSOR_TL + 4, REG_ANY },
    { CB_COLOR0_BASE, CB_COLOR0_BASE, REG_KEPT },
    { CB_COLOR0_SIZE, CB_COLOR0_SIZE, REG_KEPT },
    { CB_COLOR0_VIEW, CB_COLOR0_VIEW, REG_KEPT },
    { CB_COLOR0_INFO, CB_COLOR0_INFO, REG_KEPT },
    { CB_COLOR0_TILE, CB_COLOR0_TILE, REG_ZERO },
    { CB_COLOR0_FRAG, CB_COLOR0_FRAG, REG_ZERO },
    { CB_COLOR0_MASK, CB_COLOR0_MASK, REG_ZERO },
    { PA_SC_WINDOW_SCISSOR_TL, PA_SC_WINDOW_SCISSOR_TL + 4, REG_ANY },
    { CB_TARGET_MASK, CB_TARGET_MASK, REG_KEPT },
    { CB_SHADER_MASK, CB_SHADER_MASK, REG_ANY },
    { PA_SC_GENERIC_SCISSOR_TL, PA_SC_GENERIC_SCISSOR_TL + 4, REG_ANY },
    { VGT_MAX_VTX_INDX, VGT_MAX_VTX_INDX, REG_ANY },
    { SPI_VS_OUT_ID_0, SPI_VS_OUT_ID_9, REG_ANY },
    { SPI_PS_INPUT_CNTL_0, SPI_PS_INPUT_CNTL_31, REG_ANY },
    { SPI_VS_OUT_CONFIG, SPI_VS_OUT_CONFIG, REG_ANY },
    { SPI_PS_IN_CONTROL_0, SPI_PS_IN_CONTROL_0, REG_ANY },
    { DB_DEPTH_CONTROL, DB_DEPTH_CONTROL, REG_ANY },
    { PA_CL_VTE_CNTL, PA_CL_VTE_CNTL, REG_ANY },
    { SQ_PGM_START_PS, SQ_PGM_START_PS, REG_SHADER },
    { SQ_PGM_RESOURCES_PS, SQ_PGM_EXPORTS_PS, REG_ANY },
    { SQ_PGM_START_VS, SQ_PGM_START_VS, REG_SHADER },
    { SQ_PGM_RESOURCES_VS, SQ_PGM_RESOURCES_VS, REG_ANY },
    { SQ_PGM_CF_OFFSET_PS, SQ_PGM_CF_OFFSET_VS, REG_ANY },
};

/* The registers a draw depends on, as this buffer set them */
enum {
    K_CB_BASE, K_CB_SIZE, K_CB_VIEW, K_CB_INFO, K_TARGET_MASK,
    K_DB_BASE, K_DB_SIZE, K_DB_VIEW, K_DB_INFO, K_VS, K_PS, NKEPT,
};

struct cs {
    uint32_t set;                   /* bit K_*: set by this buffer */
    uint32_t v[NKEPT];
    const char *why;                /* the reason a buffer was refused */
};

static int kept_index(uint32_t reg)
{
    switch (reg) {
    case CB_COLOR0_BASE: return K_CB_BASE;
    case CB_COLOR0_SIZE: return K_CB_SIZE;
    case CB_COLOR0_VIEW: return K_CB_VIEW;
    case CB_COLOR0_INFO: return K_CB_INFO;
    case CB_TARGET_MASK: return K_TARGET_MASK;
    case DB_DEPTH_BASE: return K_DB_BASE;
    case DB_DEPTH_SIZE: return K_DB_SIZE;
    case DB_DEPTH_VIEW: return K_DB_VIEW;
    case DB_DEPTH_INFO: return K_DB_INFO;
    case SQ_PGM_START_VS: return K_VS;
    case SQ_PGM_START_PS: return K_PS;
    }
    return -1;
}

static int refuse(struct cs *cs, const char *why)
{
    cs->why = why;
    return -1;
}

static int check_context_reg(struct cs *cs, uint32_t reg, uint32_t v)
{
    for (uint32_t i = 0; i < ARRAY_SIZE(context_regs); i++) {
        if (reg < context_regs[i].first || reg > context_regs[i].last)
            continue;
        switch (context_regs[i].kind) {
        case REG_ZERO:
            return v ? refuse(cs, "CMASK or FMASK address") : 0;
        case REG_SHADER:
            if (!radeon_caller_owns((uint64_t)v << 8, 4))
                return refuse(cs, "shader not in the caller's memory");
            /* fall through */
        case REG_KEPT: {
            int k = kept_index(reg);
            cs->v[k] = v;
            cs->set |= 1u << k;
            return 0;
        }
        default:
            return 0;
        }
    }
    return refuse(cs, "context register not allowed");
}

/* The bytes a color or depth buffer covers: its SIZE register gives the
 * line in units of 8 pixels and the whole in units of 64. Lines must be
 * a multiple of 64 pixels and the height of 8, as the GL library has
 * them, so that the engine covers no more than that. */
static int surface_bytes(uint32_t size, uint32_t bpp, uint64_t *out)
{
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t pixels = ((size >> 10 & 0xFFFFF) + 1) * 64;
    if (pitch % 64 || pixels % (pitch * 8))
        return -1;
    *out = (uint64_t)pixels * bpp;
    return 0;
}

static int buffer_ok(struct cs *cs, int base, int size, int view, uint32_t bpp)
{
    uint32_t need = 1u << base | 1u << size | 1u << view;
    uint64_t bytes;
    if ((cs->set & need) != need)
        return refuse(cs, "draw before its buffer is set");
    if (cs->v[view])
        return refuse(cs, "more than one slice");
    if (surface_bytes(cs->v[size], bpp, &bytes) < 0)
        return refuse(cs, "buffer lines not aligned");
    if (!radeon_caller_owns((uint64_t)cs->v[base] << 8, bytes))
        return refuse(cs, "buffer not in the caller's memory");
    return 0;
}

/* What a draw writes: color buffer 0 and the depth buffer. */
static int check_draw(struct cs *cs)
{
    uint32_t need = 1u << K_TARGET_MASK | 1u << K_CB_INFO | 1u << K_DB_INFO | 1u << K_VS | 1u << K_PS;
    if ((cs->set & need) != need)
        return refuse(cs, "draw before its state is set");
    if (cs->v[K_TARGET_MASK] & ~0xFu)
        return refuse(cs, "color buffers other than 0");
    uint32_t cb = cs->v[K_CB_INFO];
    if ((cs->v[K_TARGET_MASK] & 0xF) && CB_COLOR0_FORMAT(cb)) {
        if (CB_COLOR0_FORMAT(cb) != FMT_8_8_8_8 || CB_COLOR0_ARRAY_MODE(cb) != ARRAY_LINEAR_ALIGNED ||
            CB_COLOR0_TILE_MODE(cb))
            return refuse(cs, "color buffer format");
        if (buffer_ok(cs, K_CB_BASE, K_CB_SIZE, K_CB_VIEW, 4) < 0)
            return -1;
    }
    uint32_t db = cs->v[K_DB_INFO];
    if (DB_FORMAT(db)) {
        uint32_t mode = DB_ARRAY_MODE(db);
        uint32_t bpp = DB_FORMAT(db) == 1 ? 2 : DB_FORMAT(db) == 7 ? 8 : 4;
        if ((mode != ARRAY_LINEAR_ALIGNED && mode != ARRAY_1D_TILED_THIN1) || (db & DB_TILE_SURFACE_ENABLE))
            return refuse(cs, "depth buffer format");
        if (buffer_ok(cs, K_DB_BASE, K_DB_SIZE, K_DB_VIEW, bpp) < 0)
            return -1;
    }
    return 0;
}

/* One SET_RESOURCE entry: a vertex buffer or a 2D texture, read from. */
static int check_resource(struct cs *cs, const uint32_t *w)
{
    switch (w[6] >> 30) {
    case 0:                                     /* not valid: unused */
        return 0;
    case SQ_TEX_VTX_VALID_BUFFER: {
        uint64_t addr = (uint64_t)(w[2] & 0xFF) << 32 | w[0];
        if (!radeon_caller_owns(addr, (uint64_t)w[1] + 1))
            return refuse(cs, "vertex buffer not in the caller's memory");
        return 0;
    }
    case SQ_TEX_VTX_VALID_TEXTURE: {
        uint32_t dim = w[0] & 7, mode = w[0] >> 3 & 0xF;
        uint32_t pitch = ((w[0] >> 8 & 0x7FF) + 1) * 8, width = (w[0] >> 19) + 1;
        uint32_t height = (w[1] & 0x1FFF) + 1, depth = w[1] >> 13 & 0x1FFF;
        if (dim > 1 || mode > ARRAY_LINEAR_ALIGNED || depth || w[1] >> 26 != FMT_8_8_8_8 ||
            width > pitch || (mode == ARRAY_LINEAR_ALIGNED && pitch % 64) ||
            w[4] >> 28 || (w[5] & 0x3FFFFFFF))  /* one level, no arrays */
            return refuse(cs, "texture format");
        if (!radeon_caller_owns((uint64_t)w[2] << 8, (uint64_t)pitch * height * 4))
            return refuse(cs, "texture not in the caller's memory");
        return 0;
    }
    }
    return refuse(cs, "resource type");
}

/* count registers from the packet's index into a block of [first, end). */
static int in_range(uint32_t index, uint32_t count, uint32_t first, uint32_t end)
{
    uint32_t regs = (end - first) / 4;
    return index < regs && count <= regs - index;
}

/* Check the buffer of n dwords, packet by packet. */
static int check_buffer(struct cs *cs, const uint32_t *ib, uint32_t n, uint32_t *at)
{
    uint32_t i = 0;
    while (i < n) {
        *at = i;
        uint32_t hdr = ib[i];
        if (hdr >> 30 == 2) {                   /* PACKET2: filler */
            i++;
            continue;
        }
        if (hdr >> 30 != 3)
            return refuse(cs, "packet type 0 or 1");
        uint32_t op = hdr >> 8 & 0xFF, count = (hdr >> 16 & 0x3FFF) + 1;
        if (count > n - i - 1)
            return refuse(cs, "packet past the end");
        const uint32_t *d = ib + i + 1;
        switch (op) {
        case PACKET3_SET_CONTEXT_REG:
            if (count < 2 || !in_range(d[0], count - 1, 0x28000u, SET_CONTEXT_REG_END))
                return refuse(cs, "context registers out of range");
            for (uint32_t k = 1; k < count; k++)
                if (check_context_reg(cs, 0x28000u + (d[0] + k - 1) * 4, d[k]) < 0)
                    return -1;
            break;
        case PACKET3_SET_CONFIG_REG:
            if (count != 2 || d[0] != CONFIG_REG(VGT_PRIMITIVE_TYPE))
                return refuse(cs, "config register not allowed");
            break;
        case PACKET3_SET_RESOURCE:
            if (count < 8 || (count - 1) % 7 || d[0] % 7 ||
                !in_range(d[0], count - 1, SET_RESOURCE_START, SET_RESOURCE_END))
                return refuse(cs, "resources out of range");
            for (uint32_t k = 1; k < count; k += 7)
                if (check_resource(cs, d + k) < 0)
                    return -1;
            break;
        case PACKET3_SET_SAMPLER:
            if (count < 4 || (count - 1) % 3 || d[0] % 3 ||
                !in_range(d[0], count - 1, SET_SAMPLER_START, SET_SAMPLER_END))
                return refuse(cs, "samplers out of range");
            break;
        case PACKET3_SURFACE_SYNC:              /* caches only */
            if (count != 4)
                return refuse(cs, "SURFACE_SYNC length");
            break;
        case PACKET3_INDEX_TYPE:
        case PACKET3_NUM_INSTANCES:
            if (count != 1)
                return refuse(cs, "packet length");
            break;
        case PACKET3_DRAW_INDEX_AUTO:
            if (count != 2 || (d[1] & 3) != DI_SRC_SEL_AUTO_INDEX)
                return refuse(cs, "DRAW_INDEX_AUTO");
            if (check_draw(cs) < 0)
                return -1;
            break;
        default:
            return refuse(cs, "packet not allowed");
        }
        i += 1 + count;
    }
    return 0;
}


static int check(const uint32_t *ib, uint32_t n, uint32_t *at, const char **why)
{
    struct cs cs = { 0 };
    int r = check_buffer(&cs, ib, n, at);
    *why = cs.why;
    return r;
}

const struct accel_gen radeon_r700_accel = {
    .setup = setup,
    .md_tlbs = md_tlbs,
    .md_tlb0 = MC_VM_MD_L1_TLB0_CNTL,
    .vm_contexts = 7,
    .gart_range_flush = 1,
    .gpu_init = gpu_init,
    .microcode = microcode,
    .soft_reset = 0,
    .reset_with_microcode = 1,
    .hdp_flush = hdp_flush,
    .hw_contexts = hw_contexts,
    .begin_state = begin_state,
    .set_shaders = set_shaders,
    .set_render_target = set_render_target,
    .set_texture = set_texture,
    .set_vertices = set_vertices,
    .state = r7xx_default_state,
    .state_dwords = ARRAY_SIZE(r7xx_default_state),
    .vs = blit_vs,
    .vs_bytes = sizeof(blit_vs),
    .ps = blit_ps,
    .ps_bytes = sizeof(blit_ps),
    .check = check,
};
