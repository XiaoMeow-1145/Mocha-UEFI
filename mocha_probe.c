// SPDX-License-Identifier: GPL-2.0+
/* ARM UEFI smoke test. Built as the embedded bootefi hello payload. */

#include <efi_api.h>

static const efi_guid_t fdt_guid = EFI_FDT_GUID;
static const efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const efi_guid_t loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const efi_guid_t filesystem_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static void print_hex_width(struct efi_simple_text_output_protocol *out,
			    u64 value, int width)
{
	static const u16 digits[] = u"0123456789abcdef";
	u16 text[19] = u"0x";
	int i;

	for (i = width + 1; i >= 2; --i) {
		text[i] = digits[value & 0xf];
		value >>= 4;
	}
	out->output_string(out, text);
}

static void print_hex(struct efi_simple_text_output_protocol *out, u32 value)
{
	print_hex_width(out, value, 8);
}

static void report_reserved_memory(struct efi_simple_text_output_protocol *out,
				   const u8 *map, efi_uintn_t size,
				   efi_uintn_t stride)
{
	for (efi_uintn_t offset = 0; offset < size; offset += stride) {
		struct efi_mem_desc desc;

		memcpy(&desc, map + offset, sizeof(desc));
		if (desc.type != EFI_RESERVED_MEMORY_TYPE)
			continue;
		out->output_string(out, u"EFI reserved memory start=");
		print_hex_width(out, desc.physical_start, 16);
		out->output_string(out, u", pages=");
		print_hex_width(out, desc.num_pages, 16);
		out->output_string(out, u"\r\n");
	}
}

/* The pool allocation can change the map. Allow headroom and retry. */
static efi_status_t check_memory_map(struct efi_system_table *st)
{
	struct efi_boot_services *bs = st->boottime;
	struct efi_simple_text_output_protocol *out = st->con_out;
	efi_uintn_t size = 0, key, stride = 0;
	u32 version;
	int attempt;
	efi_status_t status;

	status = bs->get_memory_map(&size, NULL, &key, &stride, &version);
	if (status != EFI_BUFFER_TOO_SMALL)
		return status == EFI_SUCCESS ? EFI_DEVICE_ERROR : status;

	for (attempt = 0; attempt < 3; ++attempt) {
		void *buffer;
		efi_status_t release;

		if (stride < sizeof(struct efi_mem_desc) ||
		    stride > (~(efi_uintn_t)0 - size) / 2)
			return EFI_DEVICE_ERROR;
		size += 2 * stride;
		status = bs->allocate_pool(EFI_LOADER_DATA, size, &buffer);
		if (status != EFI_SUCCESS)
			return status;

		status = bs->get_memory_map(&size, buffer, &key, &stride,
					    &version);
		if (status == EFI_SUCCESS) {
			if (stride < sizeof(struct efi_mem_desc) ||
			    size < stride || size % stride) {
				status = EFI_DEVICE_ERROR;
			} else {
				out->output_string(out, u"GetMemoryMap OK, bytes=");
				print_hex(out, size);
				out->output_string(out, u", descriptor size=");
				print_hex(out, stride);
				out->output_string(out, u"\r\n");
				report_reserved_memory(out, buffer, size, stride);
			}
		}
		release = bs->free_pool(buffer);
		if (release != EFI_SUCCESS)
			return release;
		if (status != EFI_BUFFER_TOO_SMALL)
			return status;
	}

	return EFI_BUFFER_TOO_SMALL;
}

static bool has_fdt(const struct efi_system_table *st)
{
	efi_uintn_t i;

	for (i = 0; i < st->nr_tables; ++i) {
		const struct efi_configuration_table *entry = &st->tables[i];
		const u8 *header = entry->table;

		if (memcmp(&entry->guid, &fdt_guid, sizeof(fdt_guid)))
			continue;
		/* Flattened Device Tree header magic is big-endian. */
		return header && header[0] == 0xd0 && header[1] == 0x0d &&
		       header[2] == 0xfe && header[3] == 0xed;
	}

	return false;
}

