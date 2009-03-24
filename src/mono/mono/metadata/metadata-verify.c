/*
 * metadata-verify.c: Metadata verfication support
 *
 * Author:
 *	Mono Project (http://www.mono-project.com)
 *
 * Copyright (C) 2005-2008 Novell, Inc. (http://www.novell.com)
 */

#include <mono/metadata/object-internals.h>
#include <mono/metadata/verify.h>
#include <mono/metadata/verify-internals.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/tokentype.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

/*
 TODO add fail fast mode
 TODO add PE32+ support
 TODO verify the entry point RVA and content.
 TODO load_section_table and load_data_directories must take PE32+ into account
 TODO add section relocation support
 TODO verify the relocation table, since we really don't use, no need so far.
 TODO do full PECOFF resources verification 
 TODO verify in the CLI header entry point and resources 
*/

#define INVALID_OFFSET ((guint32)-1)

enum {
	IMPORT_TABLE_IDX = 1, 
	RESOURCE_TABLE_IDX = 2,
	RELOCATION_TABLE_IDX = 5,
	IAT_IDX = 12,
	CLI_HEADER_IDX = 14,
};

enum {
	STRINGS_STREAM,
	USER_STRINGS_STREAM,
	BLOB_STREAM,
	GUID_STREAM,
	TILDE_STREAM
};

typedef struct {
	guint32 rva;
	guint32 size;
	guint32 translated_offset;
} DataDirectory;

typedef struct {
	guint32 offset;
	guint32 size;
} OffsetAndSize;

typedef struct {
	guint32 baseRVA;
	guint32 baseOffset;
	guint32 size;
	guint32 rellocationsRVA;
	guint16 numberOfRelocations;
} SectionHeader;

typedef struct {
	const char *data;
	guint32 size;
	GSList *errors;
	int valid;
	guint32 section_count;
	SectionHeader *sections;

	DataDirectory data_directories [16];
	OffsetAndSize metadata_streams [5]; //offset from begin of the image
} VerifyContext;

#define ADD_VERIFY_INFO(__ctx, __msg, __status, __exception)	\
	do {	\
		MonoVerifyInfoExtended *vinfo = g_new (MonoVerifyInfoExtended, 1);	\
		vinfo->info.status = __status;	\
		vinfo->info.message = ( __msg);	\
		vinfo->exception_type = (__exception);	\
		(__ctx)->errors = g_slist_prepend ((__ctx)->errors, vinfo);	\
	} while (0)


#define ADD_ERROR(__ctx, __msg)	\
	do {	\
		ADD_VERIFY_INFO(__ctx, __msg, MONO_VERIFY_ERROR, MONO_EXCEPTION_INVALID_PROGRAM); \
		(__ctx)->valid = 0; \
		return; \
	} while (0)

#define CHECK_STATE() do { if (!ctx.valid) goto cleanup; } while (0)

#define CHECK_ERROR() do { if (!ctx->valid) return; } while (0)

static guint32
pe_signature_offset (VerifyContext *ctx)
{
	return read32 (ctx->data + 0x3c);
}

static guint32
pe_header_offset (VerifyContext *ctx)
{
	return read32 (ctx->data + 0x3c) + 4;
}

static gboolean
bounds_check_virtual_address (VerifyContext *ctx, guint32 rva, guint32 size)
{
	int i;

	if (!ctx->sections)
		return FALSE;

	for (i = 0; i < ctx->section_count; ++i) {
		guint32 base = ctx->sections [i].baseRVA;
		guint32 end = ctx->sections [i].baseRVA + ctx->sections [i].size;
		if (rva >= base && rva + size <= end)
			return TRUE;
	}
	return FALSE;
}

static gboolean
bounds_check_offset (DataDirectory *dir, guint32 offset, guint32 size)
{
	if (dir->translated_offset > offset)
		return FALSE;
	if (dir->size < size)
		return FALSE;
	return offset + size <= dir->translated_offset + dir->size;
}


