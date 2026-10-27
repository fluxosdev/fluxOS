/*
 * uACPI's configuration for fluxOS, included ahead of every uACPI source
 * and of kernel/acpi, so both agree on it (see the Makefile).
 *
 * Messages arrive formatted by uACPI itself, one line at a time; frees
 * need no size; physical addresses stay 64-bit, as the tables give them,
 * and osl.c refuses what lies above 4 GiB. Before S3, uACPI flushes the
 * caches with this, so nothing dirty is lost when they lose power.
 */
#ifndef UACPI_FLUX_H
#define UACPI_FLUX_H

#define UACPI_ARCH_FLUSH_CPU_CACHE() __asm__ volatile("wbinvd" ::: "memory")

#endif