/* A media-loaded image should be able to open its own source volume. */
static efi_status_t check_boot_volume(efi_handle_t image,
				      struct efi_system_table *st)
{
	struct efi_boot_services *bs = st->boottime;
	struct efi_simple_text_output_protocol *out = st->con_out;
	struct efi_simple_file_system_protocol *fs;
	struct efi_loaded_image *loaded;
	struct efi_file_handle *root;
	efi_status_t status;
	void *interface;

	status = bs->handle_protocol(image, &loaded_image_guid, &interface);
	if (status != EFI_SUCCESS)
		return status;
	loaded = interface;
	if (!loaded->device_handle) {
		out->output_string(out, u"EFI boot volume unavailable\r\n");
		return EFI_SUCCESS;
	}

	status = bs->handle_protocol(loaded->device_handle, &filesystem_guid,
				     &interface);
	if (status == EFI_UNSUPPORTED || status == EFI_NOT_FOUND) {
		out->output_string(out, u"EFI boot volume unavailable\r\n");
		return EFI_SUCCESS;
	}
	if (status != EFI_SUCCESS)
		return status;

	fs = interface;
	status = fs->open_volume(fs, &root);
	if (status != EFI_SUCCESS)
		return status;
	status = root->close(root);
	if (status == EFI_SUCCESS)
		out->output_string(out, u"EFI boot volume open OK\r\n");
	return status;
}

static void report_gop(struct efi_system_table *st)
{
	struct efi_simple_text_output_protocol *out = st->con_out;
	void *interface;
	struct efi_gop *gop;

	if (st->boottime->locate_protocol(&gop_guid, NULL, &interface) !=
	    EFI_SUCCESS) {
		out->output_string(out, u"GOP unavailable\r\n");
		return;
	}

	gop = interface;
	if (!gop || !gop->mode || !gop->mode->info) {
		out->output_string(out, u"GOP available without an active mode\r\n");
		return;
	}

	out->output_string(out, u"GOP active, width=");
	print_hex(out, gop->mode->info->width);
	out->output_string(out, u", height=");
	print_hex(out, gop->mode->info->height);
	out->output_string(out, u"\r\n");
}

efi_status_t EFIAPI efi_main(efi_handle_t image, struct efi_system_table *st)
{
	struct efi_simple_text_output_protocol *out;
	efi_status_t status;

	if (!st || st->hdr.signature != EFI_SYSTEM_TABLE_SIGNATURE ||
	    !st->con_out || !st->boottime ||
	    st->boottime->hdr.signature != EFI_BOOT_SERVICES_SIGNATURE)
		return EFI_INVALID_PARAMETER;

	out = st->con_out;
	status = out->output_string(out, u"Mocha UEFI probe (AArch32)\r\n");
	if (status != EFI_SUCCESS)
		return status;
	out->output_string(out, u"UEFI revision=");
	print_hex(out, st->hdr.revision);
	out->output_string(out, u"\r\n");

	status = check_memory_map(st);
	if (status != EFI_SUCCESS) {
		out->output_string(out, u"GetMemoryMap failed, status=");
		print_hex(out, status);
		out->output_string(out, u"\r\n");
		return status;
	}
	if (!has_fdt(st)) {
		out->output_string(out, u"FDT missing or invalid\r\n");
		return EFI_NOT_FOUND;
	}
	out->output_string(out, u"FDT table and header OK\r\n");

	status = check_boot_volume(image, st);
	if (status != EFI_SUCCESS) {
		out->output_string(out, u"EFI boot volume failed, status=");
		print_hex(out, status);
		out->output_string(out, u"\r\n");
		return status;
	}

	report_gop(st);
	out->output_string(out, u"UEFI probe PASS\r\n");
	return EFI_SUCCESS;
}
