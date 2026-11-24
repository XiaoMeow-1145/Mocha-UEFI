// SPDX-License-Identifier: GPL-2.0+
#include <asm/system.h>
#include <blkmap.h>
#include <command.h>
#include <cpu_func.h>
#include <dm/device.h>
#include <efi_device_path.h>
#include <efi_loader.h>
#include <gzip.h>
#include <linux/unaligned/le_byteshift.h>
#include <stdio.h>
#include <time.h>
#include <u-boot/crc.h>
#include <video.h>

#include "mocha_winacpi.h"

#define WINPROBE_MAX_DISK_SIZE (256U * 1024U * 1024U)

/* The build embeds a gzip file in the normal relocatable U-Boot image. */
extern const unsigned char winprobe_disk_gz[], winprobe_disk_gz_end[];
asm(".pushsection .rodata.winprobe_disk, \"a\"\n"
    ".balign 4\n"
    ".global winprobe_disk_gz, winprobe_disk_gz_end\n"
    "winprobe_disk_gz:\n"
    ".incbin \"cmd/mocha_winprobe_disk.gz\"\n"
    "winprobe_disk_gz_end:\n"
    ".popsection\n");

#ifdef CONFIG_TARGET_MOCHA
/* This area is writable at the vendor chainload address before relocation. */
asm(".pushsection .data.winprobe_early_stack, \"aw\", %progbits\n"
    ".balign 64\n"
    ".global winprobe_early_stack_start, winprobe_early_stack_top\n"
    "winprobe_early_stack_start:\n"
    ".zero 0x40000\n"
    "winprobe_early_stack_top:\n"
    ".popsection\n");
#endif

static void winprobe_stage(const char *message)
{
	printf("WINPROBE %s\n", message);
	if (IS_ENABLED(CONFIG_VIDEO))
		video_sync_all();
}

static efi_status_t winprobe_load(struct udevice *ram, const char *name,
				 efi_handle_t *image)
{
	struct efi_device_path *device = NULL, *file = NULL;
	efi_status_t status;
	char partition[24];

	snprintf(partition, sizeof(partition), "%x:1", dev_seq(ram));
	status = efi_dp_from_name("blkmap", partition, name, &device, &file);
	if (status == EFI_SUCCESS)
		status = EFI_CALL(efi_load_image(false, efi_root, file, NULL, 0,
						 image));
	printf("WINPROBE LoadImage %s = 0x%016llx\n", name,
	       (unsigned long long)status);
	if (file)
		efi_free_pool(file);
	if (device)
		efi_free_pool(device);
	return status;
}

