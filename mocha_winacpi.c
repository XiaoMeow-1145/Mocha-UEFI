// SPDX-License-Identifier: GPL-2.0+
/* A single boot CPU and the standard ARM resources used during WinPE bring-up. */
#include <acpi/acpi_table.h>
#include <asm/system.h>
#include <dm/ofnode.h>
#include <efi_loader.h>
#include <mapmem.h>
#include <stdio.h>
#include <tables_csum.h>

#include "mocha_winacpi.h"

#define CPU_AFFINITY_MASK 0x00ffffff
#define GIC_DT_PPI 1
#define GIC_PPI_BASE 16
#define DT_IRQ_SENSE_MASK 0xf
#define DT_IRQ_LEVEL_HIGH 4
#define DT_IRQ_LEVEL_LOW 8
#define GICC_V51_SIZE offsetof(struct acpi_madt_gicc, efficiency)
#define WINACPI_ENTRIES 3

/* Scope (\_SB) { Device (CPU0) { Name (_HID, "ACPI0007") Name (_UID, Zero) } } */
static const u8 cpu_aml[] = {
	0x10, 0x22, 0x5c, '_', 'S', 'B', '_',
	0x5b, 0x82, 0x1a, 'C', 'P', 'U', '0',
	0x08, '_', 'H', 'I', 'D', 0x0d, 'A', 'C', 'P', 'I', '0', '0', '0', '7', 0,
	0x08, '_', 'U', 'I', 'D', 0x00,
};

struct winacpi_tables {
	struct acpi_rsdp rsdp __aligned(16);
	struct {
		struct acpi_table_header header;
		u32 entry[WINACPI_ENTRIES];
	} rsdt __aligned(16);
	struct {
		struct acpi_table_header header;
		u64 entry[WINACPI_ENTRIES];
	} __packed xsdt __aligned(16);
	struct acpi_fadt fadt __aligned(16);
	struct {
		struct acpi_madt header;
		u8 gicc[GICC_V51_SIZE];
		struct acpi_madt_gicd gicd;
	} __packed madt __aligned(16);
	struct acpi_gtdt gtdt __aligned(16);
	struct {
		struct acpi_table_header header;
		u8 aml[sizeof(cpu_aml)];
	} dsdt __aligned(16);
};

struct winacpi_resources {
	fdt_addr_t gicd;
	fdt_addr_t gicc;
	u32 timer_gsiv[4];
	u32 timer_flags[4];
};

static const efi_guid_t acpi_guid = EFI_ACPI_TABLE_GUID;

static bool winacpi_read_resources(struct winacpi_resources *res)
{
	ofnode root = ofnode_path("/");
	ofnode gic = ofnode_by_compatible(ofnode_null(), "arm,cortex-a15-gic");
	ofnode timer = ofnode_by_compatible(ofnode_null(), "arm,armv7-timer");
	ofnode parent;
	u32 interrupts[12];

	if (!ofnode_device_is_compatible(root, "xiaomi,mocha") &&
	    !ofnode_device_is_compatible(root, "linux,dummy-virt"))
		return false;
	if (!ofnode_valid(gic) || !ofnode_valid(timer) ||
	    !ofnode_is_enabled(gic) || !ofnode_is_enabled(timer))
		return false;

	parent = ofnode_parse_phandle(timer, "interrupt-parent", 0);
	if (!ofnode_valid(parent))
		parent = ofnode_parse_phandle(root, "interrupt-parent", 0);
	if (!ofnode_equal(parent, gic))
		return false;
	res->gicd = ofnode_get_addr_index(gic, 0);
	res->gicc = ofnode_get_addr_index(gic, 1);
	if (!res->gicd || !res->gicc || res->gicd == FDT_ADDR_T_NONE ||
	    res->gicc == FDT_ADDR_T_NONE || res->gicd == res->gicc)
		return false;

	if (ofnode_read_u32_array(timer, "interrupts", interrupts, ARRAY_SIZE(interrupts)))
		return false;
	for (int i = 0; i < ARRAY_SIZE(res->timer_gsiv); ++i) {
		u32 polarity = interrupts[i * 3 + 2] & DT_IRQ_SENSE_MASK;

		if (interrupts[i * 3] != GIC_DT_PPI || interrupts[i * 3 + 1] >= 16 ||
		    (polarity != DT_IRQ_LEVEL_HIGH && polarity != DT_IRQ_LEVEL_LOW))
			return false;
		res->timer_gsiv[i] = GIC_PPI_BASE + interrupts[i * 3 + 1];
		res->timer_flags[i] = polarity == DT_IRQ_LEVEL_LOW ? GTDT_FLAG_INT_ACTIVE_LOW : 0;
	}
	return true;
}

static void winacpi_header(struct acpi_table_header *header, const char *sig,
			   u32 length, u8 revision)
{
	memcpy(header->signature, sig, sizeof(header->signature));
	header->length = length;
	header->revision = revision;
	memcpy(header->oem_id, OEM_ID, sizeof(header->oem_id));
	memcpy(header->oem_table_id, "MOCHAWPE", sizeof(header->oem_table_id));
	header->oem_revision = 1;
	memcpy(header->creator_id, "UBOT", sizeof(header->creator_id));
	header->creator_revision = 1;
	header->checksum = table_compute_checksum(header, length);
}