static guint32
translate_rva (VerifyContext *ctx, guint32 rva)
{
	int i;

	if (!ctx->sections)
		return FALSE;

	for (i = 0; i < ctx->section_count; ++i) {
		guint32 base = ctx->sections [i].baseRVA;
		guint32 end = ctx->sections [i].baseRVA + ctx->sections [i].size;
		if (rva >= base && rva <= end) {
			guint32 res = (rva - base) + ctx->sections [i].baseOffset;
			/* double check */
			return res >= ctx->size ? INVALID_OFFSET : res;
		}
	}

	return INVALID_OFFSET;
}

static void
verify_msdos_header (VerifyContext *ctx)
{
	guint32 lfanew;
	if (ctx->size < 128)
		ADD_ERROR (ctx, g_strdup ("Not enough space for the MS-DOS header"));
	if (ctx->data [0] != 0x4d || ctx->data [1] != 0x5a)
		ADD_ERROR (ctx,  g_strdup ("Invalid MS-DOS watermark"));
	lfanew = pe_signature_offset (ctx);
	if (lfanew > ctx->size - 4)
		ADD_ERROR (ctx, g_strdup ("MS-DOS lfanew offset points to outside of the file"));
}

static void
verify_pe_header (VerifyContext *ctx)
{
	guint32 offset = pe_signature_offset (ctx);
	const char *pe_header = ctx->data + offset;
	if (pe_header [0] != 'P' || pe_header [1] != 'E' ||pe_header [2] != 0 ||pe_header [3] != 0)
		ADD_ERROR (ctx,  g_strdup ("Invalid PE header watermark"));
	pe_header += 4;
	offset += 4;

	if (offset > ctx->size - 20)
		ADD_ERROR (ctx, g_strdup ("File with truncated pe header"));
	if (read16 (pe_header) != 0x14c)
		ADD_ERROR (ctx, g_strdup ("Invalid PE header Machine value"));
}

static void
verify_pe_optional_header (VerifyContext *ctx)
{
	guint32 offset = pe_header_offset (ctx);
	guint32 header_size, file_alignment;
	const char *pe_header = ctx->data + offset;
	const char *pe_optional_header = pe_header + 20;

	header_size = read16 (pe_header + 16);
	offset += 20;

	if (header_size < 2) /*must be at least 2 or we won't be able to read magic*/
		ADD_ERROR (ctx, g_strdup ("Invalid PE optional header size"));

	if (offset > ctx->size - header_size || header_size > ctx->size)
		ADD_ERROR (ctx, g_strdup ("Invalid PE optional header size"));

	if (read16 (pe_optional_header) == 0x10b) {
		if (header_size != 224)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid optional header size %d", header_size));

		/*if (read32 (pe_optional_header + 28) != 0x400000)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid Image base %x", read32 (pe_optional_header + 28)));*/
		if (read32 (pe_optional_header + 32) != 0x2000)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid Section Aligmnent %x", read32 (pe_optional_header + 32)));
		file_alignment = read32 (pe_optional_header + 36);
		if (file_alignment != 0x200 && file_alignment != 0x1000)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid file Aligmnent %x", file_alignment));
		/* All the junk in the middle is irrelevant, specially for mono. */
		if (read32 (pe_optional_header + 92) > 0x10)
			ADD_ERROR (ctx, g_strdup_printf ("Too many data directories %x", read32 (pe_optional_header + 92)));
	} else {
		if (read16 (pe_optional_header) == 0x20B)
			ADD_ERROR (ctx, g_strdup ("Metadata verifier doesn't handle PE32+"));
		else
			ADD_ERROR (ctx, g_strdup_printf ("Invalid optional header magic %d", read16 (pe_optional_header)));
	}
}