static int do_winprobe(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	const unsigned char *trailer = winprobe_disk_gz_end - 8;
	unsigned long size = winprobe_disk_gz_end - winprobe_disk_gz;
	struct udevice *ram;
	efi_handle_t image = NULL;
	efi_status_t status;
	u64 address = 0, acpi_page = 0;
	u32 raw_size, output_crc, expected_crc, pages, cntfrq;
	unsigned long start;
	void *disk;
	int error;
	bool test_icache = argc == 2 && !strcmp(argv[1], "acpi-icache");

	if (argc == 2 && !test_icache && strcmp(argv[1], "acpi"))
		return CMD_RET_USAGE;

	winprobe_stage("starting embedded RAM disk");
	if (size < 18)
		return CMD_RET_FAILURE;
	raw_size = get_unaligned_le32(trailer + 4);
	if (!raw_size || raw_size > WINPROBE_MAX_DISK_SIZE ||
	    (raw_size & (EFI_PAGE_SIZE - 1))) {
		printf("WINPROBE invalid disk size 0x%x\n", raw_size);
		return CMD_RET_FAILURE;
	}

	winprobe_stage("initializing EFI services");
	status = efi_init_obj_list();
	if (status != EFI_SUCCESS) {
		printf("WINPROBE EFI setup = 0x%016llx\n",
		       (unsigned long long)status);
		return CMD_RET_FAILURE;
	}
	winprobe_stage("installing board FDT");
	status = efi_install_fdt(EFI_FDT_USE_INTERNAL);
	if (status != EFI_SUCCESS) {
		printf("WINPROBE FDT setup = 0x%016llx\n",
		       (unsigned long long)status);
		return CMD_RET_FAILURE;
	}
	if (argc == 2) {
		winprobe_stage("publishing ACPI for the boot CPU");
		status = winprobe_install_acpi(&acpi_page);
		if (status != EFI_SUCCESS) {
			printf("WINPROBE ACPI setup = 0x%016llx\n",
			       (unsigned long long)status);
			return CMD_RET_FAILURE;
		}
	}
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (cntfrq));
	printf("WINPROBE SCTLR=%08x CNTFRQ=%u Hz\n", get_cr(), cntfrq);
	winprobe_stage("allocating RAM disk pages");
	pages = raw_size >> EFI_PAGE_SHIFT;
	status = efi_allocate_pages(EFI_ALLOCATE_ANY_PAGES,
				    EFI_RESERVED_MEMORY_TYPE, pages, &address);
	if (status != EFI_SUCCESS) {
		printf("WINPROBE RAM allocation = 0x%016llx\n",
		       (unsigned long long)status);
		goto free_acpi;
	}

	disk = (void *)(uintptr_t)address;
	printf("WINPROBE gzip %p + 0x%lx -> %p + 0x%x\n",
	       winprobe_disk_gz, size, disk, raw_size);
	winprobe_stage("decompressing RAM disk");
	start = get_timer(0);
	error = gunzip(disk, raw_size, (unsigned char *)winprobe_disk_gz, &size);
	printf("WINPROBE inflate elapsed %lu ms\n", get_timer(start));
	if (error) {
		printf("WINPROBE gunzip failed: %d\n", error);
		status = EFI_DEVICE_ERROR;
		goto free_pages;
	}
	if (size != raw_size) {
		printf("WINPROBE RAM disk size mismatch: 0x%lx expected 0x%x\n",
		       size, raw_size);
		status = EFI_DEVICE_ERROR;
		goto free_pages;
	}
	winprobe_stage("checking RAM disk CRC32");
	start = get_timer(0);
	output_crc = crc32(0, disk, raw_size);
	printf("WINPROBE CRC elapsed %lu ms\n", get_timer(start));
	expected_crc = get_unaligned_le32(trailer);
	if (output_crc != expected_crc) {
		printf("WINPROBE RAM disk CRC32 mismatch: %08x expected %08x\n",
		       output_crc, expected_crc);
		status = EFI_DEVICE_ERROR;
		goto free_pages;
	}
	winprobe_stage("registering RAM disk");
	error = blkmap_create_ramdisk("winprobe", (ulong)address, raw_size, &ram);
	if (error) {
		printf("WINPROBE blkmap setup = %d\n", error);
		status = EFI_DEVICE_ERROR;
		goto free_pages;
	}
	printf("WINPROBE RAM disk ready at 0x%llx, bytes=0x%x\n",
	       (unsigned long long)address, raw_size);

	status = winprobe_load(ram, "/EFI/Microsoft/Boot/bootmgfw.efi", &image);
	if (status == EFI_NOT_FOUND)
		status = winprobe_load(ram, "/EFI/BOOT/BOOTARM.EFI", &image);
	if (status == EFI_SUCCESS) {
		bool icache_was_on = icache_status();

		if (test_icache) {
			if (!icache_was_on) {
				invalidate_icache_all();
				icache_enable();
			}
			printf("WINPROBE EFI entry SCTLR=%08x\n", get_cr());
		}
		if (test_icache && !icache_status())
			status = EFI_UNSUPPORTED;
		else
			status = do_bootefi_exec(image, NULL);
		if (test_icache && !icache_was_on)
			icache_disable();
		printf("WINPROBE image execution returned 0x%016llx\n",
		       (unsigned long long)status);
	}
	if (image)
		EFI_CALL(efi_unload_image(image));
	error = blkmap_destroy(ram);
	if (error) {
		printf("WINPROBE blkmap teardown = %d\n", error);
		status = EFI_DEVICE_ERROR;
		goto free_acpi;
	}
free_pages:
	efi_free_pages(address, pages);
free_acpi:
	if (acpi_page && winprobe_remove_acpi(acpi_page) != EFI_SUCCESS) {
		puts("WINPROBE ACPI cleanup failed\n");
		return CMD_RET_FAILURE;
	}
	return status == EFI_SUCCESS ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(winprobe, 2, 0, do_winprobe, "launch an EFI app from the embedded RAM ESP",
	   "[acpi|acpi-icache] - publish single-CPU ACPI; acpi-icache enables I-cache before EFI entry");
