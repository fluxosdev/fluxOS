/*
 * AtomBIOS interpreter (atom.c), ported from Linux's radeon driver, which
 * has it from AMD under the MIT license:
 *
 * Copyright 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Stanislaw Skowronek
 *
 * An ATI video BIOS carries "command tables", bytecode routines for mode
 * setting and the like, and "data tables" describing the board. The
 * interpreter runs a command table against the card's registers.
 */
#ifndef ATOM_H
#define ATOM_H

#include <stdint.h>

/* How the interpreter reaches the card; register indexes are in dwords. */
struct atom_card {
    uint32_t (*reg_read)(uint32_t idx);
    void (*reg_write)(uint32_t idx, uint32_t val);
    uint32_t (*ioreg_read)(uint32_t idx);
    void (*ioreg_write)(uint32_t idx, uint32_t val);
};

struct atom_context {
    const struct atom_card *card;
    const uint8_t *bios;
    uint32_t bios_size;
    uint16_t cmd_table, data_table;
    uint16_t iio[256];              /* indirect I/O programs, by port */
    uint16_t data_block;
    uint32_t fb_base;
    uint32_t divmul[2];
    uint16_t io_attr;
    uint16_t reg_block;
    uint8_t shift;
    int cs_equal, cs_above;
    int io_mode;
    uint32_t *scratch;              /* the "frame buffer" scratch area tables may use */
    uint32_t scratch_size_bytes;
    char name[48];                  /* the BIOS's own description */
};

/* Data table indexes (ATOM_MASTER_LIST_OF_DATA_TABLES) */
#define ATOM_DATA_FIRMWARE_INFO   4
#define ATOM_DATA_LCD_INFO        6
#define ATOM_DATA_GPIO_I2C_INFO   10
#define ATOM_DATA_VRAM_USAGE      11
#define ATOM_DATA_OBJECT_HEADER   22

/* Command table indexes (ATOM_MASTER_LIST_OF_COMMAND_TABLES) */
#define ATOM_CMD_ENABLE_CRTC_MEMREQ   6
#define ATOM_CMD_SET_PIXEL_CLOCK      12
#define ATOM_CMD_ADJUST_DISPLAY_PLL   17
#define ATOM_CMD_DAC1_ENCODER         24
#define ATOM_CMD_DAC2_ENCODER         25
#define ATOM_CMD_ENABLE_SCALER        33
#define ATOM_CMD_BLANK_CRTC           34
#define ATOM_CMD_ENABLE_CRTC          35
#define ATOM_CMD_SET_CRTC_OVERSCAN    40
#define ATOM_CMD_SELECT_CRTC_SOURCE   42
#define ATOM_CMD_UPDATE_CRTC_DBUF     44
#define ATOM_CMD_SET_CRTC_DTD_TIMING  49
#define ATOM_CMD_ENABLE_YUV           73    /* EnableYUV is GetDispObjectInfo's slot */
#define ATOM_CMD_EXTERNAL_ENCODER     50
#define ATOM_CMD_PROCESS_AUX          78    /* ProcessAuxChannelTransaction */
#define ATOM_CMD_DP_ENCODER_SERVICE   79
#define ATOM_CMD_DAC1_OUTPUT          68
#define ATOM_CMD_DAC2_OUTPUT          69
#define ATOM_CMD_DIG1_ENCODER         74
#define ATOM_CMD_DIG2_ENCODER         75
#define ATOM_CMD_UNIPHY_TRANSMITTER   76    /* DIG1TransmitterControl */
#define ATOM_CMD_LVTMA_TRANSMITTER    77    /* DIG2TransmitterControl */

/* Check the BIOS image and index it. Returns 0, or -1 if it is no AtomBIOS. */
int atom_parse(struct atom_context *ctx, const struct atom_card *card, const uint8_t *bios,
               uint32_t size);

/* Run command table index with params (at most ATOM_PARAMS_BYTES, read back
 * afterwards). Returns 0, or -1 if the table is missing or got stuck. */
#define ATOM_PARAMS_BYTES 64
int atom_execute(struct atom_context *ctx, int index, void *params, uint32_t size);

/* Where a data table starts, with its header's revisions; 0 if absent. */
uint16_t atom_data_table(struct atom_context *ctx, int index, uint16_t *size, uint8_t *frev,
                         uint8_t *crev);

/* A command table's revisions; returns 0 if it is absent. */
int atom_cmd_revision(struct atom_context *ctx, int index, uint8_t *frev, uint8_t *crev);

/* Little-endian reads from the BIOS image; 0 past its end. */
uint8_t atom_u8(const struct atom_context *ctx, uint32_t off);
uint16_t atom_u16(const struct atom_context *ctx, uint32_t off);
uint32_t atom_u32(const struct atom_context *ctx, uint32_t off);

#endif