static void
load_section_table (VerifyContext *ctx)
{
	int i;
	SectionHeader *sections;
	guint32 offset =  pe_header_offset (ctx);
	const char *ptr = ctx->data + offset;
	guint16 num_sections = ctx->section_count = read16 (ptr + 2);

	offset += 244;/*FIXME, this constant is different under PE32+*/
	ptr += 244;

	if (num_sections * 40 > ctx->size - offset)
		ADD_ERROR (ctx, g_strdup ("Invalid PE optional header size"));

	sections = ctx->sections = g_new0 (SectionHeader, num_sections);
	for (i = 0; i < num_sections; ++i) {
		sections [i].size = read32 (ptr + 8);
		sections [i].baseRVA = read32 (ptr + 12);
		sections [i].baseOffset = read32 (ptr + 20);
		sections [i].rellocationsRVA = read32 (ptr + 24);
		sections [i].numberOfRelocations = read16 (ptr + 32);
		ptr += 40;
	}

	ptr = ctx->data + offset; /*reset it to the beggining*/
	for (i = 0; i < num_sections; ++i) {
		guint32 raw_size, flags;
		if (sections [i].baseOffset == 0)
			ADD_ERROR (ctx, g_strdup ("Metadata verifier doesn't handle sections with intialized data only"));
		if (sections [i].baseOffset >= ctx->size)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid PointerToRawData %x points beyond EOF", sections [i].baseOffset));
		if (sections [i].size > ctx->size - sections [i].baseOffset)
			ADD_ERROR (ctx, g_strdup ("Invalid VirtualSize points beyond EOF"));

		raw_size = read32 (ptr + 16);
		if (raw_size < sections [i].size)
			ADD_ERROR (ctx, g_strdup ("Metadata verifier doesn't handle sections with SizeOfRawData < VirtualSize"));

		if (raw_size > ctx->size - sections [i].baseOffset)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid SizeOfRawData %x points beyond EOF", raw_size));

		if (sections [i].rellocationsRVA || sections [i].numberOfRelocations)
			ADD_ERROR (ctx, g_strdup_printf ("Metadata verifier doesn't handle section relocation"));

		flags = read32 (ptr + 36);
		/*TODO 0xFE0000E0 is all flags from cil-coff.h OR'd. Make it a less magical number*/
		if (flags == 0 || (flags & ~0xFE0000E0) != 0)
			ADD_ERROR (ctx, g_strdup_printf ("Invalid section flags %x", flags));

		ptr += 40;
	}
}

static gboolean
is_valid_data_directory (int i)
{
	/*LAMESPEC 4 == certificate 6 == debug, MS uses both*/
	return i == 1 || i == 2 || i == 5 || i == 12 || i == 14 || i == 4 || i == 6; 
}

static void
load_data_directories (VerifyContext *ctx)
{
	guint32 offset =  pe_header_offset (ctx) + 116; /*FIXME, this constant is different under PE32+*/
	const char *ptr = ctx->data + offset;
	int i;

	for (i = 0; i < 16; ++i) {
		guint32 rva = read32 (ptr);
		guint32 size = read32 (ptr + 4);

		if ((rva != 0 || size != 0) && !is_valid_data_directory (i))
			ADD_ERROR (ctx, g_strdup_printf ("Invalid data directory %d", i));

		if (rva != 0 && !bounds_check_virtual_address (ctx, rva, size))
			ADD_ERROR (ctx, g_strdup_printf ("Invalid data directory %d rva/size pair %x/%x", i, rva, size));

		ctx->data_directories [i].rva = rva;
		ctx->data_directories [i].size = size;
		ctx->data_directories [i].translated_offset = translate_rva (ctx, rva);

		ptr += 8;
	}
}

#define SIZE_OF_MSCOREE (sizeof ("mscoree.dll"))

#define SIZE_OF_CORMAIN (sizeof ("_CorExeMain"))