static void winacpi_fill(struct winacpi_tables *tables,
			const struct winacpi_resources *res)
{
	u32 mpidr = read_mpidr() & CPU_AFFINITY_MASK;
	struct acpi_madt_gicc gicc = {
		.type = ACPI_APIC_GICC,
		.length = GICC_V51_SIZE,
		.cpu_if_num = mpidr & 0xff,
		.flags = ACPI_MADTF_ENABLED,
		.phys_base = res->gicc,
		.mpidr = mpidr,
	};

	memcpy(tables->dsdt.aml, cpu_aml, sizeof(cpu_aml));
	winacpi_header(&tables->dsdt.header, "DSDT", sizeof(tables->dsdt), 2);

	tables->fadt.x_dsdt = nomap_to_sysmem(&tables->dsdt);
	tables->fadt.dsdt = tables->fadt.x_dsdt;
	tables->fadt.preferred_pm_profile = ACPI_PM_TABLET;
	tables->fadt.flags = ACPI_FADT_HW_REDUCED_ACPI;
	tables->fadt.minor_revision = 1;
	winacpi_header(&tables->fadt.header, "FACP",
		       offsetof(struct acpi_fadt, hyp_vendor_id), ACPI_FADT_REV_ACPI_5_0);

	memcpy(tables->madt.gicc, &gicc, GICC_V51_SIZE);
	tables->madt.gicd.type = ACPI_APIC_GICD;
	tables->madt.gicd.length = sizeof(tables->madt.gicd);
	tables->madt.gicd.phys_base = res->gicd;
	tables->madt.gicd.gic_version = 2;
	winacpi_header(&tables->madt.header.header, "APIC", sizeof(tables->madt),
		       ACPI_MADT_REV_ACPI_5_0);

	/* The boot CPU uses architected CP15 timers. No MMIO timer frame is exposed. */
	tables->gtdt.cnt_ctrl_base = ~(u64)0;
	tables->gtdt.cnt_read_base = ~(u64)0;
	tables->gtdt.sec_el1_gsiv = res->timer_gsiv[0];
	tables->gtdt.sec_el1_flags = res->timer_flags[0];
	tables->gtdt.el1_gsiv = res->timer_gsiv[1];
	tables->gtdt.el1_flags = res->timer_flags[1];
	tables->gtdt.virt_el1_gsiv = res->timer_gsiv[2];
	tables->gtdt.virt_el1_flags = res->timer_flags[2];
	tables->gtdt.el2_gsiv = res->timer_gsiv[3];
	tables->gtdt.el2_flags = res->timer_flags[3];
	winacpi_header(&tables->gtdt.header, "GTDT",
		       offsetof(struct acpi_gtdt, virt_el2_gsiv), 2);

	tables->rsdt.entry[0] = nomap_to_sysmem(&tables->fadt);
	tables->rsdt.entry[1] = nomap_to_sysmem(&tables->madt);
	tables->rsdt.entry[2] = nomap_to_sysmem(&tables->gtdt);
	for (int i = 0; i < WINACPI_ENTRIES; ++i)
		tables->xsdt.entry[i] = tables->rsdt.entry[i];
	winacpi_header(&tables->rsdt.header, "RSDT", sizeof(tables->rsdt), 1);
	winacpi_header(&tables->xsdt.header, "XSDT", sizeof(tables->xsdt), 1);

	memcpy(tables->rsdp.signature, RSDP_SIG, sizeof(tables->rsdp.signature));
	memcpy(tables->rsdp.oem_id, OEM_ID, sizeof(tables->rsdp.oem_id));
	tables->rsdp.revision = ACPI_RSDP_REV_ACPI_2_0;
	tables->rsdp.rsdt_address = nomap_to_sysmem(&tables->rsdt);
	tables->rsdp.xsdt_address = nomap_to_sysmem(&tables->xsdt);
	tables->rsdp.length = sizeof(tables->rsdp);
	tables->rsdp.checksum = table_compute_checksum(&tables->rsdp, 20);
	tables->rsdp.ext_checksum = table_compute_checksum(&tables->rsdp,
							  sizeof(tables->rsdp));
}

efi_status_t winprobe_install_acpi(u64 *page)
{
	struct winacpi_tables *tables;
	struct winacpi_resources resources;
	efi_status_t status;
	u64 address = 0;

	_Static_assert(sizeof(*tables) <= EFI_PAGE_SIZE, "ACPI tables must fit in one page");

	if (efi_get_configuration_table(&acpi_guid))
		return EFI_ALREADY_STARTED;
	if (!winacpi_read_resources(&resources)) {
		puts("WINPROBE unsupported GICv2 or architected timers in FDT\n");
		return EFI_UNSUPPORTED;
	}
	status = efi_allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_ACPI_RECLAIM_MEMORY,
				    1, &address);
	if (status != EFI_SUCCESS)
		return status;

	tables = (void *)(uintptr_t)address;
	memset(tables, 0, EFI_PAGE_SIZE);
	winacpi_fill(tables, &resources);
	status = efi_install_configuration_table(&acpi_guid, &tables->rsdp);
	if (status != EFI_SUCCESS) {
		efi_free_pages(address, 1);
		return status;
	}

	*page = address;
	printf("WINPROBE ACPI RSDP=%llx GICD=%llx GICC=%llx timer=%u/%u/%u/%u\n",
	       (unsigned long long)nomap_to_sysmem(&tables->rsdp),
	       (unsigned long long)resources.gicd, (unsigned long long)resources.gicc,
	       resources.timer_gsiv[0], resources.timer_gsiv[1],
	       resources.timer_gsiv[2], resources.timer_gsiv[3]);
	return EFI_SUCCESS;
}

efi_status_t winprobe_remove_acpi(u64 page)
{
	efi_status_t status;

	status = efi_install_configuration_table(&acpi_guid, NULL);
	if (status != EFI_SUCCESS)
		return status;
	return efi_free_pages(page, 1);
}
