/*
 * The fluxOS logo (lib/logo.c, made by tools/mklogo.py), shared by the
 * kernel's startup screen and the desktop's shutdown screen.
 */
#ifndef FLUX_LOGO_H
#define FLUX_LOGO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define FLUXOS_LOGO_SIZE 128

/* 0xAARRGGBB, row by row, not premultiplied */
extern const uint32_t fluxos_logo[FLUXOS_LOGO_SIZE * FLUXOS_LOGO_SIZE];

#ifdef __cplusplus
}
#endif

#endif