static void
verify_hint_name_table (VerifyContext *ctx, guint32 import_rva, const char *table_name)
{
	const char *ptr;
	guint32 hint_table_rva;

	import_rva = translate_rva (ctx, import_rva);
	g_assert (import_rva != INVALID_OFFSET);

	hint_table_rva = read32 (ctx->data + import_rva);
	if (!bounds_check_virtual_address (ctx, hint_table_rva, SIZE_OF_CORMAIN + 2))
		ADD_ERROR (ctx, g_strdup_printf ("Invalid Hint/Name rva %d for %s", hint_table_rva, table_name));

	hint_table_rva = translate_rva (ctx, hint_table_rva);
	g_assert (hint_table_rva != INVALID_OFFSET);
	ptr = ctx->data + hint_table_rva + 2;

	if (memcmp ("_CorExeMain", ptr, SIZE_OF_CORMAIN) && memcmp ("_CorDllMain", ptr, SIZE_OF_CORMAIN)) {
		char name[SIZE_OF_CORMAIN];
		memcpy (name, ptr, SIZE_OF_CORMAIN);
		name [SIZE_OF_CORMAIN - 1] = 0;
		ADD_ERROR (ctx, g_strdup_printf ("Invalid Hint / Name: '%s'", name));
	}
}

static void
verify_import_table (VerifyContext *ctx)
{
	DataDirectory it = ctx->data_directories [IMPORT_TABLE_IDX];
	guint32 offset = it.translated_offset;
	const char *ptr = ctx->data + offset;
	guint32 name_rva, ilt_rva, iat_rva;

	g_assert (offset != INVALID_OFFSET);

	if (it.size < 40)
		ADD_ERROR (ctx, g_strdup_printf ("Import table size %d is smaller than 40", it.size));

	ilt_rva = read32 (ptr);
	if (!bounds_check_virtual_address (ctx, ilt_rva, 8))
		ADD_ERROR (ctx, g_strdup_printf ("Invalid Import Lookup Table rva %x", ilt_rva));

	name_rva = read32 (ptr + 12);
	if (!bounds_check_virtual_address (ctx, name_rva, SIZE_OF_MSCOREE))
		ADD_ERROR (ctx, g_strdup_printf ("Invalid Import Table Name rva %x", name_rva));

	iat_rva = read32 (ptr + 16);
	if (!bounds_check_virtual_address (ctx, iat_rva, 8))
		ADD_ERROR (ctx, g_strdup_printf ("Invalid Import Address Table rva %x", iat_rva));

	if (iat_rva != ctx->data_directories [IAT_IDX].rva)
		ADD_ERROR (ctx, g_strdup_printf ("Import Address Table rva %x different from data directory entry %x", read32 (ptr + 16), ctx->data_directories [IAT_IDX].rva));

	name_rva = translate_rva (ctx, name_rva);
	g_assert (name_rva != INVALID_OFFSET);
	ptr = ctx->data + name_rva;

	if (memcmp ("mscoree.dll", ptr, SIZE_OF_MSCOREE)) {
		char name[SIZE_OF_MSCOREE];
		memcpy (name, ptr, SIZE_OF_MSCOREE);
		name [SIZE_OF_MSCOREE - 1] = 0;
		ADD_ERROR (ctx, g_strdup_printf ("Invalid Import Table Name: '%s'", name));
	}
	
	verify_hint_name_table (ctx, ilt_rva, "Import Lookup Table");
	CHECK_ERROR ();
	verify_hint_name_table (ctx, iat_rva, "Import Address Table");
}

