/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef MOCHA_WINACPI_H
#define MOCHA_WINACPI_H

#include <efi.h>

efi_status_t winprobe_install_acpi(u64 *page);
efi_status_t winprobe_remove_acpi(u64 page);

#endif
