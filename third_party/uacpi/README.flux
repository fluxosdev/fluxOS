uACPI 6.1.1, https://github.com/uACPI/uACPI/tree/6.1.1
Copyright (c) 2022-2026 Daniil Tatianin. MIT license in LICENSE.
Unmodified include/ and source/ (the build files and tests left out). The
kernel's services it calls are kernel/acpi/osl.c, and its configuration is
kernel/acpi/uacpi_flux.h, included ahead of every source (see the Makefile).