static void
verify_resources_table (VerifyContext *ctx)
{
	DataDirectory it = ctx->data_directories [RESOURCE_TABLE_IDX];
	guint32 offset;
	guint16 named_entries, id_entries;
	const char *ptr, *root, *end;

	if (it.rva == 0)
		return;

	if (it.size < 16)
		ADD_ERROR (ctx, g_strdup_printf ("Resource section is too small, must be at least 16 bytes long but it's %d long", it.size));

	offset = it.translated_offset;
	root = ptr = ctx->data + offset;
	end = root + it.size;

	g_assert (offset != INVALID_OFFSET);

	named_entries = read16 (ptr + 12);
	id_entries = read16 (ptr + 14);

	printf ("named %d id_entries %d\n", named_entries, id_entries);
	if ((named_entries + id_entries) * 8 + 16 > it.size)
		ADD_ERROR (ctx, g_strdup_printf ("Resource section is too small, the number of entries (%d) doesn't fit on it's size %d", named_entries + id_entries, it.size));

	if (named_entries || id_entries)
		ADD_ERROR (ctx, g_strdup_printf ("The metadata verifier doesn't support full verification of PECOFF resources"));
}

static void
verify_cli_header (VerifyContext *ctx)
{
	DataDirectory it = ctx->data_directories [CLI_HEADER_IDX];
	guint32 offset;
	const char *ptr;
	int i;

	if (it.rva == 0)
		ADD_ERROR (ctx, g_strdup_printf ("CLI header missing"));

	if (it.size != 72)
		ADD_ERROR (ctx, g_strdup_printf ("Invalid cli header size in data directory %d must be 72", it.size));

	offset = it.translated_offset;
	ptr = ctx->data + offset;

	g_assert (offset != INVALID_OFFSET);

	if (read16 (ptr) != 72)
		ADD_ERROR (ctx, g_strdup_printf ("Invalid cli header size %d must be 72", read16 (ptr)));

	if (!bounds_check_virtual_address (ctx, read32 (ptr + 8), read32 (ptr + 12)))
		ADD_ERROR (ctx, g_strdup_printf ("Invalid medatata section rva/size pair %x/%x", read32 (ptr + 8), read32 (ptr + 12)));

	if (!read32 (ptr + 8) || !read32 (ptr + 12))
		ADD_ERROR (ctx, g_strdup_printf ("Missing medatata section in the CLI header"));

	if ((read32 (ptr + 16) & ~0x0001000B) != 0)
		ADD_ERROR (ctx, g_strdup_printf ("Invalid CLI header flags"));

	ptr += 24;
	for (i = 0; i < 6; ++i) {
		guint32 rva = read32 (ptr);
		guint32 size = read32 (ptr + 4);

		if (rva != 0 && !bounds_check_virtual_address (ctx, rva, size))
			ADD_ERROR (ctx, g_strdup_printf ("Invalid cli section %i rva/size pair %x/%x", i, rva, size));

		ptr += 8;

		if (rva && i > 1)
			ADD_ERROR (ctx, g_strdup_printf ("Metadata verifier doesn't support cli header section %d", i));
	}
}

static guint32
pad4 (guint32 offset)
{
	if (offset & 0x3) //pad to the next 4 byte boundary
		offset = (offset & ~0x3) + 4;
	return offset;
}

