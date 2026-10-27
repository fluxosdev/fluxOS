/*
 * ACPI inside the kernel (kernel/acpi): what its parts share. Only these
 * files see uACPI's headers; the rest of the kernel uses drivers.h.
 *
 *   acpi.c   the tables at boot, the namespace, power-off, the events
 *   osl.c    the kernel services uACPI is built on, and its worker threads
 *   ec.c     the embedded controller, which most of a laptop hangs off
 *   power.c  AC adapter, batteries, lid, buttons, thermal zone, backlight
 *   sleep.c  suspend to RAM (S3), with wakeup.S
 */
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <uacpi/types.h>

/* acpi.c */
extern int acpi_aml;                    /* uACPI runs the firmware's AML */
uint32_t acpi_rsdp_phys(void);
int acpi_eval_int(uacpi_namespace_node *node, const char *path, uint64_t *out);
void acpi_events_resume(void);          /* the fixed buttons on again after S3 */

/* osl.c */
void osl_init(void);                    /* the clock; before uacpi_initialize() */
void osl_start(void);                   /* the worker threads, once processes run */
int osl_can_sleep(void);                /* a process with interrupts on */
void osl_suspend(void);                 /* the clock across S3 */
void osl_resume(void);
/* Run fn(arg) in the GPE worker (on the boot processor), as uACPI runs
 * GPE methods: for the EC's queries, which run _Qxx methods. 0, or -1 if
 * the queue is full. From interrupt handlers too. */
int osl_defer(void (*fn)(void *), void *arg);

/* ec.c */
void ec_early(void);                    /* from the ECDT, before the namespace loads */
void ec_init(void);                     /* the PNP0C09 device, once it has loaded */
void ec_start(void);                    /* its GPE, once the namespace is initialised */
void ec_poll(void);                     /* an event whose GPE went unseen */
void ec_resume(void);

/* power.c */
void power_init(void);                  /* find the devices; after the namespace */
void power_start(void);                 /* the power thread */
void power_fixed_button(int sleep);     /* the fixed power (0) or sleep (1) button; IRQ context */
void power_resumed(void);
int power_lid_closed(void);
void power_arm_wake(int on);            /* the lid and buttons may wake the machine */

/* sleep.c */
int sleep_supported(void);              /* the firmware has \_S3 */

#endif