static void
verify_metadata_header (VerifyContext *ctx)
{
	int i;
	DataDirectory it = ctx->data_directories [CLI_HEADER_IDX];
	guint32 offset;
	const char *ptr;

	offset = it.translated_offset;
	ptr = ctx->data + offset;
	g_assert (offset != INVALID_OFFSET);

	//build a directory entry for the metadata root
	ptr += 8;
	it.rva = read32 (ptr);
	ptr += 4;
	it.size = read32 (ptr);
	it.translated_offset = offset = translate_rva (ctx, it.rva);

	ptr = ctx->data + offset;
	g_assert (offset != INVALID_OFFSET);

	if (it.size < 20)
		ADD_ERROR (ctx, g_strdup_printf ("Metadata root section is too small %d (at least 20 bytes required for initial decoding)", it.size));

	if (read32 (ptr) != 0x424A5342)
		ADD_ERROR (ctx, g_strdup_printf ("Invalid metadata signature, expected 0x424A5342 but got %08x", read32 (ptr)));

	offset = pad4 (offset + 16 + read32 (ptr + 12));

	if (!bounds_check_offset (&it, offset, 4))
		ADD_ERROR (ctx, g_strdup_printf ("Metadata root section is too small %d (at least %d bytes required for flags decoding)", it.size, offset + 4 - it.translated_offset));

	ptr = ctx->data + offset; //move to streams header 

	if (read16 (ptr + 2) != 5)
		ADD_ERROR (ctx, g_strdup_printf ("Metadata root section have %d streams (it must have exactly 5)", read16 (ptr + 2)));

	ptr += 4;
	offset += 4;

	for (i = 0; i < 5; ++i) {
		guint32 stream_off, stream_size;
		int string_size, stream_idx;

		if (!bounds_check_offset (&it, offset, 8))
			ADD_ERROR (ctx, g_strdup_printf ("Metadata root section is too small for initial decode of stream header %d, missing %d bytes", i, offset + 9 - it.translated_offset));
		
		stream_off = it.translated_offset + read32 (ptr);
		stream_size = read32 (ptr + 4);

		if (!bounds_check_offset (&it,  stream_off, stream_size))
			ADD_ERROR (ctx, g_strdup_printf ("Invalid stream header %d offset/size pair %x/%x", 0, stream_off, stream_size));

		ptr += 8;
		offset += 8;
		
		for (string_size = 0; string_size < 32; ++string_size) {
			if (!bounds_check_offset (&it, offset++, 1))
				ADD_ERROR (ctx, g_strdup_printf ("Metadata root section is too small to decode stream header %d name", i));
			if (!ptr [string_size])
				break;
		}

		if (ptr [string_size])
			ADD_ERROR (ctx, g_strdup_printf ("Metadata stream header %d name larger than 32 bytes", i));

		if (!strncmp ("#Strings", ptr, 9))
			stream_idx = STRINGS_STREAM;
		else if (!strncmp ("#US", ptr, 4))
			stream_idx = USER_STRINGS_STREAM;
		else if (!strncmp ("#Blob", ptr, 6))
			stream_idx = BLOB_STREAM;
		else if (!strncmp ("#GUID", ptr, 6))
			stream_idx = GUID_STREAM;
		else if (!strncmp ("#~", ptr, 3))
			stream_idx = TILDE_STREAM;
		else
			ADD_ERROR (ctx, g_strdup_printf ("Metadata stream header %d invalid name %s", i, ptr));

		if (ctx->metadata_streams [stream_idx].offset != 0)
			ADD_ERROR (ctx, g_strdup_printf ("Duplicated metadata stream header %s", ptr));

		ctx->metadata_streams [stream_idx].offset = stream_off;
		ctx->metadata_streams [stream_idx].offset = stream_size;

		offset = pad4 (offset);
		ptr = ctx->data + offset;
	}
}

GSList*
mono_image_verify (const char *data, guint32 size)
{
	VerifyContext ctx;
	memset (&ctx, 0, sizeof (VerifyContext));
	ctx.data = data;
	ctx.size = size;
	ctx.valid = 1;

	verify_msdos_header (&ctx);
	CHECK_STATE();
	verify_pe_header (&ctx);
	CHECK_STATE();
	verify_pe_optional_header (&ctx);
	CHECK_STATE();
	load_section_table (&ctx);
	CHECK_STATE();
	load_data_directories (&ctx);
	CHECK_STATE();
	verify_import_table (&ctx);
	CHECK_STATE();
	/*No need to check the IAT directory entry, it's content is indirectly verified by verify_import_table*/
	verify_resources_table (&ctx);
	CHECK_STATE();
	verify_cli_header (&ctx);
	CHECK_STATE();
	verify_metadata_header (&ctx);
	CHECK_STATE();
cleanup:
	g_free (ctx.sections);
	return ctx.errors;
}
