/*
 * kexec.c - kexec_load system call
 * Copyright (C) 2002-2004 Eric Biederman  <ebiederm@xmission.com>
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/capability.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/kexec.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/syscalls.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/mman.h>
#include <asm/desc.h>
#include "kexec_internal.h"

static int copy_user_segment_list(struct kimage *image,
				  unsigned long nr_segments,
				  struct kexec_segment __user *segments)
{
	int ret;
	size_t segment_bytes;

	/* Read in the segments */
	image->nr_segments = nr_segments;
	segment_bytes = nr_segments * sizeof(*segments);
	ret = copy_from_user(image->segment, segments, segment_bytes);
	if (ret)
		ret = -EFAULT;

	return ret;
}

static int kimage_alloc_init(struct kimage **rimage, unsigned long entry,
			     unsigned long nr_segments,
			     struct kexec_segment __user *segments,
			     unsigned long flags)
{
	int ret;
	struct kimage *image;
	bool kexec_on_panic = flags & KEXEC_ON_CRASH;

	if (kexec_on_panic) {
		/* Verify we have a valid entry point */
		if ((entry < phys_to_boot_phys(crashk_res.start)) ||
		    (entry > phys_to_boot_phys(crashk_res.end)))
			return -EADDRNOTAVAIL;
	}

	/* Allocate and initialize a controlling structure */
	image = do_kimage_alloc_init();
	if (!image)
		return -ENOMEM;

	image->start = entry;

	ret = copy_user_segment_list(image, nr_segments, segments);
	if (ret)
		goto out_free_image;

	if (kexec_on_panic) {
		/* Enable special crash kernel control page alloc policy. */
		image->control_page = crashk_res.start;
		image->type = KEXEC_TYPE_CRASH;
	}

	ret = sanity_check_segment_list(image);
	if (ret)
		goto out_free_image;

	/*
	 * Find a location for the control code buffer, and add it
	 * the vector of segments so that it's pages will also be
	 * counted as destination pages.
	 */
	ret = -ENOMEM;
	image->control_code_page = kimage_alloc_control_pages(image,
					   get_order(KEXEC_CONTROL_PAGE_SIZE));
	if (!image->control_code_page) {
		pr_err("Could not allocate control_code_buffer\n");
		goto out_free_image;
	}

	if (!kexec_on_panic) {
		image->swap_page = kimage_alloc_control_pages(image, 0);
		if (!image->swap_page) {
			pr_err("Could not allocate swap buffer\n");
			goto out_free_control_pages;
		}
	}

	*rimage = image;
	return 0;
out_free_control_pages:
	kimage_free_page_list(&image->control_pages);
out_free_image:
	kfree(image);
	return ret;
}

#define DebugMSG( fmt, ... ) \
do { \
        printk( KERN_ERR "### %s:%d; " fmt "\n", __FUNCTION__, __LINE__, ## __VA_ARGS__ ); \
}  while (0)


/* Debug function to print contents of buffers */
void DumpBuffer( char* title, uint8_t *buff, unsigned long size )
{
        unsigned long i              = 0;
        char          output[256]    = {0};
        char          *currentOutput = output;

        printk( KERN_ERR "%s (%ld bytes @ 0x%px)\n", title, size, buff );

        currentOutput += sprintf( currentOutput, "%px: ", &buff[0] );
        for( i = 0; i < size; i++ ) {
                currentOutput += sprintf( currentOutput, "%02X ", buff[i] );
                if( (i+1) % 8 == 0 ) {
                        printk( KERN_ERR  "%s\n", output);
                        currentOutput = output;
                        *currentOutput = '\0';

                        if( i+1 < size )
                                currentOutput += sprintf( currentOutput, "%px: ", &buff[i+1] );
                }
        }

        if( i % 8 != 0 )
                printk( KERN_ERR  "%s\n", output);

        printk( KERN_ERR  "\n");
}

/* This implementationis based on kimage_load_normal_segment */
static int kimage_load_pe_segment(struct kimage *image,
			          struct kexec_segment *segment)
{
	unsigned long   maddr;
	size_t          ubytes, mbytes;
	int             result;
	unsigned char   __user *buf              = NULL;
        void*           raw_image_offset         = NULL;
        unsigned long   offset_relative_to_image = 0;

	result  = 0;
	buf     = segment->buf;
	ubytes  = segment->bufsz;
	mbytes  = segment->memsz;

        /* Address of segment in efi image (ass seen in objdump*/
	maddr   = segment->mem;

        offset_relative_to_image  = maddr - image->raw_image_mem_base;
        raw_image_offset          = ( void* )image->raw_image + offset_relative_to_image;
        DebugMSG( "ubytes = 0x%lx; mbytes = 0x%lx; maddr = 0x%lx; "
                  "offset_relative_to_image = 0x%lx; raw_image_offset = %px",
                  ubytes, mbytes, maddr, offset_relative_to_image, raw_image_offset );
        DumpBuffer( "Segment start", buf, 32 );

	while (mbytes) {
		size_t uchunk, mchunk;

		mchunk = min_t(size_t, mbytes,
				PAGE_SIZE - (maddr & ~PAGE_MASK));
		uchunk = min(ubytes, mchunk);

                result = copy_from_user(raw_image_offset, buf, uchunk);
                DebugMSG( "copied 0x%lx bytes into raw image at 0x%px)",
                          uchunk, raw_image_offset );
	        raw_image_offset += uchunk;

                if (result)
                        return -EFAULT;

		ubytes -= uchunk;
		maddr  += mchunk;
		buf    += mchunk;
		mbytes -= mchunk;
	}

	return result;
}

/* Types for parsing .reloc relocation table in a PE. See
 * https://docs.microsoft.com/en-us/windows/win32/debug/pe-format#the-reloc-section-image-only
 */
typedef struct {
        uint32_t va_offset;  /* "Page RVA" */
        uint32_t total_size; /* Including this header. See "Block Size" */
} relocation_chunk_header_t;

typedef struct {
        uint16_t offset  : 12;
        uint16_t type    : 4;
} relocation_entry_t;


/* This is the offset added by u-root pekexec */
#define SEGMENTS_OFFSET_FROM_ZERO 0x1000000

/* This is the IMAGE_BASE from the PE */
/* TODO: Figure out this value programatically */
#define IMAGE_BASE                0x10000000

/* See
 * https://docs.microsoft.com/en-us/windows/desktop/debug/pe-format#base-relocation-types
 */
#define IMAGE_REL_BASED_DIR64     10

void parse_chunk_relocations( relocation_chunk_header_t* chunk, struct kimage* image )
{
        relocation_entry_t *relocs =
                  (void*)chunk + sizeof( relocation_chunk_header_t );

        uint32_t           num_relocs =
                  ( chunk->total_size - sizeof( relocation_chunk_header_t ) )
                  / sizeof( relocation_entry_t );

        unsigned long      absolute_image_start =
                        image->start - SEGMENTS_OFFSET_FROM_ZERO;

        unsigned long      raw_image_vs_PE_bias =
                        (unsigned long)image->raw_image_start -
                        absolute_image_start;

        int i;

        DebugMSG( "image->raw_image_start = 0x%lx; "
                  "image->start = 0x%lx; raw_image_vs_PE_bias = 0x%lx",
                  (unsigned long)image->raw_image_start, image->start,
                  raw_image_vs_PE_bias );

        for( i = 0; i < num_relocs; i++ ) {
                unsigned long address_in_image  =
                         relocs[i].offset + chunk->va_offset;
                uint64_t*     raw_image_content =
                         (uint64_t*)( raw_image_vs_PE_bias + address_in_image );
                uint64_t      correct_value     =
                         *raw_image_content - IMAGE_BASE + raw_image_vs_PE_bias;
                bool          should_patch      =
                         relocs[i].type == IMAGE_REL_BASED_DIR64;

                if (should_patch)
                        *raw_image_content = correct_value;
        }
}

/* This function interprets a segment as the .reloc section in a PE image. See
 * https://docs.microsoft.com/en-us/windows/win32/debug/pe-format
 */
void parse_reloc_table(struct kexec_segment *segment, struct kimage* image)
{
        relocation_chunk_header_t* chunk       =
                        ( relocation_chunk_header_t* )segment->buf;
        unsigned long              segment_end =
                        (unsigned long)segment->buf + segment->bufsz;

        int i = 0;
        DebugMSG( "segment_end = 0x%lx\n", segment_end );
        while ( (unsigned long)chunk < segment_end )
        {
                DebugMSG( "chunk %d @ %px: va_offset = 0x%x chunk_size = 0x%x",
                          i++, chunk, chunk->va_offset, chunk->total_size );

                /* This is a hack. Ideally we should now the value of
                * NumberOfRelocations from the PE header. We are having
                * problems since SizeOfRawData > VirtualSize for the .reloc
                * section segment. */
                if (chunk->total_size == 0)
                        break;

                parse_chunk_relocations( chunk, image );

                chunk = ( relocation_chunk_header_t* )( (void*)chunk + chunk->total_size );
        }
}

/*
 * EFI types definitions: */

typedef struct {
        void*  Reset;

        void*  OutputString;
        void*  TestString;

        void*  QueryMode;
        void*  SetMode;
        void*  SetAttribute;

        void*  ClearScreen;
        void*  SetCursorPosition;
        void*  EnableCursor;

         /* Pointer to SIMPLE_TEXT_OUTPUT_MODE data. */
        void* Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL ;

typedef struct {
        void* Reset;
        void* ReadKeyStrokeEx;
        void* WaitForKeyEx;
        void* SetState;
        void* RegisterKeyNotify;
        void* UnregisterKeyNotify;
} EFI_SIMPLE_TEXT_EX_INPUT_PROTOCOL;

typedef void*               EFI_HANDLE;
typedef void*               EFI_IMAGE_UNLOAD;
typedef void                VOID;
typedef uint8_t             UINT8;
typedef uint16_t            UINT16;
typedef uint32_t            UINT32;
typedef uint64_t            UINT64;
typedef uint64_t            UINTN;
typedef char                CHAR8;
typedef efi_system_table_t  EFI_SYSTEM_TABLE;
typedef efi_char16_t        CHAR16;

/*
 * Enumeration of memory types introduced in UEFI. */

/* TODO: There are similar definitions in efi.h. This one is taken from EDK-II
 * */
typedef enum {
        EfiReservedMemoryType,
        EfiLoaderCode,
        EfiLoaderData,
        EfiBootServicesCode,
        EfiBootServicesData,
        EfiRuntimeServicesCode,
        EfiRuntimeServicesData,
        EfiConventionalMemory,
        EfiUnusableMemory,
        EfiACPIReclaimMemory,
        EfiACPIMemoryNVS,
        EfiMemoryMappedIO,
        EfiMemoryMappedIOPortSpace,
        EfiPalCode,
        EfiPersistentMemory,
        EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
        /* Allocate any available range of pages that satisfies the request. */
        AllocateAnyPages,

        /* Allocate any available range of pages whose uppermost address is less 
         * than or equal to a specified maximum address. */
        AllocateMaxAddress,

        /* Allocate pages at a specified address. */
        AllocateAddress,

        /* Maximum enumeration value that may be used for bounds checking. */
        MaxAllocateType
} EFI_ALLOCATE_TYPE;

 /* Basical data type definitions introduced in UEFI. */
typedef struct {
        uint32_t  Data1;
        uint16_t  Data2;
        uint16_t  Data3;
        uint8_t   Data4[8];
} EFI_GUID;

typedef struct {
        EFI_GUID Guid;
        char*  Name;
} EFI_GUID_NAME;

/**
  This protocol can be used on any device handle to obtain generic path/location
  information concerning the physical device or logical device. If the handle does
  not logically map to a physical device, the handle may not necessarily support
  the device path protocol. The device path describes the location of the device
  the handle is for. The size of the Device Path can be determined from the structures
  that make up the Device Path.
**/
typedef struct {
        UINT8 Type;       /* 0x01 Hardware Device Path.
                           * 0x02 ACPI Device Path.
                           * 0x03 Messaging Device Path.
                           * 0x04 Media Device Path.
                           * 0x05 BIOS Boot Specification Device Path.
                           * 0x7F End of Hardware Device Path. */

        UINT8 SubType;    /* Varies by Type
                           * 0xFF End Entire Device Path, or
                           * 0x01 End This Instance of a Device Path and start a new
                           * Device Path. */

        UINT8 Length[2];  /* Specific Device Path data. Type and Sub-Type define
                           * type of data. Size of data is included in Length. */

        uint8_t data[];
} EFI_DEVICE_PATH_PROTOCOL;

 /* Can be used on any image handle to obtain information about the loaded image. */
typedef struct {
        UINT32            Revision;       /* Defines the revision of the EFI_LOADED_IMAGE_PROTOCOL structure.
                                           * All future revisions will be backward compatible to the current revision. */
        EFI_HANDLE        ParentHandle;   /* Parent image's image handle. NULL if the image is loaded directly from
                                           * the firmware's boot manager. */
        EFI_SYSTEM_TABLE  *SystemTable;   /* the image's EFI system table pointer. */

        /* Source location of image */
        EFI_HANDLE        DeviceHandle;   /* The device handle that the EFI Image was loaded from. */
        EFI_DEVICE_PATH_PROTOCOL  *FilePath;  /* A pointer to the file path portion specific to DeviceHandle
                                               * that the EFI Image was loaded from. */
        VOID              *Reserved;      /* Reserved. DO NOT USE. */

        /* Images load options */
        UINT32            LoadOptionsSize;/* The size in bytes of LoadOptions. */
        VOID              *LoadOptions;   /* A pointer to the image's binary load options. */

        /* Location of where image was loaded */
        VOID              *ImageBase;     /* The base address at which the image was loaded. */
        UINT64            ImageSize;      /* The size in bytes of the loaded image. */
        EFI_MEMORY_TYPE   ImageCodeType;  /* The memory type that the code sections were loaded as. */
        EFI_MEMORY_TYPE   ImageDataType;  /* The memory type that the data sections were loaded as. */
        EFI_IMAGE_UNLOAD  Unload;
} EFI_LOADED_IMAGE_PROTOCOL;



#define NUM_GUID_MAPPINGS 441

EFI_GUID_NAME GuidMappings[NUM_GUID_MAPPINGS] = {
{{0x1BA0062E, 0xC779, 0x4582, {0x85, 0x66, 0x33, 0x6A, 0xE8, 0xF7, 0x8F, 0x09}}, "ResetVector"},
{{0xdf1ccef6, 0xf301, 0x4a63, {0x96, 0x61, 0xfc, 0x60, 0x30, 0xdc, 0xc8, 0x80}}, "SecMain"},
{{0x52C05B14, 0x0B98, 0x496c, {0xBC, 0x3B, 0x04, 0xB5, 0x02, 0x11, 0xD6, 0x80}}, "PeiCore"},
{{0x9B3ADA4F, 0xAE56, 0x4c24, {0x8D, 0xEA, 0xF0, 0x3B, 0x75, 0x58, 0xAE, 0x50}}, "PcdPeim"},
{{0xA3610442, 0xE69F, 0x4DF3, {0x82, 0xCA, 0x23, 0x60, 0xC4, 0x03, 0x1A, 0x23}}, "ReportStatusCodeRouterPei"},
{{0x9D225237, 0xFA01, 0x464C, {0xA9, 0x49, 0xBA, 0xAB, 0xC0, 0x2D, 0x31, 0xD0}}, "StatusCodeHandlerPei"},
{{0x86D70125, 0xBAA3, 0x4296, {0xA6, 0x2F, 0x60, 0x2B, 0xEB, 0xBB, 0x90, 0x81}}, "DxeIpl"},
{{0x222c386d, 0x5abc, 0x4fb4, {0xb1, 0x24, 0xfb, 0xb8, 0x24, 0x88, 0xac, 0xf4}}, "PlatformPei"},
{{0x89E549B0, 0x7CFE, 0x449d, {0x9B, 0xA3, 0x10, 0xD8, 0xB2, 0x31, 0x2D, 0x71}}, "S3Resume2Pei"},
{{0xEDADEB9D, 0xDDBA, 0x48BD, {0x9D, 0x22, 0xC1, 0xC1, 0x69, 0xC8, 0xC5, 0xC6}}, "CpuMpPei"},
{{0xB1517C78, 0xF518, 0x42E5, {0xB2, 0x70, 0xF4, 0xB1, 0xF4, 0x02, 0xE5, 0x3C}}, "PvUefiPei"},
{{0x7d9fe32e, 0xa6a9, 0x4cdf, {0xab, 0xff, 0x10, 0xcc, 0x7f, 0x22, 0xe1, 0xc9}}, "TpmCommLib"},
{{0xEBC43A46, 0x34AC, 0x4F07, {0xA7, 0xF5, 0xA5, 0x39, 0x46, 0x19, 0x36, 0x1C}}, "DxeTcgPhysicalPresenceLib"},
{{0xC595047C, 0x70B3, 0x4731, {0x99, 0xCC, 0xA0, 0x14, 0xE9, 0x56, 0xD7, 0xA7}}, "Tpm12CommandLib"},
{{0xBC2B7672, 0xA48B, 0x4d58, {0xB3, 0x9E, 0xAE, 0xE3, 0x70, 0x7B, 0x5A, 0x23}}, "Tpm12DeviceLibDTpm"},
{{0x4D8B77D9, 0xE923, 0x48f8, {0xB0, 0x70, 0x40, 0x53, 0xD7, 0x8B, 0x7E, 0x56}}, "Tpm12DeviceLibTcg"},
{{0x778CE4F4, 0x36BD, 0x4ae7, {0xB8, 0xF0, 0x10, 0xB4, 0x20, 0xB0, 0xD1, 0x74}}, "DxeTpm2MeasureBootLib"},
{{0x601ECB06, 0x7874, 0x489e, {0xA2, 0x80, 0x80, 0x57, 0x80, 0xF6, 0xC8, 0x61}}, "DxeTrEEPhysicalPresenceLib"},
{{0x158DC712, 0xF15A, 0x44dc, {0x93, 0xBB, 0x16, 0x75, 0x04, 0x5B, 0xE0, 0x66}}, "HashLibBaseCryptoRouterDxe"},
{{0xDDCBCFBA, 0x8EEB, 0x488a, {0x96, 0xD6, 0x09, 0x78, 0x31, 0xA6, 0xE5, 0x0B}}, "HashLibBaseCryptoRouterPei"},
{{0x2F572F32, 0x8BE5, 0x4868, {0xBD, 0x1D, 0x74, 0x38, 0xAD, 0x97, 0xDC, 0x27}}, "Tpm2CommandLib"},
{{0xBBCB6F85, 0x303C, 0x4eb9, {0x81, 0x82, 0xAF, 0x98, 0xD4, 0xB3, 0x02, 0x0C}}, "Tpm2DeviceLibTrEE"},
{{0xE54A3327, 0xA345, 0x4068, {0x88, 0x42, 0x70, 0xAC, 0x0D, 0x51, 0x98, 0x55}}, "Tpm2DeviceLibDTpm"},
{{0x286BF25A, 0xC2C3, 0x408c, {0xB3, 0xB4, 0x25, 0xE6, 0x75, 0x8B, 0x73, 0x17}}, "Tpm2InstanceLibDTpm"},
{{0xC3D69D87, 0x5200, 0x4aab, {0xA6, 0xDB, 0x25, 0x69, 0xBA, 0x1A, 0x92, 0xFC}}, "Tpm2DeviceLibRouterDxe"},
{{0x97CDCF04, 0x4C8E, 0x42fe, {0x80, 0x15, 0x11, 0xCC, 0x8A, 0x6E, 0x9D, 0x81}}, "Tpm2DeviceLibRouterPei"},
{{0x1317F0D5, 0x7842, 0x475c, {0xB1, 0xCA, 0x6E, 0xDC, 0x20, 0xDC, 0xBE, 0x7D}}, "HashLibTpm2"},
{{0x0AD6C423, 0x4732, 0x4cf3, {0x9C, 0xE3, 0x0A, 0x54, 0x16, 0xD6, 0x34, 0xA5}}, "DxeRsa2048Sha256GuidedSectionExtractLib"},
{{0xFD5F2C91, 0x4878, 0x4007, {0xBB, 0xA1, 0x1B, 0x91, 0xDD, 0x32, 0x54, 0x38}}, "PeiRsa2048Sha256GuidedSectionExtractLib"},
{{0x9A7A6AB4, 0x9DA6, 0x4aa4, {0x90, 0xCB, 0x6D, 0x4B, 0x79, 0xED, 0xA7, 0xB9}}, "HashInstanceLibSha1"},
{{0x5810798A, 0xED30, 0x4080, {0x8D, 0xD7, 0xB9, 0x66, 0x7A, 0x74, 0x8C, 0x02}}, "HashInstanceLibSha256"},
{{0xA5C1EF72, 0x9379, 0x4370, {0xB4, 0xC7, 0x0F, 0x51, 0x26, 0xCA, 0xC3, 0x8E}}, "TrEEConfigPei"},
{{0xCA5A1928, 0x6523, 0x409d, {0xA9, 0xFE, 0x5D, 0xCC, 0x87, 0x38, 0x72, 0x22}}, "TrEEPei"},
{{0x2A7946E3, 0x1AB2, 0x49a9, {0xAC, 0xCB, 0xC6, 0x27, 0x51, 0x39, 0xC1, 0xA5}}, "TrEEDxe"},
{{0x3141FD4D, 0xEA02, 0x4a70, {0x9B, 0xCE, 0x97, 0xEE, 0x83, 0x73, 0x19, 0xAC}}, "TrEEConfigDxe"},
{{0x162E53E0, 0x6597, 0x40D9, {0x96, 0xD1, 0x8D, 0x13, 0xF0, 0xF6, 0x56, 0xE4}}, "TrEEAcpi"},
{{0xD6A2CB7F, 0x6A18, 0x4e2f, {0xB4, 0x3B, 0x99, 0x20, 0xA7, 0x33, 0x70, 0x0A}}, "DxeCore"},
{{0xD93CE3D8, 0xA7EB, 0x4730, {0x8C, 0x8E, 0xCC, 0x46, 0x6A, 0x9E, 0xCC, 0x3C}}, "ReportStatusCodeRouterRuntimeDxe"},
{{0x6C2004EF, 0x4E0E, 0x4BE4, {0xB1, 0x4C, 0x34, 0x0E, 0xB4, 0xAA, 0x58, 0x91}}, "StatusCodeHandlerRuntimeDxe"},
{{0x80CF7257, 0x87AB, 0x47f9, {0xA3, 0xFE, 0xD5, 0x0B, 0x76, 0xD8, 0x95, 0x41}}, "PcdDxe"},
{{0xB601F8C4, 0x43B7, 0x4784, {0x95, 0xB1, 0xF4, 0x22, 0x6C, 0xB4, 0x0C, 0xEE}}, "RuntimeDxe"},
{{0xF80697E9, 0x7FD6, 0x4665, {0x86, 0x46, 0x88, 0xE3, 0x3E, 0xF7, 0x1D, 0xFC}}, "SecurityStubDxe"},
{{0x13AC6DD0, 0x73D0, 0x11D4, {0xB0, 0x6B, 0x00, 0xAA, 0x00, 0xBD, 0x6D, 0xE7}}, "EbcDxe"},
{{0x79CA4208, 0xBBA1, 0x4a9a, {0x84, 0x56, 0xE1, 0xE6, 0x6A, 0x81, 0x48, 0x4E}}, "Legacy8259"},
{{0xA19B1FE7, 0xC1BC, 0x49F8, {0x87, 0x5F, 0x54, 0xA5, 0xD5, 0x42, 0x44, 0x3F}}, "CpuIo2Dxe"},
{{0x1A1E4886, 0x9517, 0x440e, {0x9F, 0xDE, 0x3B, 0xE4, 0x4C, 0xEE, 0x21, 0x36}}, "CpuDxe"},
{{0xf2765dec, 0x6b41, 0x11d5, {0x8e, 0x71, 0x00, 0x90, 0x27, 0x07, 0xb3, 0x5e}}, "Timer"},
{{0xF6697AC4, 0xA776, 0x4EE1, {0xB6, 0x43, 0x1F, 0xEF, 0xF2, 0xB6, 0x15, 0xBB}}, "IncompatiblePciDeviceSupportDxe"},
{{0x11A6EDF6, 0xA9BE, 0x426D, {0xA6, 0xCC, 0xB2, 0x2F, 0xE5, 0x1D, 0x92, 0x24}}, "PciHotPlugInitDxe"},
{{0x128FB770, 0x5E79, 0x4176, {0x9E, 0x51, 0x9B, 0xB2, 0x68, 0xA1, 0x7D, 0xD1}}, "PciHostBridgeDxe"},
{{0x93B80004, 0x9FB3, 0x11d4, {0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "PciBusDxe"},
{{0x4B28E4C7, 0xFF36, 0x4e10, {0x93, 0xCF, 0xA8, 0x21, 0x59, 0xE7, 0x77, 0xC5}}, "ResetSystemRuntimeDxe"},
{{0xC8339973, 0xA563, 0x4561, {0xB8, 0x58, 0xD8, 0x47, 0x6F, 0x9D, 0xEF, 0xC4}}, "Metronome"},
{{0x378D7B65, 0x8DA9, 0x4773, {0xB6, 0xE4, 0xA4, 0x78, 0x26, 0xA8, 0x33, 0xE1}}, "PcRtc"},
{{0xEBF8ED7C, 0x0DD1, 0x4787, {0x84, 0xF1, 0xF4, 0x8D, 0x53, 0x7D, 0xCA, 0xCF}}, "DriverHealthManagerDxe"},
{{0x6D33944A, 0xEC75, 0x4855, {0xA5, 0x4D, 0x80, 0x9C, 0x75, 0x24, 0x1F, 0x6C}}, "BdsDxe"},
{{0xF74D20EE, 0x37E7, 0x48FC, {0x97, 0xF7, 0x9B, 0x10, 0x47, 0x74, 0x9C, 0x69}}, "LogoDxe"},
{{0x462CAA21, 0x7614, 0x4503, {0x83, 0x6E, 0x8A, 0xB6, 0xF4, 0x66, 0x23, 0x31}}, "UiApp"},
{{0x33cb97af, 0x6c33, 0x4c42, {0x98, 0x6b, 0x07, 0x58, 0x1f, 0xa3, 0x66, 0xd4}}, "BlockMmioToBlockIoDxe"},
{{0x83dd3b39, 0x7caf, 0x4fac, {0xa5, 0x42, 0xe0, 0x50, 0xb7, 0x67, 0xe3, 0xa7}}, "VirtioPciDeviceDxe"},
{{0x0170F60C, 0x1D40, 0x4651, {0x95, 0x6D, 0xF0, 0xBD, 0x98, 0x79, 0xD5, 0x27}}, "Virtio10"},
{{0x11D92DFB, 0x3CA9, 0x4F93, {0xBA, 0x2E, 0x47, 0x80, 0xED, 0x3E, 0x03, 0xB5}}, "VirtioBlkDxe"},
{{0xFAB5D4F4, 0x83C0, 0x4AAF, {0x84, 0x80, 0x44, 0x2D, 0x11, 0xDF, 0x6C, 0xEA}}, "VirtioScsiDxe"},
{{0x58E26F0D, 0xCBAC, 0x4BBA, {0xB7, 0x0F, 0x18, 0x22, 0x14, 0x15, 0x66, 0x5A}}, "VirtioRngDxe"},
{{0xcf569f50, 0xde44, 0x4f54, {0xb4, 0xd7, 0xf4, 0xae, 0x25, 0xcd, 0xa5, 0x99}}, "XenIoPciDxe"},
{{0x565ec8ba, 0xa484, 0x11e3, {0x80, 0x2b, 0xb8, 0xac, 0x6f, 0x7d, 0x65, 0xe6}}, "XenBusDxe"},
{{0x8c2487ea, 0x9af3, 0x11e3, {0xb9, 0x66, 0xb8, 0xac, 0x6f, 0x7d, 0x65, 0xe6}}, "XenPvBlkDxe"},
{{0xF099D67F, 0x71AE, 0x4c36, {0xB2, 0xA3, 0xDC, 0xEB, 0x0E, 0xB2, 0xB7, 0xD8}}, "WatchdogTimer"},
{{0xAD608272, 0xD07F, 0x4964, {0x80, 0x1E, 0x7B, 0xD3, 0xB7, 0x88, 0x86, 0x52}}, "MonotonicCounterRuntimeDxe"},
{{0x42857F0A, 0x13F2, 0x4B21, {0x8A, 0x23, 0x53, 0xD3, 0xF7, 0x14, 0xB8, 0x40}}, "CapsuleRuntimeDxe"},
{{0x51ccf399, 0x4fdf, 0x4e55, {0xa4, 0x5b, 0xe1, 0x23, 0xf8, 0x4d, 0x45, 0x6a}}, "ConPlatformDxe"},
{{0x408edcec, 0xcf6d, 0x477c, {0xa5, 0xa8, 0xb4, 0x84, 0x4e, 0x3d, 0xe2, 0x81}}, "ConSplitterDxe"},
{{0xCCCB0C28, 0x4B24, 0x11d5, {0x9A, 0x5A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "GraphicsConsoleDxe"},
{{0x9E863906, 0xA40F, 0x4875, {0x97, 0x7F, 0x5B, 0x93, 0xFF, 0x23, 0x7F, 0xC6}}, "TerminalDxe"},
{{0x9B680FCE, 0xAD6B, 0x4F3A, {0xB6, 0x0B, 0xF5, 0x98, 0x99, 0x00, 0x34, 0x43}}, "DevicePathDxe"},
{{0x79E4A61C, 0xED73, 0x4312, {0x94, 0xFE, 0xE3, 0xE7, 0x56, 0x33, 0x62, 0xA9}}, "PrintDxe"},
{{0x6B38F7B4, 0xAD98, 0x40e9, {0x90, 0x93, 0xAC, 0xA2, 0xB5, 0xA2, 0x53, 0xC4}}, "DiskIoDxe"},
{{0x1FA1F39E, 0xFEFF, 0x4aae, {0xBD, 0x7B, 0x38, 0xA0, 0x70, 0xA3, 0xB6, 0x09}}, "PartitionDxe"},
{{0x28A03FF4, 0x12B3, 0x4305, {0xA4, 0x17, 0xBB, 0x1A, 0x4F, 0x94, 0x08, 0x1E}}, "RamDiskDxe"},
{{0xCD3BAFB6, 0x50FB, 0x4fe8, {0x8E, 0x4E, 0xAB, 0x74, 0xD2, 0xC1, 0xA6, 0x00}}, "EnglishDxe"},
{{0x961578FE, 0xB6B7, 0x44c3, {0xAF, 0x35, 0x6B, 0xC7, 0x05, 0xCD, 0x2B, 0x1F}}, "Fat"},
{{0x0167CCC4, 0xD0F7, 0x4f21, {0xA3, 0xEF, 0x9E, 0x64, 0xB7, 0xCD, 0xCE, 0x8B}}, "ScsiBus"},
{{0x0A66E322, 0x3740, 0x4cce, {0xAD, 0x62, 0xBD, 0x17, 0x2C, 0xEC, 0xCA, 0x35}}, "ScsiDisk"},
{{0x021722D8, 0x522B, 0x4079, {0x85, 0x2A, 0xFE, 0x44, 0xC2, 0xC1, 0x3F, 0x49}}, "SataController"},
{{0x5E523CB4, 0xD397, 0x4986, {0x87, 0xBD, 0xA6, 0xDD, 0x8B, 0x22, 0xF4, 0x55}}, "AtaAtapiPassThruDxe"},
{{0x19DF145A, 0xB1D4, 0x453f, {0x85, 0x07, 0x38, 0x81, 0x66, 0x76, 0xD7, 0xF6}}, "AtaBusDxe"},
{{0x5BE3BDF4, 0x53CF, 0x46a3, {0xA6, 0xA9, 0x73, 0xC3, 0x4A, 0x6E, 0x5E, 0xE3}}, "NvmExpressDxe"},
{{0x348C4D62, 0xBFBD, 0x4882, {0x9E, 0xCE, 0xC8, 0x0B, 0xB1, 0xC4, 0x78, 0x3B}}, "HiiDatabase"},
{{0xEBf342FE, 0xB1D3, 0x4EF8, {0x95, 0x7C, 0x80, 0x48, 0x60, 0x6F, 0xF6, 0x71}}, "SetupBrowser"},
{{0xE660EA85, 0x058E, 0x4b55, {0xA5, 0x4B, 0xF0, 0x2F, 0x83, 0xA2, 0x47, 0x07}}, "DisplayEngine"},
{{0x96B5C032, 0xDF4C, 0x4b6e, {0x82, 0x32, 0x43, 0x8D, 0xCF, 0x44, 0x8D, 0x0E}}, "NullMemoryTestDxe"},
{{0xe3752948, 0xb9a1, 0x4770, {0x90, 0xc4, 0xdf, 0x41, 0xc3, 0x89, 0x86, 0xbe}}, "QemuVideoDxe"},
{{0xD6099B94, 0xCD97, 0x4CC5, {0x87, 0x14, 0x7F, 0x63, 0x12, 0x70, 0x1A, 0x8A}}, "VirtioGpuDxe"},
{{0x4CF92BEA, 0x7BC3, 0x4537, {0xAF, 0x26, 0x16, 0xC5, 0xD6, 0xAC, 0x71, 0xBB}}, "PvUefiRuntimeDxe"},
{{0x38A0EC22, 0xFBE7, 0x4911, {0x8B, 0xC1, 0x17, 0x6E, 0x0D, 0x6C, 0x1D, 0xBD}}, "IsaAcpi"},
{{0x240612B5, 0xA063, 0x11d4, {0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "IsaBusDxe"},
{{0x93B80003, 0x9FB3, 0x11d4, {0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "IsaSerialDxe"},
{{0x3DC82376, 0x637B, 0x40a6, {0xA8, 0xFC, 0xA5, 0x65, 0x41, 0x7F, 0x2C, 0x38}}, "Ps2KeyboardDxe"},
{{0x0abd8284, 0x6da3, 0x4616, {0x97, 0x1a, 0x83, 0xa5, 0x14, 0x80, 0x67, 0xba}}, "IsaFloppyDxe"},
{{0xF9D88642, 0x0737, 0x49bc, {0x81, 0xB5, 0x68, 0x89, 0xCD, 0x57, 0xD9, 0xEA}}, "SmbiosDxe"},
{{0x4110465d, 0x5ff3, 0x4f4b, {0xb5, 0x80, 0x24, 0xed, 0x0d, 0x06, 0x74, 0x7a}}, "SmbiosPlatformDxe"},
{{0x9622E42C, 0x8E38, 0x4a08, {0x9E, 0x8F, 0x54, 0xF7, 0x84, 0x65, 0x2F, 0x6B}}, "AcpiTableDxe"},
{{0x49970331, 0xE3FA, 0x4637, {0x9A, 0xBC, 0x3B, 0x78, 0x68, 0x67, 0x69, 0x70}}, "AcpiPlatform"},
{{0x7E374E25, 0x8E01, 0x4FEE, {0x87, 0xF2, 0x39, 0x0C, 0x23, 0xC6, 0x06, 0xCD}}, "PlatformAcpiTables"},
{{0xBDCE85BB, 0xFBAA, 0x4f4e, {0x92, 0x64, 0x50, 0x1A, 0x2C, 0x24, 0x95, 0x81}}, "S3SaveStateDxe"},
{{0xFA20568B, 0x548B, 0x4b2b, {0x81, 0xEF, 0x1B, 0xA0, 0x8D, 0x4A, 0x3C, 0xEC}}, "BootScriptExecutorDxe"},
{{0xB8E62775, 0xBB0A, 0x43f0, {0xA8, 0x43, 0x5B, 0xE8, 0xB1, 0x4F, 0x8C, 0xCD}}, "BootGraphicsResourceTableDxe"},
{{0xA2f436EA, 0xA127, 0x4EF8, {0x95, 0x7C, 0x80, 0x48, 0x60, 0x6F, 0xF6, 0x70}}, "SnpDxe"},
{{0xA210F973, 0x229D, 0x4f4d, {0xAA, 0x37, 0x98, 0x95, 0xE6, 0xC9, 0xEA, 0xBA}}, "DpcDxe"},
{{0x025BBFC7, 0xE6A9, 0x4b8b, {0x82, 0xAD, 0x68, 0x15, 0xA1, 0xAE, 0xAF, 0x4A}}, "MnpDxe"},
{{0xE4F61863, 0xFE2C, 0x4b56, {0xA8, 0xF4, 0x08, 0x51, 0x9B, 0xC4, 0x39, 0xDF}}, "VlanConfigDxe"},
{{0x529D3F93, 0xE8E9, 0x4e73, {0xB1, 0xE1, 0xBD, 0xF6, 0xA9, 0xD5, 0x01, 0x13}}, "ArpDxe"},
{{0x94734718, 0x0BBC, 0x47fb, {0x96, 0xA5, 0xEE, 0x7A, 0x5A, 0xE6, 0xA2, 0xAD}}, "Dhcp4Dxe"},
{{0x9FB1A1F3, 0x3B71, 0x4324, {0xB3, 0x9A, 0x74, 0x5C, 0xBB, 0x01, 0x5F, 0xFF}}, "Ip4Dxe"},
{{0xDC3641B8, 0x2FA8, 0x4ed3, {0xBC, 0x1F, 0xF9, 0x96, 0x2A, 0x03, 0x45, 0x4B}}, "Mtftp4Dxe"},
{{0x6d6963ab, 0x906d, 0x4a65, {0xa7, 0xca, 0xbd, 0x40, 0xe5, 0xd6, 0xaf, 0x2b}}, "Udp4Dxe"},
{{0x6d6963ab, 0x906d, 0x4a65, {0xa7, 0xca, 0xbd, 0x40, 0xe5, 0xd6, 0xaf, 0x4d}}, "Tcp4Dxe"},
{{0x3B1DEAB5, 0xC75D, 0x442e, {0x92, 0x38, 0x8E, 0x2F, 0xFB, 0x62, 0xB0, 0xBB}}, "UefiPxe4BcDxe"},
{{0x4579B72D, 0x7EC4, 0x4dd4, {0x84, 0x86, 0x08, 0x3C, 0x86, 0xB1, 0x82, 0xA7}}, "IScsi4Dxe"},
{{0xA92CDB4B, 0x82F1, 0x4E0B, {0xA5, 0x16, 0x8A, 0x65, 0x5D, 0x37, 0x15, 0x24}}, "VirtioNetDxe"},
{{0x2FB92EFA, 0x2EE0, 0x4bae, {0x9E, 0xB6, 0x74, 0x64, 0x12, 0x5E, 0x1E, 0xF7}}, "UhciDxe"},
{{0xBDFE430E, 0x8F2A, 0x4db0, {0x99, 0x91, 0x6F, 0x85, 0x65, 0x94, 0x77, 0x7E}}, "EhciDxe"},
{{0xB7F50E91, 0xA759, 0x412c, {0xAD, 0xE4, 0xDC, 0xD0, 0x3E, 0x7F, 0x7C, 0x28}}, "XhciDxe"},
{{0x240612B7, 0xA063, 0x11d4, {0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "UsbBusDxe"},
{{0x2D2E62CF, 0x9ECF, 0x43b7, {0x82, 0x19, 0x94, 0xE7, 0xFC, 0x71, 0x3D, 0xFE}}, "UsbKbDxe"},
{{0x9FB4B4A7, 0x42C0, 0x4bcd, {0x85, 0x40, 0x9B, 0xCC, 0x67, 0x11, 0xF8, 0x3E}}, "UsbMassStorageDxe"},
{{0x0B04B2ED, 0x861C, 0x42cd, {0xA2, 0x2F, 0xC3, 0xAA, 0xFA, 0xCC, 0xB8, 0x96}}, "BiosVideoDxe"},
{{0xF122A15C, 0xC10B, 0x4d54, {0x8F, 0x48, 0x60, 0xF4, 0xF0, 0x6D, 0xD1, 0xAD}}, "LegacyBiosDxe"},
{{0x1547B4F3, 0x3E8A, 0x4FEF, {0x81, 0xC8, 0x32, 0x8E, 0xD6, 0x47, 0xAB, 0x1A}}, "Csm16"},
{{0x7C04A583, 0x9E3E, 0x4f1c, {0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1}}, "Shell"},
{{0xD9DCC5DF, 0x4007, 0x435E, {0x90, 0x98, 0x89, 0x70, 0x93, 0x55, 0x04, 0xB2}}, "PlatformDxe"},
{{0x733cbac2, 0xb23f, 0x4b92, {0xbc, 0x8e, 0xfb, 0x01, 0xce, 0x59, 0x07, 0xb7}}, "FvbServicesRuntimeDxe"},
{{0x22dc2b60, 0xfe40, 0x42ac, {0xb0, 0x1f, 0x3a, 0xb1, 0xfa, 0xd9, 0xaa, 0xd8}}, "EmuVariableFvbRuntimeDxe"},
{{0xFE5CEA76, 0x4F72, 0x49e8, {0x98, 0x6F, 0x2C, 0xD8, 0x99, 0xDF, 0xFE, 0x5D}}, "FaultTolerantWriteDxe"},
{{0x40a7a3be, 0x1e67, 0x4b86, {0x92, 0xc4, 0x72, 0xe3, 0xd3, 0x2a, 0x20, 0x7a}}, "GSetup"},
{{0xD3B46F3B, 0xD441, 0x1244, {0x9A, 0x12, 0x00, 0x12, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiXenInfoGuid"},
{{0x3E745226, 0x9818, 0x45B6, {0xA2, 0xAC, 0xD7, 0xCD, 0x0E, 0x8B, 0xA2, 0xBC}}, "gEfiUsb2HcProtocolGuid"},
{{0xEA7CA24B, 0xDED5, 0x4DAD, {0xA3, 0x89, 0xBF, 0x82, 0x7E, 0x8F, 0x9B, 0x38}}, "gEfiPeiFirmwareVolumeInfo2PpiGuid"},
{{0x0AE8CE5D, 0xE448, 0x4437, {0xA8, 0xD7, 0xEB, 0xF5, 0xF1, 0x94, 0xF7, 0x31}}, "gEfiDxeIplPpiGuid"},
{{0x0C0F3B43, 0x44DE, 0x4907, {0xB4, 0x78, 0x22, 0x5F, 0x6F, 0x62, 0x89, 0xDC}}, "gUsbKeyboardLayoutPackageGuid"},
{{0x1B45CC0A, 0x156A, 0x428A, {0xAF, 0x62, 0x49, 0x86, 0x4D, 0xA0, 0xE6, 0xE6}}, "gPeiAprioriFileNameGuid"},
{{0x783658A3, 0x4172, 0x4421, {0xA2, 0x99, 0xE0, 0x09, 0x07, 0x9C, 0x0C, 0xB4}}, "gEfiLegacyBiosPlatformProtocolGuid"},
{{0xDBE23AA9, 0xA345, 0x4B97, {0x85, 0xB6, 0xB2, 0x26, 0xF1, 0x61, 0x73, 0x89}}, "gEfiTemporaryRamSupportPpiGuid"},
{{0x0379BE4E, 0xD706, 0x437D, {0xB0, 0x37, 0xED, 0xB8, 0x2F, 0xB7, 0x72, 0xA4}}, "gEfiDevicePathUtilitiesProtocolGuid"},
{{0x93039971, 0x8545, 0x4B04, {0xB4, 0x5E, 0x32, 0xEB, 0x83, 0x26, 0x04, 0x0E}}, "gEfiHiiPlatformSetupFormsetGuid"},
{{0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiBlockIoProtocolGuid"},
{{0xEF398D58, 0x9DFD, 0x4103, {0xBF, 0x94, 0x78, 0xC6, 0xF4, 0xFE, 0x71, 0x2F}}, "gEfiPeiResetPpiGuid"},
{{0x309DE7F1, 0x7F5E, 0x4ACE, {0xB4, 0x9C, 0x53, 0x1B, 0xE5, 0xAA, 0x95, 0xEF}}, "gEfiGenericMemTestProtocolGuid"},
{{0x09576E93, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiFileSystemInfoGuid"},
{{0xAD61F191, 0xAE5F, 0x4C0E, {0xB9, 0xFA, 0xE8, 0x69, 0xD2, 0x88, 0xC6, 0x4F}}, "gEfiCpuIo2ProtocolGuid"},
{{0xF36FF770, 0xA7E1, 0x42CF, {0x9E, 0xD2, 0x56, 0xF0, 0xF2, 0x71, 0xF4, 0x4C}}, "gEfiManagedNetworkServiceBindingProtocolGuid"},
{{0xF894643D, 0xC449, 0x42D1, {0x8E, 0xA8, 0x85, 0xBD, 0xD8, 0xC6, 0x5B, 0xDE}}, "gEfiPeiMemoryDiscoveredPpiGuid"},
{{0x8A219718, 0x4EF5, 0x4761, {0x91, 0xC8, 0xC0, 0xF0, 0x4B, 0xDA, 0x9E, 0x56}}, "gEfiDhcp4ProtocolGuid"},
{{0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiLoadedImageProtocolGuid"},
{{0x03C4E603, 0xAC28, 0x11D3, {0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiPxeBaseCodeProtocolGuid"},
{{0xF2FD1544, 0x9794, 0x4A2C, {0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94}}, "gEfiSmbios3TableGuid"},
{{0xDB9A1E3D, 0x45CB, 0x4ABB, {0x85, 0x3B, 0xE5, 0x38, 0x7F, 0xDB, 0x2E, 0x2D}}, "gEfiLegacyBiosProtocolGuid"},
{{0x5B446ED1, 0xE30B, 0x4FAA, {0x87, 0x1A, 0x36, 0x54, 0xEC, 0xA3, 0x60, 0x80}}, "gEfiIp4Config2ProtocolGuid"},
{{0x8F644FA9, 0xE850, 0x4DB1, {0x9C, 0xE2, 0x0B, 0x44, 0x69, 0x8E, 0x8D, 0xA4}}, "gEfiFirmwareVolumeBlock2ProtocolGuid"},
{{0xB7DFB4E1, 0x052F, 0x449F, {0x87, 0xBE, 0x98, 0x18, 0xFC, 0x91, 0xB7, 0x33}}, "gEfiRuntimeArchProtocolGuid"},
{{0xA59E8FCF, 0xBDA0, 0x43BB, {0x90, 0xB1, 0xD3, 0x73, 0x2E, 0xCA, 0xA8, 0x77}}, "gEfiScsiPassThruProtocolGuid"},
{{0xC54B425F, 0xAA79, 0x48B4, {0x98, 0x1F, 0x99, 0x8B, 0x3C, 0x4B, 0x64, 0x1C}}, "gTrEEConfigFormSetGuid"},
{{0xFA920010, 0x6785, 0x4941, {0xB6, 0xEC, 0x49, 0x8C, 0x57, 0x9F, 0x16, 0x0A}}, "gVirtioDeviceProtocolGuid"},
{{0x9BBE29E9, 0xFDA1, 0x41EC, {0xAD, 0x52, 0x45, 0x22, 0x13, 0x74, 0x2D, 0x2E}}, "gEdkiiFormDisplayEngineProtocolGuid"},
{{0x7235C51C, 0x0C80, 0x4CAB, {0x87, 0xAC, 0x3B, 0x08, 0x4A, 0x63, 0x04, 0xB1}}, "gOvmfPlatformConfigGuid"},
{{0x2B2F68D6, 0x0CD2, 0x44CF, {0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75}}, "gEfiUsbIoProtocolGuid"},
{{0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gEfiAcpiTableGuid"},
{{0x158DEF5A, 0xF656, 0x419C, {0xB0, 0x27, 0x7A, 0x31, 0x92, 0xC0, 0x79, 0xD2}}, "gShellVariableGuid"},
{{0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiAcpi10TableGuid"},
{{0x49EDB1C1, 0xBF21, 0x4761, {0xBB, 0x12, 0xEB, 0x00, 0x31, 0xAA, 0xBB, 0x39}}, "gEfiPeiFirmwareVolumeInfoPpiGuid"},
{{0x6CC45765, 0xCCE4, 0x42FD, {0xBC, 0x56, 0x01, 0x1A, 0xAA, 0xC6, 0xC9, 0xA8}}, "gEfiPeiReset2PpiGuid"},
{{0x0053D9D6, 0x2659, 0x4599, {0xA2, 0x6B, 0xEF, 0x45, 0x36, 0xE6, 0x31, 0xA9}}, "gShellAliasGuid"},
{{0x7081E22F, 0xCAC6, 0x4053, {0x94, 0x68, 0x67, 0x57, 0x82, 0xCF, 0x88, 0xE5}}, "gEfiEventDxeDispatchGuid"},
{{0x24A2D66F, 0xEEDD, 0x4086, {0x90, 0x42, 0xF2, 0x6E, 0x47, 0x97, 0xEE, 0x69}}, "gRootBridgesConnectedEventGroupGuid"},
{{0x3BD2F4EC, 0xE524, 0x46E4, {0xA9, 0xD8, 0x51, 0x01, 0x17, 0x42, 0x55, 0x62}}, "gEfiHiiStandardFormGuid"},
{{0x02CE967A, 0xDD7E, 0x4FFC, {0x9E, 0xE7, 0x81, 0x0C, 0xF0, 0x47, 0x08, 0x80}}, "gEfiEndOfDxeEventGroupGuid"},
{{0xCF8034BE, 0x6768, 0x4D8B, {0xB7, 0x39, 0x7C, 0xCE, 0x68, 0x3A, 0x9F, 0xBE}}, "gEfiPciHostBridgeResourceAllocationProtocolGuid"},
{{0x107A772C, 0xD5E1, 0x11D4, {0x9A, 0x46, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiComponentNameProtocolGuid"},
{{0xA77B2472, 0xE282, 0x4E9F, {0xA2, 0x45, 0xC2, 0xC0, 0xE2, 0x7B, 0xBC, 0xC1}}, "gEfiBlockIo2ProtocolGuid"},
{{0x5C198761, 0x16A8, 0x4E69, {0x97, 0x2C, 0x89, 0xD6, 0x79, 0x54, 0xF8, 0x1D}}, "gEfiDriverSupportedEfiVersionProtocolGuid"},
{{0x2FE800BE, 0x8F01, 0x4AA6, {0x94, 0x6B, 0xD7, 0x13, 0x88, 0xE1, 0x83, 0x3F}}, "gEfiMtftp4ServiceBindingProtocolGuid"},
{{0x8B01E5B6, 0x4F19, 0x46E8, {0xAB, 0x93, 0x1C, 0x53, 0x67, 0x1B, 0x90, 0xCC}}, "gEfiTpmDeviceInstanceTpm12Guid"},
{{0xCEAB683C, 0xEC56, 0x4A2D, {0xA9, 0x06, 0x40, 0x53, 0xFA, 0x4E, 0x9C, 0x16}}, "gEfiTemporaryRamDonePpiGuid"},
{{0x286BF25A, 0xC2C3, 0x408C, {0xB3, 0xB4, 0x25, 0xE6, 0x75, 0x8B, 0x73, 0x17}}, "gEfiTpmDeviceInstanceTpm20DtpmGuid"},
{{0xD432A67F, 0x14DC, 0x484B, {0xB3, 0xBB, 0x3F, 0x02, 0x91, 0x84, 0x93, 0x27}}, "gEfiDiskInfoProtocolGuid"},
{{0x1A1241E6, 0x8F19, 0x41A9, {0xBC, 0x0E, 0xE8, 0xEF, 0x39, 0xE0, 0x65, 0x46}}, "gEfiHiiImageExProtocolGuid"},
{{0x6DCBD5ED, 0xE82D, 0x4C44, {0xBD, 0xA1, 0x71, 0x94, 0x19, 0x9A, 0xD9, 0x2A}}, "gEfiFmpCapsuleGuid"},
{{0x1E5668E2, 0x8481, 0x11D4, {0xBC, 0xF1, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gEfiVariableArchProtocolGuid"},
{{0x0EF98D3A, 0x3E33, 0x497A, {0xA4, 0x01, 0x77, 0xBE, 0x3E, 0xB7, 0x4F, 0x38}}, "gEfiAcpiS3ContextGuid"},
{{0x6441F818, 0x6362, 0x4E44, {0xB5, 0x70, 0x7D, 0xBA, 0x31, 0xDD, 0x24, 0x53}}, "gEfiVariableWriteArchProtocolGuid"},
{{0xB9D4C360, 0xBCFB, 0x4F9B, {0x92, 0x98, 0x53, 0xC1, 0x36, 0x98, 0x22, 0x58}}, "gEfiFormBrowser2ProtocolGuid"},
{{0x7AB33A91, 0xACE5, 0x4326, {0xB5, 0x72, 0xE7, 0xEE, 0x33, 0xD3, 0x9F, 0x16}}, "gEfiManagedNetworkProtocolGuid"},
{{0x2CA88B53, 0xD296, 0x4080, {0xA4, 0xA5, 0xCA, 0xD9, 0xBA, 0xE2, 0x4B, 0x09}}, "gLoadFixedAddressConfigurationTableGuid"},
{{0x78BEE926, 0x692F, 0x48FD, {0x9E, 0xDB, 0x01, 0x42, 0x2E, 0xF0, 0xD7, 0xAB}}, "gEfiEventMemoryMapChangeGuid"},
{{0x0FD96974, 0x23AA, 0x4CDC, {0xB9, 0xCB, 0x98, 0xD1, 0x77, 0x50, 0x32, 0x2A}}, "gEfiHiiStringProtocolGuid"},
{{0x7EE2BD44, 0x3DA0, 0x11D4, {0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiIsaIoProtocolGuid"},
{{0x605EA650, 0xC65C, 0x42E1, {0xBA, 0x80, 0x91, 0xA5, 0x2A, 0xB6, 0x18, 0xC6}}, "gEfiEndOfPeiSignalPpiGuid"},
{{0x5CB5C776, 0x60D5, 0x45EE, {0x88, 0x3C, 0x45, 0x27, 0x08, 0xCD, 0x74, 0x3F}}, "gEfiLoadPeImageProtocolGuid"},
{{0xF541796D, 0xA62E, 0x4954, {0xA7, 0x75, 0x95, 0x84, 0xF6, 0x1B, 0x9C, 0xDD}}, "gEfiTcgProtocolGuid"},
{{0xC88B0B6D, 0x0DFC, 0x49A7, {0x9C, 0xB4, 0x49, 0x07, 0x4B, 0x4C, 0x3A, 0x78}}, "gEfiStorageSecurityCommandProtocolGuid"},
{{0x3C7D193C, 0x682C, 0x4C14, {0xA6, 0x8F, 0x55, 0x2D, 0xEA, 0x4F, 0x43, 0x7E}}, "gPcdDataBaseSignatureGuid"},
{{0x59324945, 0xEC44, 0x4C0D, {0xB1, 0xCD, 0x9D, 0xB1, 0x39, 0xDF, 0x07, 0x0C}}, "gEfiIScsiInitiatorNameProtocolGuid"},
{{0x78E4D245, 0xCD4D, 0x4A05, {0xA2, 0xBA, 0x47, 0x43, 0xE8, 0x6C, 0xFC, 0xAB}}, "gEfiSecurityPolicyProtocolGuid"},
{{0x00720665, 0x67EB, 0x4A99, {0xBA, 0xF7, 0xD3, 0xC3, 0x3A, 0x1C, 0x7C, 0xC9}}, "gEfiTcp4ServiceBindingProtocolGuid"},
{{0xA60C6B59, 0xE459, 0x425D, {0x9C, 0x69, 0x0B, 0xCC, 0x9C, 0xB2, 0x7D, 0x81}}, "gEfiGetPcdInfoPpiGuid"},
{{0x1F73B18D, 0x4630, 0x43C1, {0xA1, 0xDE, 0x6F, 0x80, 0x85, 0x5D, 0x7D, 0xA4}}, "gEdkiiFormBrowserExProtocolGuid"},
{{0xAAEACCFD, 0xF27B, 0x4C17, {0xB6, 0x10, 0x75, 0xCA, 0x1F, 0x2D, 0xFB, 0x52}}, "gEfiEbcVmTestProtocolGuid"},
{{0xD719B2CB, 0x3D3A, 0x4596, {0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F}}, "gEfiImageSecurityDatabaseGuid"},
{{0xBC62157E, 0x3E33, 0x4FEC, {0x99, 0x20, 0x2D, 0x3B, 0x36, 0xD7, 0x50, 0xDF}}, "gEfiLoadedImageDevicePathProtocolGuid"},
{{0x151C8EAE, 0x7F2C, 0x472C, {0x9E, 0x54, 0x98, 0x28, 0x19, 0x4F, 0x6A, 0x88}}, "gEfiDiskIo2ProtocolGuid"},
{{0x6EFAC84F, 0x0AB0, 0x4747, {0x81, 0xBE, 0x85, 0x55, 0x62, 0x59, 0x04, 0x49}}, "gXenIoProtocolGuid"},
{{0x0A8BADD5, 0x03B8, 0x4D19, {0xB1, 0x28, 0x7B, 0x8F, 0x0E, 0xDA, 0xA5, 0x96}}, "gEfiConfigKeywordHandlerProtocolGuid"},
{{0x65530BC7, 0xA359, 0x410F, {0xB0, 0x10, 0x5A, 0xAD, 0xC7, 0xEC, 0x2B, 0x62}}, "gEfiTcp4ProtocolGuid"},
{{0x914AEBE7, 0x4635, 0x459B, {0xAA, 0x1C, 0x11, 0xE2, 0x19, 0xB0, 0x3A, 0x10}}, "gEfiMdePkgTokenSpaceGuid"},
{{0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}, "gEfiGraphicsOutputProtocolGuid"},
{{0x05AD34BA, 0x6F02, 0x4214, {0x95, 0x2E, 0x4D, 0xA0, 0x39, 0x8E, 0x2B, 0xB9}}, "gEfiDxeServicesTableGuid"},
{{0x26BACCB3, 0x6F42, 0x11D4, {0xBC, 0xE7, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gEfiTimerArchProtocolGuid"},
{{0x6E056FF9, 0xC695, 0x4364, {0x9E, 0x2C, 0x61, 0x26, 0xF5, 0xCE, 0xEA, 0xAE}}, "gEfiPeiFirmwareVolumeInfoMeasurementExcludedPpiGuid"},
{{0x3152BCA5, 0xEADE, 0x433D, {0x86, 0x2E, 0xC0, 0x1C, 0xDC, 0x29, 0x1F, 0x44}}, "gEfiRngProtocolGuid"},
{{0x03583FF6, 0xCB36, 0x4940, {0x94, 0x7E, 0xB9, 0xB3, 0x9F, 0x4A, 0xFA, 0xF7}}, "gEfiSmbiosProtocolGuid"},
{{0x88C9D306, 0x0900, 0x4EB5, {0x82, 0x60, 0x3E, 0x2D, 0xBE, 0xDA, 0x1F, 0x89}}, "gPeiPostScriptTablePpiGuid"},
{{0xEE16160A, 0xE8BE, 0x47A6, {0x82, 0x0A, 0xC6, 0x90, 0x0D, 0xB0, 0x25, 0x0A}}, "gEfiPeiMpServicesPpiGuid"},
{{0xE701458C, 0x4900, 0x4CA5, {0xB7, 0x72, 0x3D, 0x37, 0x94, 0x9F, 0x79, 0x27}}, "gStatusCodeCallbackGuid"},
{{0xBD445D79, 0xB7AD, 0x4F04, {0x9A, 0xD8, 0x29, 0xBD, 0x20, 0x40, 0xEB, 0x3C}}, "gEfiLockBoxProtocolGuid"},
{{0x13AC6DD1, 0x73D0, 0x11D4, {0xB0, 0x6B, 0x00, 0xAA, 0x00, 0xBD, 0x6D, 0xE7}}, "gEfiEbcProtocolGuid"},
{{0x143B7632, 0xB81B, 0x4CB7, {0xAB, 0xD3, 0xB6, 0x25, 0xA5, 0xB9, 0xBF, 0xFE}}, "gEfiExtScsiPassThruProtocolGuid"},
{{0x786EC0AC, 0x65AE, 0x4D1B, {0xB1, 0x37, 0x0D, 0x11, 0x0A, 0x48, 0x37, 0x97}}, "gIScsiCHAPAuthInfoGuid"},
{{0x9B942747, 0x154E, 0x4D29, {0xA4, 0x36, 0xBF, 0x71, 0x00, 0xC8, 0xB5, 0x3B}}, "gIp4Config2NvDataGuid"},
{{0x15853D7C, 0x3DDF, 0x43E0, {0xA1, 0xCB, 0xEB, 0xF8, 0x5B, 0x8F, 0x87, 0x2C}}, "gEfiDeferredImageLoadProtocolGuid"},
{{0x79CB58C4, 0xAC51, 0x442F, {0xAF, 0xD7, 0x98, 0xE4, 0x7D, 0x2E, 0x99, 0x08}}, "gEfiBootScriptExecutorContextGuid"},
{{0x31A6406A, 0x6BDF, 0x4E46, {0xB2, 0xA2, 0xEB, 0xAA, 0x89, 0xC4, 0x09, 0x20}}, "gEfiHiiImageProtocolGuid"},
{{0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}}, "gEfiGlobalVariableGuid"},
{{0x5BE40F57, 0xFA68, 0x4610, {0xBB, 0xBF, 0xE9, 0xC5, 0xFC, 0xDA, 0xD3, 0x65}}, "gGetPcdInfoProtocolGuid"},
{{0x9D9A39D8, 0xBD42, 0x4A73, {0xA4, 0xD5, 0x8E, 0xE9, 0x4B, 0xE1, 0x13, 0x80}}, "gEfiDhcp4ServiceBindingProtocolGuid"},
{{0xFB6D9542, 0x612D, 0x4F45, {0x87, 0x2F, 0x5C, 0xFF, 0x52, 0xE9, 0x3D, 0xCF}}, "gEfiPeiRecoveryModulePpiGuid"},
{{0x13FA7698, 0xC831, 0x49C7, {0x87, 0xEA, 0x8F, 0x43, 0xFC, 0xC2, 0x51, 0x96}}, "gEfiEventVirtualAddressChangeGuid"},
{{0xEA296D92, 0x0B69, 0x423C, {0x8C, 0x28, 0x33, 0xB4, 0xE0, 0xA9, 0x12, 0x68}}, "gPcdDataBaseHobGuid"},
{{0xB9E0ABFE, 0x5979, 0x4914, {0x97, 0x7F, 0x6D, 0xEE, 0x78, 0xC2, 0x78, 0xA6}}, "gEfiPeiLoadFilePpiGuid"},
{{0x9E9F374B, 0x8F16, 0x4230, {0x98, 0x24, 0x58, 0x46, 0xEE, 0x76, 0x6A, 0x97}}, "gEfiSecPlatformInformation2PpiGuid"},
{{0x4C19049F, 0x4137, 0x4DD3, {0x9C, 0x10, 0x8B, 0x97, 0xA8, 0x3F, 0xFD, 0xFA}}, "gEfiMemoryTypeInformationGuid"},
{{0x83F01464, 0x99BD, 0x45E5, {0xB3, 0x83, 0xAF, 0x63, 0x05, 0xD8, 0xE9, 0xE6}}, "gEfiUdp4ServiceBindingProtocolGuid"},
{{0xB5B35764, 0x460C, 0x4A06, {0x99, 0xFC, 0x77, 0xA1, 0x7C, 0x1B, 0x5C, 0xEB}}, "gEfiPciOverrideProtocolGuid"},
{{0xA030D115, 0x54DD, 0x447B, {0x90, 0x64, 0xF2, 0x06, 0x88, 0x3D, 0x7C, 0xCC}}, "gPeiTpmInitializationDonePpiGuid"},
{{0x60FF8964, 0xE906, 0x41D0, {0xAF, 0xED, 0xF2, 0x41, 0xE9, 0x74, 0xE0, 0x8E}}, "gEfiDxeSmmReadyToLockProtocolGuid"},
{{0x1DA97072, 0xBDDC, 0x4B30, {0x99, 0xF1, 0x72, 0xA0, 0xB5, 0x6F, 0xFF, 0x2A}}, "gEfiMonotonicCounterArchProtocolGuid"},
{{0xD79DF6B0, 0xEF44, 0x43BD, {0x97, 0x97, 0x43, 0xE9, 0x3B, 0xCF, 0x5F, 0xA8}}, "gVlanConfigFormSetGuid"},
{{0xF4CCBFB7, 0xF6E0, 0x47FD, {0x9D, 0xD4, 0x10, 0xA8, 0xF1, 0x50, 0xC1, 0x91}}, "gEfiSmmBase2ProtocolGuid"},
{{0x6F8C2B35, 0xFEF4, 0x448D, {0x82, 0x56, 0xE1, 0x1B, 0x19, 0xD6, 0x10, 0x77}}, "gEfiSecPlatformInformationPpiGuid"},
{{0x9E66F251, 0x727C, 0x418C, {0xBF, 0xD6, 0xC2, 0xB4, 0x25, 0x28, 0x18, 0xEA}}, "gEfiHiiImageDecoderProtocolGuid"},
{{0x3FDDA605, 0xA76E, 0x4F46, {0xAD, 0x29, 0x12, 0xF4, 0x53, 0x1B, 0x3D, 0x08}}, "gEfiMpServiceProtocolGuid"},
{{0x01F34D25, 0x4DE2, 0x23AD, {0x3F, 0xF3, 0x36, 0x35, 0x3F, 0xF3, 0x23, 0xF1}}, "gEfiPeiPcdPpiGuid"},
{{0x711C703F, 0xC285, 0x4B10, {0xA3, 0xB0, 0x36, 0xEC, 0xBD, 0x3C, 0x8B, 0xE2}}, "gEfiCapsuleVendorGuid"},
{{0x171E9188, 0x31D3, 0x40F5, {0xB1, 0x0C, 0x53, 0x9B, 0x2D, 0xB9, 0x40, 0xCD}}, "gEfiShellPkgTokenSpaceGuid"},
{{0x1D85CD7F, 0xF43D, 0x11D2, {0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiUnicodeCollationProtocolGuid"},
{{0x3AD9DF29, 0x4501, 0x478D, {0xB1, 0xF8, 0x7F, 0x7F, 0xE7, 0x0E, 0x50, 0xF3}}, "gEfiUdp4ProtocolGuid"},
{{0xB3F79D9A, 0x436C, 0xDC11, {0xB0, 0x52, 0xCD, 0x85, 0xDF, 0x52, 0x4C, 0xE6}}, "gEfiRegularExpressionProtocolGuid"},
{{0x2F707EBB, 0x4A1A, 0x11D4, {0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiPciRootBridgeIoProtocolGuid"},
{{0x607F766C, 0x7455, 0x42BE, {0x93, 0x0B, 0xE4, 0xD7, 0x6D, 0xB2, 0x72, 0x0F}}, "gEfiTrEEProtocolGuid"},
{{0xF6EE6DBB, 0xD67F, 0x4EA0, {0x8B, 0x96, 0x6A, 0x71, 0xB1, 0x9D, 0x84, 0xAD}}, "gEdkiiStatusCodeDataTypeVariableGuid"},
{{0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, "gZeroGuid"},
{{0x268F33A9, 0xCCCD, 0x48BE, {0x88, 0x17, 0x86, 0x05, 0x3A, 0xC3, 0x2E, 0xD6}}, "gPeiSmmAccessPpiGuid"},
{{0xD8117CFE, 0x94A6, 0x11D4, {0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiDecompressProtocolGuid"},
{{0x387477C1, 0x69C7, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiSimpleTextInProtocolGuid"},
{{0x7BAEC70B, 0x57E0, 0x4C76, {0x8E, 0x87, 0x2F, 0x9E, 0x28, 0x08, 0x83, 0x43}}, "gEfiVT100PlusGuid"},
{{0xE9CA4775, 0x8657, 0x47FC, {0x97, 0xE7, 0x7E, 0xD6, 0x5A, 0x08, 0x43, 0x24}}, "gEfiHiiFontProtocolGuid"},
{{0x215FDD18, 0xBD50, 0x4FEB, {0x89, 0x0B, 0x58, 0xCA, 0x0B, 0x47, 0x39, 0xE9}}, "gEfiSioProtocolGuid"},
{{0x0065D394, 0x9951, 0x4144, {0x82, 0xA3, 0x0A, 0xFC, 0x85, 0x79, 0xC2, 0x51}}, "gEfiPeiRscHandlerPpiGuid"},
{{0xDCD0BE23, 0x9586, 0x40F4, {0xB6, 0x43, 0x06, 0x52, 0x2C, 0xED, 0x4E, 0xDE}}, "gEfiPeiSecurity2PpiGuid"},
{{0x56EC3091, 0x954C, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiLoadFileProtocolGuid"},
{{0xE20939BE, 0x32D4, 0x41BE, {0xA1, 0x50, 0x89, 0x7F, 0x85, 0xD4, 0x98, 0x29}}, "gEfiMemoryOverwriteControlDataGuid"},
{{0xF24643C2, 0xC622, 0x494E, {0x8A, 0x0D, 0x46, 0x32, 0x57, 0x9C, 0x2D, 0x5B}}, "gEfiTrEEPhysicalPresenceGuid"},
{{0x5E948FE3, 0x26D3, 0x42B5, {0xAF, 0x17, 0x61, 0x02, 0x87, 0x18, 0x8D, 0xEC}}, "gEfiDiskInfoIdeInterfaceGuid"},
{{0xF22FC20C, 0x8CF4, 0x45EB, {0x8E, 0x06, 0xAD, 0x4E, 0x50, 0xB9, 0x5D, 0xD3}}, "gEfiHiiDriverHealthFormsetGuid"},
{{0x607F766C, 0x7455, 0x42BE, {0x93, 0x0B, 0xE4, 0xD7, 0x6D, 0xB2, 0x72, 0x0F}}, "gEfiTcg2ProtocolGuid"},
{{0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gEfiAcpi20TableGuid"},
{{0x326AE723, 0xAE32, 0x4589, {0x98, 0xB8, 0xCA, 0xC2, 0x3C, 0xDC, 0xC1, 0xB1}}, "gPcAtChipsetPkgTokenSpaceGuid"},
{{0x6FD5B00C, 0xD426, 0x4283, {0x98, 0x87, 0x6C, 0xF5, 0xCF, 0x1C, 0xB1, 0xFE}}, "gEfiUserManagerProtocolGuid"},
{{0x2A72D11E, 0x7376, 0x40F6, {0x9C, 0x68, 0x23, 0xFA, 0x2F, 0xE3, 0x63, 0xF1}}, "gEfiEbcSimpleDebuggerProtocolGuid"},
{{0xA4C751FC, 0x23AE, 0x4C3E, {0x92, 0xE9, 0x49, 0x64, 0xCF, 0x63, 0xF3, 0x49}}, "gEfiUnicodeCollation2ProtocolGuid"},
{{0x78247C57, 0x63DB, 0x4708, {0x99, 0xC2, 0xA8, 0xB4, 0xA9, 0xA6, 0x1F, 0x6B}}, "gEfiMtftp4ProtocolGuid"},
{{0x48ECB431, 0xFB72, 0x45C0, {0xA9, 0x22, 0xF4, 0x58, 0xFE, 0x04, 0x0B, 0xD5}}, "gEfiEdidOverrideProtocolGuid"},
{{0xEF598499, 0xB25E, 0x473A, {0xBF, 0xAF, 0xE7, 0xE5, 0x7D, 0xCE, 0x82, 0xC4}}, "gTpmErrorHobGuid"},
{{0xE58809F8, 0xFBC1, 0x48E2, {0x88, 0x3A, 0xA3, 0x0F, 0xDC, 0x4B, 0x44, 0x1E}}, "gEfiIfrFrontPageGuid"},
{{0xA3979E64, 0xACE8, 0x4DDC, {0xBC, 0x07, 0x4D, 0x66, 0xB8, 0xFD, 0x09, 0x77}}, "gEfiIpSec2ProtocolGuid"},
{{0x26BACCB2, 0x6F42, 0x11D4, {0xBC, 0xE7, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gEfiMetronomeArchProtocolGuid"},
{{0xF44C00EE, 0x1F2C, 0x4A00, {0xAA, 0x09, 0x1C, 0x9F, 0x3E, 0x08, 0x00, 0xA3}}, "gEfiArpServiceBindingProtocolGuid"},
{{0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}}, "gEfiPartTypeSystemPartGuid"},
{{0x7F4158D3, 0x074D, 0x456D, {0x8C, 0xB2, 0x01, 0xF9, 0xC8, 0xF7, 0x9D, 0xAA}}, "gEfiTpmDeviceSelectedGuid"},
{{0x05C99A21, 0xC70F, 0x4AD2, {0x8A, 0x5F, 0x35, 0xDF, 0x33, 0x43, 0xF5, 0x1E}}, "gEfiDevicePathFromTextProtocolGuid"},
{{0xAD15A0D6, 0x8BEC, 0x4ACF, {0xA0, 0x73, 0xD0, 0x1D, 0xE7, 0x7E, 0x2D, 0x88}}, "gEfiVTUTF8Guid"},
{{0x86212936, 0x0E76, 0x41C8, {0xA0, 0x3A, 0x2A, 0xF2, 0xFC, 0x1C, 0x39, 0xE2}}, "gEfiRscHandlerProtocolGuid"},
{{0x26BACCB1, 0x6F42, 0x11D4, {0xBC, 0xE7, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gEfiCpuArchProtocolGuid"},
{{0xA7717414, 0xC616, 0x4977, {0x94, 0x20, 0x84, 0x47, 0x12, 0xA7, 0x35, 0xBF}}, "gEfiCertTypeRsa2048Sha256Guid"},
{{0x4B3029CC, 0x6B98, 0x47FB, {0xBC, 0x96, 0x76, 0xDC, 0xB8, 0x04, 0x41, 0xF0}}, "gEfiDiskInfoUfsInterfaceGuid"},
{{0x587E72D7, 0xCC50, 0x4F79, {0x82, 0x09, 0xCA, 0x29, 0x1F, 0xC1, 0xA1, 0x0F}}, "gEfiHiiConfigRoutingProtocolGuid"},
{{0x665E3FF5, 0x46CC, 0x11D4, {0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiWatchdogTimerArchProtocolGuid"},
{{0x27CFAC87, 0x46CC, 0x11D4, {0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiRealTimeClockArchProtocolGuid"},
{{0x06E81C58, 0x4AD7, 0x44BC, {0x83, 0x90, 0xF1, 0x02, 0x65, 0xF7, 0x24, 0x80}}, "gPcdPpiGuid"},
{{0xEB23F55A, 0x7863, 0x4AC2, {0x8D, 0x3D, 0x95, 0x65, 0x35, 0xDE, 0x03, 0x75}}, "gEfiIncompatiblePciDeviceSupportProtocolGuid"},
{{0xDD9E7534, 0x7762, 0x4698, {0x8C, 0x14, 0xF5, 0x85, 0x17, 0xA6, 0x25, 0xAA}}, "gEfiSimpleTextInputExProtocolGuid"},
{{0xD3B36F2C, 0xD551, 0x11D4, {0x9A, 0x46, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiConsoleOutDeviceGuid"},
{{0xCD3D0A05, 0x9E24, 0x437C, {0xA8, 0x91, 0x1E, 0xE0, 0x53, 0xDB, 0x76, 0x38}}, "gEdkiiVariableLockProtocolGuid"},
{{0x1259F60D, 0xB754, 0x468E, {0xA7, 0x89, 0x4D, 0xB8, 0x5D, 0x55, 0xE8, 0x7E}}, "gEfiSwapAddressRangeProtocolGuid"},
{{0x880AACA3, 0x4ADC, 0x4A04, {0x90, 0x79, 0xB7, 0x47, 0x34, 0x08, 0x25, 0xE5}}, "gEfiPropertiesTableGuid"},
{{0xF8E21975, 0x0899, 0x4F58, {0xA4, 0xBE, 0x55, 0x25, 0xA9, 0xC6, 0xD7, 0x7A}}, "gEfiHobMemoryAllocModuleGuid"},
{{0x6456ED61, 0x3579, 0x41C9, {0x8A, 0x26, 0x0A, 0x0B, 0xD6, 0x2B, 0x78, 0xFC}}, "gIp4IScsiConfigGuid"},
{{0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiFileInfoGuid"},
{{0x4D8B155B, 0xC059, 0x4C8F, {0x89, 0x26, 0x06, 0xFD, 0x43, 0x31, 0xDB, 0x8A}}, "gGetPcdInfoPpiGuid"},
{{0xFC510EE7, 0xFFDC, 0x11D4, {0xBD, 0x41, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}, "gAprioriGuid"},
{{0x4006C0C1, 0xFCB3, 0x403E, {0x99, 0x6D, 0x4A, 0x6C, 0x87, 0x24, 0xE0, 0x6D}}, "gEfiLoadFile2ProtocolGuid"},
{{0xAF060190, 0x5E3A, 0x4025, {0xAF, 0xBD, 0xE1, 0xF9, 0x05, 0xBF, 0xAA, 0x4C}}, "gEfiHiiImageDecoderNamePngGuid"},
{{0xAC05BF33, 0x995A, 0x4ED4, {0xAA, 0xB8, 0xEF, 0x7A, 0xE8, 0x0F, 0x5C, 0xB0}}, "gUefiCpuPkgTokenSpaceGuid"},
{{0x4DF19259, 0xDC71, 0x4D46, {0xBE, 0xF1, 0x35, 0x7B, 0xB5, 0x78, 0xC4, 0x18}}, "gEfiPs2PolicyProtocolGuid"},
{{0xE0C14753, 0xF9BE, 0x11D2, {0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiPcAnsiGuid"},
{{0x76B6BDFA, 0x2ACD, 0x4462, {0x9E, 0x3F, 0xCB, 0x58, 0xC9, 0x69, 0xD9, 0x37}}, "gPerformanceProtocolGuid"},
{{0xCE345171, 0xBA0B, 0x11D2, {0x8E, 0x4F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiDiskIoProtocolGuid"},
{{0x2755590C, 0x6F3C, 0x42FA, {0x9E, 0xA4, 0xA3, 0xBA, 0x54, 0x3C, 0xDA, 0x25}}, "gEfiDebugSupportProtocolGuid"},
{{0x752F3136, 0x4E16, 0x4FDC, {0xA2, 0x2A, 0xE5, 0xF4, 0x68, 0x12, 0xF4, 0xCA}}, "gEfiShellParametersProtocolGuid"},
{{0xD2B2B828, 0x0826, 0x48A7, {0xB3, 0xDF, 0x98, 0x3C, 0x00, 0x60, 0x24, 0xF0}}, "gEfiStatusCodeRuntimeProtocolGuid"},
{{0x996EC11C, 0x5397, 0x4E73, {0xB5, 0x8F, 0x82, 0x7E, 0x52, 0x90, 0x6D, 0xEF}}, "gEfiVectorHandoffTableGuid"},
{{0x7CE88FB3, 0x4BD7, 0x4679, {0x87, 0xA8, 0xA8, 0xD8, 0xDE, 0xE5, 0x0D, 0x2B}}, "gEfiEventReadyToBootGuid"},
{{0x0F0B1735, 0x87A0, 0x4193, {0xB2, 0x66, 0x53, 0x8C, 0x38, 0xAF, 0x48, 0xCE}}, "gEfiIfrTianoGuid"},
{{0xAB38A0DF, 0x6873, 0x44A9, {0x87, 0xE6, 0xD4, 0xEB, 0x56, 0x14, 0x84, 0x49}}, "gEfiRamDiskProtocolGuid"},
{{0x7D916D80, 0x5BB1, 0x458C, {0xA4, 0x8F, 0xE2, 0x5F, 0xDD, 0x51, 0xEF, 0x94}}, "gEfiTtyTermGuid"},
{{0x51AA59DE, 0xFDF2, 0x4EA3, {0xBC, 0x63, 0x87, 0x5F, 0xB7, 0x84, 0x2E, 0xE9}}, "gEfiHashAlgorithmSha256Guid"},
{{0xEF9FC172, 0xA1B2, 0x4693, {0xB3, 0x27, 0x6D, 0x32, 0xFC, 0x41, 0x60, 0x42}}, "gEfiHiiDatabaseProtocolGuid"},
{{0x31878C87, 0x0B75, 0x11D5, {0x9A, 0x4F, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiSimplePointerProtocolGuid"},
{{0x19CB87AB, 0x2CB9, 0x4665, {0x83, 0x60, 0xDD, 0xCF, 0x60, 0x54, 0xF7, 0x9D}}, "gEfiPciHotPlugRequestProtocolGuid"},
{{0x49152E77, 0x1ADA, 0x4764, {0xB7, 0xA2, 0x7A, 0xFE, 0xFE, 0xD9, 0x5E, 0x8B}}, "gEfiDebugImageInfoTableGuid"},
{{0x7408D748, 0xFC8C, 0x4EE6, {0x92, 0x88, 0xC4, 0xBE, 0xC0, 0x92, 0xA4, 0x10}}, "gEfiPeiMasterBootModePpiGuid"},
{{0x3A4D7A7C, 0x018A, 0x4B42, {0x81, 0xB3, 0xDC, 0x10, 0xE3, 0xB5, 0x91, 0xBD}}, "gUsbKeyboardLayoutKeyGuid"},
{{0xDFA66065, 0xB419, 0x11D3, {0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiVT100Guid"},
{{0x2B9FFB52, 0x1B13, 0x416F, {0xA8, 0x7B, 0xBC, 0x93, 0x0D, 0xEF, 0x92, 0xA8}}, "gTcgEventEntryHobGuid"},
{{0xC51711E7, 0xB4BF, 0x404A, {0xBF, 0xB8, 0x0A, 0x04, 0x8E, 0xF1, 0xFF, 0xE4}}, "gEfiIp4ServiceBindingProtocolGuid"},
{{0x37499A9D, 0x542F, 0x4C89, {0xA0, 0x26, 0x35, 0xDA, 0x14, 0x20, 0x94, 0xE4}}, "gEfiUartDevicePathGuid"},
{{0x387477C2, 0x69C7, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiSimpleTextOutProtocolGuid"},
{{0x27CFAC88, 0x46CC, 0x11D4, {0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiResetArchProtocolGuid"},
{{0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiSimpleFileSystemProtocolGuid"},
{{0x982C298B, 0xF4FA, 0x41CB, {0xB8, 0x38, 0x77, 0xAA, 0x68, 0x8F, 0xB8, 0x39}}, "gEfiUgaDrawProtocolGuid"},
{{0x229832D3, 0x7A30, 0x4B36, {0xB8, 0x27, 0xF4, 0x0C, 0xB7, 0xD4, 0x54, 0x36}}, "gEfiPeiStatusCodePpiGuid"},
{{0x52C78312, 0x8EDC, 0x4233, {0x98, 0xF2, 0x1A, 0x1A, 0xA5, 0xE3, 0x88, 0xA5}}, "gEfiNvmExpressPassThruProtocolGuid"},
{{0x3EBD9E82, 0x2C78, 0x4DE6, {0x97, 0x86, 0x8D, 0x4B, 0xFC, 0xB7, 0xC8, 0x81}}, "gEfiFaultTolerantWriteProtocolGuid"},
{{0x821C9A09, 0x541A, 0x40F6, {0x9F, 0x43, 0x0A, 0xD1, 0x93, 0xA1, 0x2C, 0xFE}}, "gEdkiiMemoryProfileGuid"},
{{0x665E3FF6, 0x46CC, 0x11D4, {0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiBdsArchProtocolGuid"},
{{0x8F644FA9, 0xE850, 0x4DB1, {0x9C, 0xE2, 0x0B, 0x44, 0x69, 0x8E, 0x8D, 0xA4}}, "gEfiFirmwareVolumeBlockProtocolGuid"},
{{0xCDEA2BD3, 0xFC25, 0x4C1C, {0xB9, 0x7C, 0xB3, 0x11, 0x86, 0x06, 0x49, 0x90}}, "gEfiBootLogoProtocolGuid"},
{{0x0D3FB176, 0x9569, 0x4D51, {0xA3, 0xEF, 0x7D, 0x61, 0xC6, 0x4F, 0xEA, 0xBA}}, "gEfiSecurityPkgTokenSpaceGuid"},
{{0xA1E37052, 0x80D9, 0x4E65, {0xA3, 0x17, 0x3E, 0x9A, 0x55, 0xC4, 0x3E, 0xC9}}, "gEfiIdeControllerInitProtocolGuid"},
{{0x31CA5D1A, 0xD511, 0x4931, {0xB7, 0x82, 0xAE, 0x6B, 0x2B, 0x17, 0x8C, 0xD7}}, "gEfiIfrFrameworkGuid"},
{{0x2A46715F, 0x3581, 0x4A55, {0x8E, 0x73, 0x2B, 0x76, 0x9A, 0xAA, 0x30, 0xC5}}, "gRamDiskFormSetGuid"},
{{0x77AB535A, 0x45FC, 0x624B, {0x55, 0x60, 0xF7, 0xB2, 0x81, 0xD1, 0xF9, 0x6E}}, "gEfiVirtualDiskGuid"},
{{0xB2360B42, 0x7173, 0x420A, {0x86, 0x96, 0x46, 0xCA, 0x6B, 0xAB, 0x10, 0x60}}, "gMeasuredFvHobGuid"},
{{0x6A7A5CFF, 0xE8D9, 0x4F70, {0xBA, 0xDA, 0x75, 0xAB, 0x30, 0x25, 0xCE, 0x14}}, "gEfiComponentName2ProtocolGuid"},
{{0xE9DB0D58, 0xD48D, 0x47F6, {0x9C, 0x6E, 0x6F, 0x40, 0xE8, 0x6C, 0x7B, 0x41}}, "gPeiTpmInitializedPpiGuid"},
{{0xEFEFD093, 0x0D9B, 0x46EB, {0xA8, 0x56, 0x48, 0x35, 0x07, 0x00, 0xC9, 0x08}}, "gEfiHiiImageDecoderNameJpegGuid"},
{{0x245DCA21, 0xFB7B, 0x11D3, {0x8F, 0x01, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiPxeBaseCodeCallbackProtocolGuid"},
{{0x3C8D294C, 0x5FC3, 0x4451, {0xBB, 0x31, 0xC4, 0xC0, 0x32, 0x29, 0x5E, 0x6C}}, "gIdleLoopEventGuid"},
{{0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, "gEfiTpmDeviceInstanceNoneGuid"},
{{0x220E73B6, 0x6BDB, 0x4413, {0x84, 0x05, 0xB9, 0x74, 0xB1, 0x08, 0x61, 0x9A}}, "gEfiFirmwareVolume2ProtocolGuid"},
{{0x480F8AE9, 0x0C46, 0x4AA9, {0xBC, 0x89, 0xDB, 0x9F, 0xBA, 0x61, 0x98, 0x06}}, "gEfiDpcProtocolGuid"},
{{0xEB97088E, 0xCFDF, 0x49C6, {0xBE, 0x4B, 0xD9, 0x06, 0xA5, 0xB2, 0x0E, 0x86}}, "gEfiAcpiSdtProtocolGuid"},
{{0xDB47D7D3, 0xFE81, 0x11D3, {0x9A, 0x35, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiFileSystemVolumeLabelInfoIdGuid"},
{{0xDCFA911D, 0x26EB, 0x469F, {0xA2, 0x20, 0x38, 0xB7, 0xDC, 0x46, 0x12, 0x20}}, "gEfiMemoryAttributesTableGuid"},
{{0x14982A4F, 0xB0ED, 0x45B8, {0xA8, 0x11, 0x5A, 0x7A, 0x9B, 0xC2, 0x32, 0xDF}}, "gEfiHiiKeyBoardLayoutGuid"},
{{0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gEfiDevicePathProtocolGuid"},
{{0x3BC1B285, 0x8A15, 0x4A82, {0xAA, 0xBF, 0x4D, 0x7D, 0x13, 0xFB, 0x32, 0x65}}, "gEfiBusSpecificDriverOverrideProtocolGuid"},
{{0x060CC026, 0x4C0D, 0x4DDA, {0x8F, 0x41, 0x59, 0x5F, 0xEF, 0x00, 0xA5, 0x02}}, "gMemoryStatusCodeRecordGuid"},
{{0x1D3DE7F0, 0x0807, 0x424F, {0xAA, 0x69, 0x11, 0xA5, 0x4E, 0x19, 0xA4, 0x6F}}, "gEfiAtaPassThruProtocolGuid"},
{{0x27ABF055, 0xB1B8, 0x4C26, {0x80, 0x48, 0x74, 0x8F, 0x37, 0xBA, 0xA2, 0xDF}}, "gEfiEventExitBootServicesGuid"},
{{0xFFE06BDD, 0x6107, 0x46A6, {0x7B, 0xB2, 0x5A, 0x9C, 0x7E, 0xC5, 0x27, 0x5C}}, "gEfiAcpiTableProtocolGuid"},
{{0x41D94CD2, 0x35B6, 0x455A, {0x82, 0x58, 0xD4, 0xE5, 0x13, 0x34, 0xAA, 0xDD}}, "gEfiIp4ProtocolGuid"},
{{0x93BB96AF, 0xB9F2, 0x4EB8, {0x94, 0x62, 0xE0, 0xBA, 0x74, 0x56, 0x42, 0x36}}, "gUefiOvmfPkgTokenSpaceGuid"},
{{0x0CC252D2, 0xC106, 0x4661, {0xB5, 0xBD, 0x31, 0x47, 0xA4, 0xF8, 0x1F, 0x92}}, "gEfiPrint2SProtocolGuid"},
{{0x2AB86EF5, 0xECB5, 0x4134, {0xB5, 0x56, 0x38, 0x54, 0xCA, 0x1F, 0xE1, 0xB4}}, "gEfiPeiReadOnlyVariable2PpiGuid"},
{{0x0F6499B1, 0xE9AD, 0x493D, {0xB9, 0xC2, 0x2F, 0x90, 0x81, 0x5C, 0x6C, 0xBC}}, "gEfiPhysicalPresenceGuid"},
{{0x9E23D768, 0xD2F3, 0x4366, {0x9F, 0xC3, 0x3A, 0x7A, 0xBA, 0x86, 0x43, 0x74}}, "gEfiVlanConfigProtocolGuid"},
{{0x38321DBA, 0x4FE0, 0x4E17, {0x8A, 0xEC, 0x41, 0x30, 0x55, 0xEA, 0xED, 0xC1}}, "gEfiLegacy8259ProtocolGuid"},
{{0x6B558CE3, 0x69E5, 0x4C67, {0xA6, 0x34, 0xF7, 0xFE, 0x72, 0xAD, 0xBE, 0x84}}, "gBlockMmioProtocolGuid"},
{{0x6D582DBC, 0xDB85, 0x4514, {0x8F, 0xCC, 0x5A, 0xDF, 0x62, 0x27, 0xB1, 0x47}}, "gEfiPeiS3Resume2PpiGuid"},
{{0x6A1EE763, 0xD47A, 0x43B4, {0xAA, 0xBE, 0xEF, 0x1D, 0xE2, 0xAB, 0x56, 0xFC}}, "gEfiHiiPackageListProtocolGuid"},
{{0x2E3044AC, 0x879F, 0x490F, {0x97, 0x60, 0xBB, 0xDF, 0xAF, 0x69, 0x5F, 0x50}}, "gEfiLegacyBiosGuid"},
{{0x30CFE3E7, 0x3DE1, 0x4586, {0xBE, 0x20, 0xDE, 0xAB, 0xA1, 0xB3, 0xB7, 0x93}}, "gEfiPciEnumerationCompleteProtocolGuid"},
{{0x3D3CA290, 0xB9A5, 0x11E3, {0xB7, 0x5D, 0xB8, 0xAC, 0x6F, 0x7D, 0x65, 0xE6}}, "gXenBusProtocolGuid"},
{{0x8D59D32B, 0xC655, 0x4AE9, {0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43}}, "gEfiAbsolutePointerProtocolGuid"},
{{0x1A36E4E7, 0xFAB6, 0x476A, {0x8E, 0x75, 0x69, 0x5A, 0x05, 0x76, 0xFD, 0xD7}}, "gEfiPeiDecompressPpiGuid"},
{{0xF5089266, 0x1AA0, 0x4953, {0x97, 0xD8, 0x56, 0x2F, 0x8A, 0x73, 0xB5, 0x19}}, "gEfiUsbHcProtocolGuid"},
{{0x11B34006, 0xD85B, 0x4D0A, {0xA2, 0x90, 0xD5, 0xA5, 0x71, 0x31, 0x0E, 0xF7}}, "gPcdProtocolGuid"},
{{0x1ACED566, 0x76ED, 0x4218, {0xBC, 0x81, 0x76, 0x7F, 0x1F, 0x97, 0x7A, 0x89}}, "gEfiNetworkInterfaceIdentifierProtocolGuid_31"},
{{0x8B843E20, 0x8132, 0x4852, {0x90, 0xCC, 0x55, 0x1A, 0x4E, 0x4A, 0x7F, 0x1C}}, "gEfiDevicePathToTextProtocolGuid"},
{{0x4F6C5507, 0x232F, 0x4787, {0xB9, 0x5E, 0x72, 0xF8, 0x62, 0x49, 0x0C, 0xB1}}, "gEventExitBootServicesFailedGuid"},
{{0xBD8C1056, 0x9F36, 0x44EC, {0x92, 0xA8, 0xA6, 0x33, 0x7F, 0x81, 0x79, 0x86}}, "gEfiEdidActiveProtocolGuid"},
{{0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, "gEfiPartTypeUnusedGuid"},
{{0xD3B36F2D, 0xD551, 0x11D4, {0x9A, 0x46, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiStandardErrorDeviceGuid"},
{{0x9E498932, 0x4ABC, 0x45AF, {0xA3, 0x4D, 0x02, 0x47, 0x78, 0x7B, 0xE7, 0xC6}}, "gEfiDiskInfoAhciInterfaceGuid"},
{{0x92D11080, 0x496F, 0x4D95, {0xBE, 0x7E, 0x03, 0x74, 0x88, 0x38, 0x2B, 0x0A}}, "gEfiStatusCodeDataTypeStringGuid"},
{{0x1C0C34F6, 0xD380, 0x41FA, {0xA0, 0x49, 0x8A, 0xD0, 0x6C, 0x1A, 0x66, 0xAA}}, "gEfiEdidDiscoveredProtocolGuid"},
{{0x9E58292B, 0x7C68, 0x497D, {0xA0, 0xCE, 0x65, 0x00, 0xFD, 0x9F, 0x1B, 0x95}}, "gEdkiiWorkingBlockSignatureGuid"},
{{0xA19832B9, 0xAC25, 0x11D3, {0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiSimpleNetworkProtocolGuid"},
{{0x53CD299F, 0x2BC1, 0x40C0, {0x8C, 0x07, 0x23, 0xF6, 0x4F, 0xDB, 0x30, 0xE0}}, "gEdkiiPlatformLogoProtocolGuid"},
{{0xAF9FFD67, 0xEC10, 0x488A, {0x9D, 0xFC, 0x6C, 0xBF, 0x5E, 0xE2, 0x2C, 0x2E}}, "gEfiAcpiVariableGuid"},
{{0x1E43298F, 0x3478, 0x41A7, {0xB5, 0x77, 0x86, 0x06, 0x46, 0x35, 0xC7, 0x28}}, "gOptionRomPkgTokenSpaceGuid"},
{{0x07D75280, 0x27D4, 0x4D69, {0x90, 0xD0, 0x56, 0x43, 0xE2, 0x38, 0xB3, 0x41}}, "gEfiPciPlatformProtocolGuid"},
{{0xDB4E8151, 0x57ED, 0x4BED, {0x88, 0x33, 0x67, 0x51, 0xB5, 0xD1, 0xA8, 0xD7}}, "gConnectConInEventGuid"},
{{0xE43176D7, 0xB6E8, 0x4827, {0xB7, 0x84, 0x7F, 0xFD, 0xC4, 0xB6, 0x85, 0x61}}, "gEfiRngAlgorithmRaw"},
{{0x95A9A93E, 0xA86E, 0x4926, {0xAA, 0xEF, 0x99, 0x18, 0xE7, 0x72, 0xD9, 0x87}}, "gEfiEraseBlockProtocolGuid"},
{{0x8C8CE578, 0x8A3D, 0x4F1C, {0x99, 0x35, 0x89, 0x61, 0x85, 0xC3, 0x2D, 0xD3}}, "gEfiFirmwareFileSystem2Guid"},
{{0xF4B427BB, 0xBA21, 0x4F16, {0xBC, 0x4E, 0x43, 0xE4, 0x16, 0xAB, 0x61, 0x9C}}, "gEfiArpProtocolGuid"},
{{0x4CF5B200, 0x68B8, 0x4CA5, {0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A}}, "gEfiPciIoProtocolGuid"},
{{0x5473C07A, 0x3DCB, 0x4DCA, {0xBD, 0x6F, 0x1E, 0x96, 0x89, 0xE7, 0x34, 0x9A}}, "gEfiFirmwareFileSystem3Guid"},
{{0x6302D008, 0x7F9B, 0x4F30, {0x87, 0xAC, 0x60, 0xC9, 0xFE, 0xF5, 0xDA, 0x4E}}, "gEfiShellProtocolGuid"},
{{0x3CD652B4, 0x6D33, 0x4DCE, {0x89, 0xDB, 0x83, 0xDF, 0x97, 0x66, 0xFC, 0xCA}}, "gEfiVectorHandoffInfoPpiGuid"},
{{0x7739F24C, 0x93D7, 0x11D4, {0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiHobListGuid"},
{{0x932F47E6, 0x2362, 0x4002, {0x80, 0x3E, 0x3C, 0xD5, 0x4B, 0x13, 0x8F, 0x85}}, "gEfiScsiIoProtocolGuid"},
{{0x08F74BAA, 0xEA36, 0x41D9, {0x95, 0x21, 0x21, 0xA7, 0x0F, 0x87, 0x80, 0xBC}}, "gEfiDiskInfoScsiInterfaceGuid"},
{{0x64A892DC, 0x5561, 0x4536, {0x92, 0xC7, 0x79, 0x9B, 0xFC, 0x18, 0x33, 0x55}}, "gEfiIsaAcpiProtocolGuid"},
{{0xEB9D2D31, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiSmbiosTableGuid"},
{{0xBB25CF6F, 0xF1D4, 0x11D2, {0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD}}, "gEfiSerialIoProtocolGuid"},
{{0xAA0E8BC1, 0xDABC, 0x46B0, {0xA8, 0x44, 0x37, 0xB8, 0x16, 0x9B, 0x2B, 0xEA}}, "gEfiPciHotPlugInitProtocolGuid"},
{{0xD3B36F2B, 0xD551, 0x11D4, {0x9A, 0x46, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiConsoleInDeviceGuid"},
{{0xA770C357, 0xB693, 0x4E6D, {0xA6, 0xCF, 0xD2, 0x1C, 0x72, 0x8E, 0x55, 0x0B}}, "gEdkiiFormBrowserEx2ProtocolGuid"},
{{0x3079818C, 0x46D4, 0x4A73, {0xAE, 0xF3, 0xE3, 0xE4, 0x6C, 0xF1, 0xEE, 0xDB}}, "gEfiBootScriptExecutorVariableGuid"},
{{0x6B30C738, 0xA391, 0x11D4, {0x9A, 0x3B, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}, "gEfiPlatformDriverOverrideProtocolGuid"},
{{0xFD0F4478, 0x0EFD, 0x461D, {0xBA, 0x2D, 0xE5, 0x8C, 0x45, 0xFD, 0x5F, 0x5E}}, "gEfiGetPcdInfoProtocolGuid"},
{{0x31CE593D, 0x108A, 0x485D, {0xAD, 0xB2, 0x78, 0xF2, 0x1F, 0x29, 0x66, 0xBE}}, "gEfiLegacyInterruptProtocolGuid"},
{{0xEB704011, 0x1402, 0x11D3, {0x8E, 0x77, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}, "gMtcVendorGuid"},
{{0x18A031AB, 0xB443, 0x4D1A, {0xA5, 0xC0, 0x0C, 0x09, 0x26, 0x1E, 0x9F, 0x71}}, "gEfiDriverBindingProtocolGuid"},
{{0xA1AFF049, 0xFDEB, 0x442A, {0xB3, 0x20, 0x13, 0xAB, 0x4C, 0xB7, 0x2B, 0xBC}}, "gEfiMdeModulePkgTokenSpaceGuid"},
{{0x13A3F0F6, 0x264A, 0x3EF0, {0xF2, 0xE0, 0xDE, 0xC5, 0x12, 0x34, 0x2F, 0x34}}, "gEfiPcdProtocolGuid"},
{{0xF05976EF, 0x83F1, 0x4F3D, {0x86, 0x19, 0xF7, 0x59, 0x5D, 0x41, 0xE5, 0x38}}, "gEfiPrint2ProtocolGuid"},
{{0x94AB2F58, 0x1438, 0x4EF1, {0x91, 0x52, 0x18, 0x94, 0x1A, 0x3A, 0x0E, 0x68}}, "gEfiSecurity2ArchProtocolGuid"},
{{0xD3705011, 0xBC19, 0x4AF7, {0xBE, 0x16, 0xF6, 0x80, 0x30, 0x37, 0x8C, 0x15}}, "gEfiIntelFrameworkModulePkgTokenSpaceGuid"},
{{0xE857CAF6, 0xC046, 0x45DC, {0xBE, 0x3F, 0xEE, 0x07, 0x65, 0xFB, 0xA8, 0x87}}, "gEfiS3SaveStateProtocolGuid"},
{{0x70101EAF, 0x0085, 0x440C, {0xB3, 0x56, 0x8E, 0xE3, 0x6F, 0xEF, 0x24, 0xF0}}, "gEfiLegacyRegion2ProtocolGuid"},
{{0xC7735A2F, 0x88F5, 0x4882, {0xAE, 0x63, 0xFA, 0xAC, 0x8C, 0x8B, 0x86, 0xB3}}, "gEfiVgaMiniPortProtocolGuid"},
{{0x5053697E, 0x2CBC, 0x4819, {0x90, 0xD9, 0x05, 0x80, 0xDE, 0xEE, 0x57, 0x54}}, "gEfiCapsuleArchProtocolGuid"},
{{0xB1EE129E, 0xDA36, 0x4181, {0x91, 0xF8, 0x04, 0xA4, 0x92, 0x37, 0x66, 0xA7}}, "gEfiDriverFamilyOverrideProtocolGuid"},
{{0xA46423E3, 0x4617, 0x49F1, {0xB9, 0xFF, 0xD1, 0xBF, 0xA9, 0x11, 0x58, 0x39}}, "gEfiSecurityArchProtocolGuid"},
{{0x330D4706, 0xF2A0, 0x4E4F, {0xA3, 0x69, 0xB6, 0x6F, 0xA8, 0xD5, 0x43, 0x85}}, "gEfiHiiConfigAccessProtocolGuid"},
{{0xFC1BCDB0, 0x7D31, 0x49AA, {0x93, 0x6A, 0xA4, 0x60, 0x0D, 0x9D, 0xD0, 0x83}}, "CRC32"},
{{0xA31280AD, 0x481E, 0x41B6, {0x95, 0xE8, 0x12, 0x7F, 0x4C, 0x98, 0x47, 0x79}}, "TIANO_COMPRESS"},
{{0xEE4E5898, 0x3914, 0x4259, {0x9D, 0x6E, 0xDC, 0x7B, 0xD7, 0x94, 0x03, 0xCF}}, "LZMA_COMPRESS"}};
/* End of GuidMappings */


/****************** End of EFI types ***********************/

/* Using *char[] is much more elegant, but it is prone to chnages of enum
 * values. Therefore we opted to use switch cases, automatically generated.
 * */
char* get_efi_mem_type_str( int mem_type )
{
        char *description = "<None>";

        switch(mem_type) {
        case EfiReservedMemoryType:
                description = "EfiReservedMemoryType";
                break;
        case EfiLoaderCode:
                description = "EfiLoaderCode";
                break;
        case EfiLoaderData:
                description = "EfiLoaderData";
                break;
        case EfiBootServicesCode:
                description = "EfiBootServicesCode";
                break;
        case EfiBootServicesData:
                description = "EfiBootServicesData";
                break;
        case EfiRuntimeServicesCode:
                description = "EfiRuntimeServicesCode";
                break;
        case EfiRuntimeServicesData:
                description = "EfiRuntimeServicesData";
                break;
        case EfiConventionalMemory:
                description = "EfiConventionalMemory";
                break;
        case EfiUnusableMemory:
                description = "EfiUnusableMemory";
                break;
        case EfiACPIReclaimMemory:
                description = "EfiACPIReclaimMemory";
                break;
        case EfiACPIMemoryNVS:
                description = "EfiACPIMemoryNVS";
                break;
        case EfiMemoryMappedIO:
                description = "EfiMemoryMappedIO";
                break;
        case EfiMemoryMappedIOPortSpace:
                description = "EfiMemoryMappedIOPortSpace";
                break;
        case EfiPalCode:
                description = "EfiPalCode";
                break;
        case EfiPersistentMemory:
                description = "EfiPersistentMemory";
                break;
        case EfiMaxMemoryType:
                description = "EfiMaxMemoryType";
                break;
        }

        return description;
}

char* get_efi_allocation_type_str( int allocation_type )
{
        char *description = "<None>";

        switch(allocation_type) {
        case AllocateAnyPages:
                description = "AllocateAnyPages";
                break;
        case AllocateMaxAddress:
                description = "AllocateMaxAddress";
                break;
        case AllocateAddress:
                description = "AllocateAddress";
                break;
        case MaxAllocateType:
                description = "MaxAllocateType";
                break;
        }

        return description;
}



int32_t
CompareGuid (EFI_GUID     *Guid1, EFI_GUID     *Guid2)
{
        int32_t *g1;
        int32_t *g2;
        int32_t r;

         /* Compare 32 bits at a time */
        g1  = (int32_t*) Guid1;
        g2  = (int32_t*) Guid2;

        r = g1[0] - g2[0];
        r |= g1[1] - g2[1];
        r |= g1[2] - g2[2];
        r |= g1[3] - g2[3];

        return r;
}

char temp_GUID_buff[64];
char* get_GUID_str( EFI_GUID* guid )
{
  sprintf( temp_GUID_buff, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            guid->Data1,
            guid->Data2,
            guid->Data3,
            guid->Data4[0],
            guid->Data4[1],
            guid->Data4[2],
            guid->Data4[3],
            guid->Data4[4],
            guid->Data4[5],
            guid->Data4[6],
            guid->Data4[7] );

  return temp_GUID_buff;
}

char* GetGuidName( EFI_GUID *Protocol )
{
        int i;
        if( Protocol == NULL )
                return "<NULL protocol pointer>";

        for( i = 0; i < NUM_GUID_MAPPINGS; i++ ) {
                if (CompareGuid (Protocol, &GuidMappings[i].Guid) == 0) {
                        return GuidMappings[i].Name;
                }
        }

        return "<Unknown>";
}

/*********** Protocol handlers ****************/
void efi_set_wstring_from_ascii( CHAR16* dst, const char* src, size_t max_dst_size_bytes )
{
        int i = 0;
        char* dst_as_char = (char*)(dst);
        for (i = 0; i*2 < max_dst_size_bytes; i++ ) {
                dst_as_char[i*2] = src[i];
                dst_as_char[i*2+1] = '\0';

                if ( src[i] == '\0' )
                        break;
        }
}

/* The following struct is based on the reverse engineering of the LoadOptions
 * blob when observing a normal Windows EFI boot  */
typedef struct {
        CHAR8 header1[8];
        UINT32 val1;
        UINT32 val2;
        UINT32 val3;
        CHAR16 option[49];
        UINT16 val4;
        UINT32 val5;
        UINT32 val6;
        UINT32 val7;
        UINT32 val8;
} REVERSED_LOAD_OPTIONS;

REVERSED_LOAD_OPTIONS windows_load_options =  {
        .header1 = "WINDOWS",
        .val1 = 0x1,
        .val2 = sizeof(REVERSED_LOAD_OPTIONS),
        .val3 = sizeof(REVERSED_LOAD_OPTIONS) - 16,
        .option = {0},
        .val4 = 0x73,
        .val5 = 0x1,
        .val6 = 0x10,
        .val7 = 0x4,
        .val8 = 0x4ff7f
};

/* All device paths must end in this constant "device" node
 * See ch. 9.3 in
 * https:*uefi.org/sites/default/files/resources/UEFI%20Spec%202_6.pdf */
EFI_DEVICE_PATH_PROTOCOL end_device_path_node = {
        .Type     = 0x7F,
        .SubType  = 0xFF,
        .Length   = {0x04, 0x00}
};

EFI_DEVICE_PATH_PROTOCOL* creat_windows_loader_device(void)
{
        const char* windows_loader_bootmg_file          =
                        "\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
        size_t sizeof_bootmg_file_path_as_wstring       =
            sizeof( CHAR16 ) * ( strlen( windows_loader_bootmg_file ) + 1 );
        EFI_DEVICE_PATH_PROTOCOL *windows_loader_device = NULL;
        uint16_t* pathLength                            = NULL;

        /* We now create a DevicePath of the "device" the started launching
         * Windows */
        windows_loader_device = (EFI_DEVICE_PATH_PROTOCOL*) vmalloc(
              sizeof( EFI_DEVICE_PATH_PROTOCOL ) +
              sizeof_bootmg_file_path_as_wstring +
              sizeof( end_device_path_node ) );
        DebugMSG( "windows_loader_device @ 0x%px", windows_loader_device );

        windows_loader_device->Type    = 0x4,    /* Media Device Path. */
        windows_loader_device->SubType = 0x4,    /* File Path. */
        pathLength                     = (uint16_t*)windows_loader_device->Length;
        *pathLength                    = sizeof( EFI_DEVICE_PATH_PROTOCOL ) +
                                                sizeof_bootmg_file_path_as_wstring;
        efi_set_wstring_from_ascii( (CHAR16*)windows_loader_device->data,
                                    windows_loader_bootmg_file,
                                    sizeof_bootmg_file_path_as_wstring );

        /* Terminate path with "End of Hardware Device Path": */
        memcpy( (uint8_t*)windows_loader_device + *pathLength,
                &end_device_path_node,
                sizeof( end_device_path_node ) );

        DumpBuffer( "Windows LoadedImage device", (uint8_t*)windows_loader_device,
                    *pathLength + sizeof( end_device_path_node ) );

        return windows_loader_device;
}

/* BOOT_DEVICE_HANDLE and windows_boot_device_path are mocks. The mock handle
 * helps us identify later on the handle. windows_boot_device_path is copied
 * from a normal Windows EFI boot we logged. */
#define BOOT_DEVICE_HANDLE (EFI_HANDLE)0xDEADBEEF

uint8_t windows_boot_device_path[72] = {
        /* ACPIPciRoot(0x0) */
        0x02, 0x01, 0x0C, 0x00, 0xD0, 0x41, 0x03, 0x0A,
        0x00, 0x00, 0x00, 0x00,

        /* Pci(0x4,0x0) */
        0x01, 0x01, 0x06, 0x00,
        0x00, 0x04,

        /* Scsi(0x1,0x0) */
        0x03, 0x02, 0x08, 0x00, 0x01, 0x00,
        0x00, 0x00,

        /* HD(2,GPT,F6B5FF3C-2E8F-470D-98A8-D1110EDD1E1E,0x8000,0x32000) */
        0x04, 0x01, 0x2A, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x20, 0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x3C, 0xFF, 0xB5, 0xF6, 0x8F, 0x2E,
        0x0D, 0x47, 0x98, 0xA8, 0xD1, 0x11, 0x0E, 0xDD,
        0x1E, 0x1E, 0x02, 0x02,

        /* End Node */
        0x7F, 0xFF, 0x04, 0x00,
};

EFI_LOADED_IMAGE_PROTOCOL windows_loaded_image = {
        .Revision         = 0x1000,
        .ParentHandle     = (void*)0x420000,
        .SystemTable      = NULL,
        .DeviceHandle     = BOOT_DEVICE_HANDLE,
        .FilePath         = NULL,
        .LoadOptionsSize  = sizeof(REVERSED_LOAD_OPTIONS),
        .LoadOptions      = NULL,
        .ImageBase        = NULL,
        .ImageSize        = 0,
        .ImageCodeType    = EfiLoaderCode,
        .ImageDataType    = EfiLoaderData,
        .Unload           = (void*)0x430000,
};

efi_system_table_t  fake_systab        = {0};
efi_boot_services_t linux_bootservices = {0};

void kimage_load_pe(struct kimage *image, unsigned long nr_segments)
{
        unsigned long raw_image_relative_start;
        size_t        image_size = 0;
        int           i;

        /* Calculate total image size and allocate it: */
        for (i = 0; i < nr_segments; i++) {
                image_size += image->segment[i].memsz;
        }
        image->raw_image          = vmalloc_exec( image_size );

        /* ImageBase in objdump of efi image */
        image->raw_image_mem_base = image->segment[0].mem;

        raw_image_relative_start  = image->start - image->raw_image_mem_base;
        image->raw_image_start    = (void*)( image->raw_image + raw_image_relative_start );
        DebugMSG(  "image->raw_image = %px; "
                   "image->raw_image_mem_base = 0x%lx; "
                   "image_size = 0x%lx; "
                   "image->raw_image_start = %px\n",
                   image->raw_image,
                   image->raw_image_mem_base,
                   image_size,
                   image->raw_image_start );

        for (i = 0; i < nr_segments; i++) {
                kimage_load_pe_segment(image, &image->segment[i]);
        }

        windows_loaded_image.ImageBase   = (VOID*)image->raw_image;
        windows_loaded_image.ImageSize   = image_size;
        windows_loaded_image.SystemTable = (void*)&fake_systab;

       /* We now need to parse the relocation table of the PE and then patch the
        * efi binary. We assume that the last segment is the relocatiuon
        * segment. */
       /* TODO: Patch the relocations in user space. I.e., the segments being
        * sent to kexec_load should already be patched */
        parse_reloc_table( &image->segment[nr_segments-1], image );
}

efi_status_t efi_handle_protocol_LoadedImage( void* handle, void** interface )
{
        EFI_DEVICE_PATH_PROTOCOL *windows_loader_device = NULL;

        DebugMSG( "Called" );

        efi_set_wstring_from_ascii( windows_load_options.option,
                                    "BCDOBJECT={9dea862c-5cdd-4e70-acc1-f32b344d4795}",
                                    sizeof( windows_load_options.option ) );

        windows_loader_device            = creat_windows_loader_device();
        windows_loaded_image.FilePath    = windows_loader_device;
        windows_loaded_image.LoadOptions = &windows_load_options;
        DumpBuffer( "LoadOptions",
                    ( uint8_t* )&windows_load_options,
                    sizeof( windows_load_options ) );

        *interface = (void*)&windows_loaded_image;

        DebugMSG( "LoadedImage at %px;", *interface);
        DebugMSG( "Revision         = 0x%x;", windows_loaded_image.Revision);
        DebugMSG( "ParentHandle     = %px;", windows_loaded_image.ParentHandle);
        DebugMSG( "SystemTable      = %px;", windows_loaded_image.SystemTable );
        DebugMSG( "DeviceHandle     = %px;", windows_loaded_image.DeviceHandle );
        DebugMSG( "FilePath         = %px;", windows_loaded_image.FilePath );
        DebugMSG( "LoadOptionsSize  = %d;", windows_loaded_image.LoadOptionsSize );
        DebugMSG( "LoadOptions      = %px;", windows_loaded_image.LoadOptions );
        DebugMSG( "ImageBase        = %px;", windows_loaded_image.ImageBase );
        DebugMSG( "ImageSize        = 0x%llx;", windows_loaded_image.ImageSize );
        DebugMSG( "ImageCodeType    = 0x%x;", windows_loaded_image.ImageCodeType );
        DebugMSG( "ImageDataType    = 0x%x;", windows_loaded_image.ImageDataType );
        DebugMSG( "Unload           = %px;", windows_loaded_image.Unload);

        return EFI_SUCCESS;
}

efi_status_t efi_handle_protocol_DevicePath( void* handle, void** interface )
{
        DebugMSG( "Called" );

        if (handle != BOOT_DEVICE_HANDLE) {
                DebugMSG( "unknown handle %px", handle );

                return EFI_UNSUPPORTED;
        }

        *interface = (void*)windows_boot_device_path;

        DebugMSG( "Returning constant boot device path @ %px",
                   windows_boot_device_path );

        DumpBuffer( "Boot Device Path", (uint8_t*) *interface, sizeof( windows_boot_device_path ) );

        return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_conin_hook_Reset(void)
{
         DebugMSG( "ConIn was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conin_hook_ReadKeyStrokeEx(void)
{
         DebugMSG( "ConIn was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conin_hook_SetState(
                                                        void* this_protocol,
                                                        void* KeyToggleState )
{
         DebugMSG( "Ignoring call!" );

         return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_conin_hook_RegisterKeyNotify(void)
{
         DebugMSG( "ConIn was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conin_hook_UnregisterKeyNotify(void)
{
         DebugMSG( "ConIn was called" );

         return EFI_UNSUPPORTED;
}

#define CON_IN_HANDLE         0xdeadbeefcafebab1
#define WAIT_FOR_KEY_EVENT_ID 0xABCDEFABCDEF2345

EFI_SIMPLE_TEXT_EX_INPUT_PROTOCOL con_in = {
        .Reset               = efi_conin_hook_Reset,
        .ReadKeyStrokeEx     = efi_conin_hook_ReadKeyStrokeEx,
        .WaitForKeyEx        = (void*)WAIT_FOR_KEY_EVENT_ID,
        .SetState            = efi_conin_hook_SetState,
        .RegisterKeyNotify   = efi_conin_hook_RegisterKeyNotify,
        .UnregisterKeyNotify = efi_conin_hook_UnregisterKeyNotify
};


efi_status_t efi_handle_protocol_SimpleTextInputExProtocol( void*  handle,
                                                            void** interface )
{
        DebugMSG( "handle = %px", handle );

        if (handle != (void*)CON_IN_HANDLE) {
                DebugMSG( "unknown handle %px", handle );

                return EFI_UNSUPPORTED;
        }

        *interface = &con_in;

        return EFI_SUCCESS;
}
/*********** End of protocols *****************/

/* This function receives a virtual addr and created a 1:1 mapping between
 * virtual memory to the actual physical address that belongs to addr */
void efi_setup_11_mapping( void* addr, size_t size )
{
        unsigned long start     = ALIGN_DOWN( virt_to_phys(addr), PAGE_SIZE);
        unsigned long end       = ALIGN(virt_to_phys(addr) + size, PAGE_SIZE);
        unsigned long mmap_ret  = 0;
        unsigned long populate  = 0;
        int           remap_err = 0;

        struct mm_struct      *mm  = current->mm;
        struct vm_area_struct *vma = NULL;

        vma = find_vma(mm, start) ;
        DebugMSG( "start = 0x%lx, end = 0x%lx, vma->vm_start = 0x%lx; "
                  "vma->vm_end = 0x%lx",
                  start, end, vma->vm_start, vma->vm_end );

        if ( vma->vm_start <= start ) {
                /* vma already exists. We expect the flags to contain VM_PFNMAP
                 * which means we already created 1:1 mapping for this address
                 * Otherwise - something is wrong. Specifically, the user-space
                 * memory was probably already in use. */

                /* The following flags are set by remap_pfn_range */
                u32  pfn_remapping_flags    =
                                VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;

                bool memory_is_pfn_remapped =
                                vma->vm_flags & pfn_remapping_flags;
                BUG_ON( ! memory_is_pfn_remapped );

                /* end must be smaller than the vma end: */
                BUG_ON( vma->vm_end < end );

                /* We already mapped these addresses as 1:1 */
                DebugMSG( "These addresses should already be 1:1 mapped. Skipping." );
                return;
        }

        /* TODO: should we make sure size of a multiple of PAGE_SIZE? */
        /* BUG_ON( size % PAGE_SIZE != 0 ); */

        /* The mm semaphore is required for both do_mmap AND remap_pfn_range */
        down_write(&mm->mmap_sem);

        /* First, we need to add a vma structure corresponding to the
         * user-space address matching the physical address */
        mmap_ret = do_mmap( NULL,
                            start,
                            end - start,
                            PROT_READ | PROT_WRITE,
                            MAP_FIXED | MAP_PRIVATE,
                            VM_READ | VM_WRITE,
                            0,
                            &populate,
                            NULL /* struct list_head *u */
        );
        DebugMSG( "mmap_ret = 0x%lx; populate = 0x%lx", mmap_ret, populate );

        /* Fetch the vma struct for our newly allocated user-space memory */
        vma = find_vma(mm, start) ;
        DebugMSG( "vma->vm_start = 0x%lx; vma->vm_end = 0x%lx",
                  vma->vm_start, vma->vm_end );

        /* Adjust end to fit the entire vma */
        if (vma->vm_end > end)
                end = vma->vm_end;

        /* Next,remap the physical memory, allocated to the kernel,
         * to the user-space */
        remap_err = remap_pfn_range( vma, start, start >> PAGE_SHIFT,
                                     end - start, PAGE_KERNEL );
        DebugMSG( "remap_pfn_range -> %d", remap_err );

        up_write(&mm->mmap_sem);
}

#define EFI_MAX_MEMORY_MAPPINGS 1000
#define EFI_DEFAULT_MEM_ATTRIBUTES ( EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB )

typedef struct {
	u32 type;
	u32 pad;
	u64 phys_addr;
	u64 virt_addr;
	u64 num_pages;
	u64 attribute;
    u64 pad2;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
        EFI_MEMORY_DESCRIPTOR mem_descriptor;
        struct list_head      list;
} MemoryAllocation;

LIST_HEAD( efi_memory_mappings );
uint64_t efi_mem_map_epoch = 0;

void efi_register_mem_allocation(  EFI_MEMORY_TYPE       MemoryType,
                                   UINTN                 NumberOfPages,
                                   void*                 allocation )
{
        EFI_MEMORY_DESCRIPTOR *mem_map   = NULL;
        MemoryAllocation      *mem_alloc = kmalloc( sizeof(MemoryAllocation),
                                                    GFP_KERNEL );
        if(!mem_alloc) {
                DebugMSG( "ERROR: OUT OF MEMORY!" );
                return;
        }

        DebugMSG( "Registering %lld pages of type %s @ %px",
                   NumberOfPages, get_efi_mem_type_str( MemoryType ),
                   allocation );

        /* TODO: Search if the memory address already exists in
         * &efi_memory_mappings. If so, use that mapping. */

        mem_map = &mem_alloc->mem_descriptor;
        INIT_LIST_HEAD( &mem_alloc->list );

        memset( mem_map, 0, sizeof( *mem_map ) );
        mem_map->type      = MemoryType;
        mem_map->pad       = 0;
        mem_map->phys_addr = virt_to_phys( allocation );
        mem_map->virt_addr = 0;  // Similar to EDK-II code
        mem_map->num_pages = NumberOfPages;
        mem_map->attribute = EFI_DEFAULT_MEM_ATTRIBUTES;

        list_add_tail( &mem_alloc->list, &efi_memory_mappings);
}

efi_status_t efi_unregister_allocation( efi_physical_addr_t PhysicalAddress,
                                        UINTN               NumberOfPages )
{
        EFI_MEMORY_DESCRIPTOR *mem_map          = NULL;
        u64                   offset_in_mapping = 0;
        efi_physical_addr_t   end_of_region     = 0;

        MemoryAllocation *mem_alloc = NULL;
        list_for_each_entry( mem_alloc, &efi_memory_mappings, list ) {
                mem_map = &mem_alloc->mem_descriptor;

                end_of_region = mem_map->phys_addr +
                                mem_map->num_pages * PAGE_SIZE;
                if (PhysicalAddress < mem_map->phys_addr ||
                    PhysicalAddress >= end_of_region)
                        continue;

                offset_in_mapping = PhysicalAddress - mem_map->phys_addr;

                DebugMSG( "Located mapping phys->virt: 0x%llx->0x%llx "
                          "(%lld pages, offset=0x%llx)",
                          mem_map->phys_addr, mem_map->virt_addr,
                          NumberOfPages, offset_in_mapping );

                if (offset_in_mapping != 0 ||
                    mem_map->num_pages != NumberOfPages ) {
                       DebugMSG( "Free request is different than allocation!!" );
                       /* TODO: handle greacefully. For example, allow
                        * reclaiming parts or regions */
                       return EFI_INVALID_PARAMETER;
                }

                mem_map->type = EfiConventionalMemory; /* Memory is free now */

                return EFI_SUCCESS;
        }

        DebugMSG( "Couldn't find mapping." );
        return EFI_INVALID_PARAMETER;
}

/*********** EFI hooks ************************/
__attribute__((ms_abi)) efi_status_t efi_hook_RaiseTPL(void)
{
         DebugMSG( "BOOT SERVICE #0 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_RestoreTPL(void)
{
         DebugMSG( "BOOT SERVICE #1 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_FreePages(
                                          efi_physical_addr_t PhysicalAddress,
                                          UINTN               NumberOfPages )
{
        DebugMSG( "Physical address = 0x%llx, NumberOfPages = %lld",
                   PhysicalAddress, NumberOfPages );

        return efi_unregister_allocation( PhysicalAddress, NumberOfPages );
}

size_t efi_get_mem_map_size(void)
{
        u32              num_mem_allocations = 0;
        struct list_head *position           = NULL;

        list_for_each ( position , &efi_memory_mappings )
        {
                num_mem_allocations++;
        }

        DebugMSG( "Number of entries in MemMap: %d", num_mem_allocations );

        return num_mem_allocations * sizeof( EFI_MEMORY_DESCRIPTOR );
}

__attribute__((ms_abi)) efi_status_t efi_hook_GetMemoryMap(
                                     unsigned long         *MemoryMapSize,
                                     EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                     unsigned long         *MapKey,
                                     unsigned long         *DescriptorSize,
                                     u32                   *DescriptorVersion)

{
        size_t                current_mapping_size = efi_get_mem_map_size();
        int                   entryIdx             = 0;
        EFI_MEMORY_DESCRIPTOR *mem_map             = NULL;
        efi_status_t          status               = EFI_SUCCESS;
        uint8_t*              current_offset       = ( uint8_t* )MemoryMap;
        MemoryAllocation      *mem_alloc           = NULL;

        *DescriptorVersion        = 1;
        *DescriptorSize           = sizeof( EFI_MEMORY_DESCRIPTOR );

        DebugMSG( "MemoryMapSize @ %px "
                  "MemoryMap @ %px "
                  "DescriptorSize = %ld "
                  "DescriptorVersion = %d",
                  MemoryMapSize, MemoryMap,
                  *DescriptorSize, *DescriptorVersion );

        if (*MemoryMapSize < current_mapping_size ) {
                unsigned long mmap_size_in  = *MemoryMapSize;
                *MemoryMapSize              = current_mapping_size;
                status                      = EFI_BUFFER_TOO_SMALL;
                DebugMSG( "Buffer too small. MemoryMapSize = %ld bytes, "
                          "need %ld. status = 0x%lx",
                           mmap_size_in, *MemoryMapSize, status );

                return status;
        }


        list_for_each_entry( mem_alloc, &efi_memory_mappings, list ) {
                mem_map = &mem_alloc->mem_descriptor;
                memcpy( current_offset, mem_map, sizeof( *mem_map ) );
                current_offset += sizeof( *mem_map );


                DebugMSG( "%3d: %-25s, 0x%16llx -> 0x%16llx, %5lld, 0x%016llx",
                    entryIdx++, get_efi_mem_type_str(mem_map->type),
                    mem_map->phys_addr, mem_map->virt_addr,
                    mem_map->num_pages, mem_map->attribute );
        }

        *MemoryMapSize  = current_offset - ( uint8_t* )MemoryMap;
        *MapKey         = efi_mem_map_epoch;

        DebugMSG( "MemoryMapSize = %ld MapKey = 0x%lx", 
                  *MemoryMapSize, *MapKey );

        return EFI_SUCCESS;
}

#define NUM_PAGES(size) ((size-1) / PAGE_SIZE + 1)

__attribute__((ms_abi)) efi_status_t efi_hook_AllocatePool(
                        EFI_MEMORY_TYPE pool_type,
                        unsigned long  size,
                        void           **buffer )
{
        void* allocation = NULL;

        DebugMSG( "pool_type = 0x%x (%s), size = 0x%lx",
                  pool_type, get_efi_mem_type_str( pool_type ), size );

        /* TODO: search for free memory which is EfiConventionalMemory, instead
         * of always allocating new kernel memory */
        allocation = kmalloc( size, GFP_KERNEL | GFP_DMA );
        if (allocation == NULL)
                return EFI_OUT_OF_RESOURCES;

        DebugMSG( "Allocated at 0x%px (physical addr: 0x%llx)",
                  allocation, virt_to_phys( allocation ) );

        efi_setup_11_mapping( allocation, size );
        *buffer = ( void* )virt_to_phys( allocation );

        efi_register_mem_allocation( pool_type, NUM_PAGES( size ), allocation );

        return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_hook_AllocatePages(
                                           EFI_ALLOCATE_TYPE     Type,
                                           EFI_MEMORY_TYPE       MemoryType,
                                           UINTN                 NumberOfPages,
                                           efi_physical_addr_t   *Memory )
{
        efi_status_t status = EFI_UNSUPPORTED;

        DebugMSG( "Num pages = %lld; Allocation type: %s; "
                  "Memory type: %s; Requested address = 0x%llx",
                   NumberOfPages,
                   get_efi_allocation_type_str( Type ),
                   get_efi_mem_type_str( MemoryType ),
                   *Memory );

        if ( MemoryType != EfiLoaderData         &&
             MemoryType != EfiConventionalMemory &&
             MemoryType != EfiLoaderCode
             ) {
                DebugMSG( "Unsupproted MemoryType 0x%x", MemoryType );
                return EFI_UNSUPPORTED;
        }

        if ( Type == AllocateAddress ) {
                /* We reassign the existing physical address to a new vritual
                 * address. */
                void* allocation =
                      memremap( *Memory, NumberOfPages*PAGE_SIZE, MEMREMAP_WB );
                DebugMSG( "Allocated %px --> 0x%llx", allocation,
                          virt_to_phys( allocation) );

                efi_setup_11_mapping( allocation, NumberOfPages * PAGE_SIZE );
                efi_register_mem_allocation( MemoryType,
                                             NumberOfPages,
                                             allocation );

                /* TODO: maintain bookkeeping of thois allocation for MemMap */

                return EFI_SUCCESS;
        }
        else if ( Type == AllocateAnyPages ) {
                void* phys_allocation = 0;

                DebugMSG( "Calling efi_hook_AllocatePool" );
                status = efi_hook_AllocatePool( MemoryType,
                                                NumberOfPages * PAGE_SIZE,
                                                &phys_allocation);

                *Memory = ( efi_physical_addr_t )phys_allocation;

                return status;
        }

        DebugMSG( "FAIL! Unknown Type" );
        return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_FreePool(void* buff)
{
         DebugMSG( "buff @ %px; TODO: implement bookkeeping", buff );

         /* TODO: We need to do some book keeping for the sake of MemoryMap */

         /* Since we performed 11 mapping, we can't just kfree memory. We
          * therefore just ignore the call for now */

         return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CreateEvent(void)
{
         DebugMSG( "BOOT SERVICE #7 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_SetTimer(void)
{
         DebugMSG( "BOOT SERVICE #8 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_WaitForEvent(void)
{
         DebugMSG( "BOOT SERVICE #9 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_SignalEvent(void)
{
         DebugMSG( "BOOT SERVICE #10 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CloseEvent(void)
{
         DebugMSG( "BOOT SERVICE #11 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CheckEvent(void)
{
         DebugMSG( "BOOT SERVICE #12 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_InstallProtocolInterface(void)
{
         DebugMSG( "BOOT SERVICE #13 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_ReinstallProtocolInterface(void)
{
         DebugMSG( "BOOT SERVICE #14 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_UninstallProtocolInterface(void)
{
         DebugMSG( "BOOT SERVICE #15 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_HandleProtocol( void* handle,
                                                              EFI_GUID* guid,
                                                              void** interface )
{
        const char* protocolName = GetGuidName( guid );
        DebugMSG( "handle = 0x%px guid = %s: %s",
                   handle, protocolName, get_GUID_str( guid ) );

        if (strcmp (protocolName, "gEfiLoadedImageProtocolGuid") == 0) {
                return efi_handle_protocol_LoadedImage( handle, interface );
        }
        if (strcmp (protocolName, "gEfiDevicePathProtocolGuid") == 0) {
                return efi_handle_protocol_DevicePath( handle, interface );
        }

        DebugMSG( "Unsuppurted protocol requested." );
        return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_Reserved(void)
{
         DebugMSG( "BOOT SERVICE #17 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_RegisterProtocolNotify(void)
{
         DebugMSG( "BOOT SERVICE #18 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_LocateHandle(
                                        int        SearchType,
                                        EFI_GUID   *Protocol,
                                        VOID       *SearchKey,
                                        UINTN      *BufferSize,
                                        EFI_HANDLE *Buffer)

{
         DebugMSG( "SearchType = %d, protocol = %s (%s), BufferSize = %lld",
                   SearchType, GetGuidName( Protocol ),
                   get_GUID_str( Protocol ), *BufferSize );

         return EFI_NOT_FOUND;
}

__attribute__((ms_abi)) efi_status_t efi_hook_LocateDevicePath(void)
{
         DebugMSG( "BOOT SERVICE #20 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_InstallConfigurationTable(void)
{
         DebugMSG( "BOOT SERVICE #21 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_LoadImage(void)
{
         DebugMSG( "BOOT SERVICE #22 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_StartImage(void)
{
         DebugMSG( "BOOT SERVICE #23 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_Exit(void)
{
         DebugMSG( "BOOT SERVICE #24 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_UnloadImage(void)
{
         DebugMSG( "BOOT SERVICE #25 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_ExitBootServices(void)
{
         DebugMSG( "BOOT SERVICE #26 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_GetNextMonotonicCount(void)
{
         DebugMSG( "BOOT SERVICE #27 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_Stall(void)
{
         DebugMSG( "Ignoring call" );

         return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_hook_SetWatchdogTimer( UINTN    Timeout,
                                                                UINT64   WatchdogCode,
                                                                UINTN    DataSize,
                                                                CHAR16   *WatchdogData )
{
        DebugMSG( "Timeout = %lld, WatchdogCode = 0x%llx, DataSize = %lld",
                  Timeout, WatchdogCode, DataSize );

        /* It's Ok to ignore this call. See
         * https://uefi.org/sites/default/files/resources/UEFI%20Spec%202_6.pdf
         */
        return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_hook_ConnectController(void)
{
         DebugMSG( "BOOT SERVICE #30 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_DisconnectController(void)
{
         DebugMSG( "BOOT SERVICE #31 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_OpenProtocol( EFI_HANDLE  UserHandle,
                                                            EFI_GUID    *Protocol,
                                                            VOID        **Interface,
                                                            EFI_HANDLE  ImageHandle,
                                                            EFI_HANDLE  ControllerHandle,
                                                            UINT32      Attributes )

{
        const char* protocolName = GetGuidName( Protocol );
        DebugMSG( "handle = 0x%px guid = %s: %s",
                   UserHandle, protocolName, get_GUID_str( Protocol ) );

        if (strcmp (protocolName, "gEfiSimpleTextInputExProtocolGuid") == 0) {
                return efi_handle_protocol_SimpleTextInputExProtocol(
                                                        UserHandle, Interface );
        }

        return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CloseProtocol(void)
{
         DebugMSG( "BOOT SERVICE #33 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_OpenProtocolInformation(void)
{
         DebugMSG( "BOOT SERVICE #34 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_ProtocolsPerHandle(void)
{
         DebugMSG( "BOOT SERVICE #35 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_LocateHandleBuffer(void)
{
         DebugMSG( "BOOT SERVICE #36 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_LocateProtocol(void)
{
         DebugMSG( "BOOT SERVICE #37 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_InstallMultipleProtocolInterfaces(void)
{
         DebugMSG( "BOOT SERVICE #38 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_UninstallMultipleProtocolInterfaces(void)
{
         DebugMSG( "BOOT SERVICE #39 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CalculateCrc32(void)
{
         DebugMSG( "BOOT SERVICE #40 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CopyMem(void)
{
         DebugMSG( "BOOT SERVICE #41 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_SetMem(void)
{
         DebugMSG( "BOOT SERVICE #42 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_hook_CreateEventEx(void)
{
         DebugMSG( "BOOT SERVICE #43 called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_Reset(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

void wchar_to_ascii( char *dst_ascii, size_t len, char* src_wchar)
{
        /* src_wcharis CHAR16. We convert it to char* by skipping every
         * 2nd char */
        unsigned int currIdx = 0;
        char c;

        while (currIdx < len)
        {
                c = src_wchar[currIdx*2];
                if (c == 0)
                        break;

                dst_ascii[currIdx++] = c;
        }
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_OutputString(void* this,
                                                                  char* str )
{
        char str_as_ascii[1024] = {0};
        wchar_to_ascii( str_as_ascii, sizeof( str_as_ascii ), str );

        DebugMSG( "output: %s", str_as_ascii );

        return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_TestString(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_QueryMode(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_SetMode(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_SetAttribute(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_ClearScreen(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_SetCursorPosition(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_conout_hook_EnableCursor(void)
{
         DebugMSG( "ConOut was called" );

         return EFI_UNSUPPORTED;
}

EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con_out = {
        .Reset             = efi_conout_hook_Reset,
        .OutputString      = efi_conout_hook_OutputString,
        .TestString        = efi_conout_hook_TestString,
        .QueryMode         = efi_conout_hook_QueryMode,
        .SetMode           = efi_conout_hook_SetMode,
        .SetAttribute      = efi_conout_hook_SetAttribute,
        .ClearScreen       = efi_conout_hook_ClearScreen,
        .SetCursorPosition = efi_conout_hook_SetCursorPosition,
        .EnableCursor      = efi_conout_hook_EnableCursor,

        .Mode = NULL
};

void* efi_boot_service_hooks[44] = {0};

void initialize_efi_boot_service_hooks(void)
{
        efi_boot_service_hooks[0] = efi_hook_RaiseTPL;
        efi_boot_service_hooks[1] = efi_hook_RestoreTPL;
        efi_boot_service_hooks[2] = efi_hook_AllocatePages;
        efi_boot_service_hooks[3] = efi_hook_FreePages;
        efi_boot_service_hooks[4] = efi_hook_GetMemoryMap;
        efi_boot_service_hooks[5] = efi_hook_AllocatePool;
        efi_boot_service_hooks[6] = efi_hook_FreePool;
        efi_boot_service_hooks[7] = efi_hook_CreateEvent;
        efi_boot_service_hooks[8] = efi_hook_SetTimer;
        efi_boot_service_hooks[9] = efi_hook_WaitForEvent;
        efi_boot_service_hooks[10] = efi_hook_SignalEvent;
        efi_boot_service_hooks[11] = efi_hook_CloseEvent;
        efi_boot_service_hooks[12] = efi_hook_CheckEvent;
        efi_boot_service_hooks[13] = efi_hook_InstallProtocolInterface;
        efi_boot_service_hooks[14] = efi_hook_ReinstallProtocolInterface;
        efi_boot_service_hooks[15] = efi_hook_UninstallProtocolInterface;
        efi_boot_service_hooks[16] = efi_hook_HandleProtocol;
        efi_boot_service_hooks[17] = efi_hook_Reserved;
        efi_boot_service_hooks[18] = efi_hook_RegisterProtocolNotify;
        efi_boot_service_hooks[19] = efi_hook_LocateHandle;
        efi_boot_service_hooks[20] = efi_hook_LocateDevicePath;
        efi_boot_service_hooks[21] = efi_hook_InstallConfigurationTable;
        efi_boot_service_hooks[22] = efi_hook_LoadImage;
        efi_boot_service_hooks[23] = efi_hook_StartImage;
        efi_boot_service_hooks[24] = efi_hook_Exit;
        efi_boot_service_hooks[25] = efi_hook_UnloadImage;
        efi_boot_service_hooks[26] = efi_hook_ExitBootServices;
        efi_boot_service_hooks[27] = efi_hook_GetNextMonotonicCount;
        efi_boot_service_hooks[28] = efi_hook_Stall;
        efi_boot_service_hooks[29] = efi_hook_SetWatchdogTimer;
        efi_boot_service_hooks[30] = efi_hook_ConnectController;
        efi_boot_service_hooks[31] = efi_hook_DisconnectController;
        efi_boot_service_hooks[32] = efi_hook_OpenProtocol;
        efi_boot_service_hooks[33] = efi_hook_CloseProtocol;
        efi_boot_service_hooks[34] = efi_hook_OpenProtocolInformation;
        efi_boot_service_hooks[35] = efi_hook_ProtocolsPerHandle;
        efi_boot_service_hooks[36] = efi_hook_LocateHandleBuffer;
        efi_boot_service_hooks[37] = efi_hook_LocateProtocol;
        efi_boot_service_hooks[38] = efi_hook_InstallMultipleProtocolInterfaces;
        efi_boot_service_hooks[39] = efi_hook_UninstallMultipleProtocolInterfaces;
        efi_boot_service_hooks[40] = efi_hook_CalculateCrc32;
        efi_boot_service_hooks[41] = efi_hook_CopyMem;
        efi_boot_service_hooks[42] = efi_hook_SetMem;
        efi_boot_service_hooks[43] = efi_hook_CreateEventEx;
}

efi_time_t fake_time = {
        .year       = 2019,
        .month      = 1,
        .day        = 1,
        .hour       = 10,
        .minute     = 0,
        .second     = 0,
        .pad1       = 0,
        .nanosecond = 0,
        .timezone   = 0,
        .daylight   = 0,
        .pad2       = 0
};

__attribute__((ms_abi)) efi_status_t efi_runtime_get_time(efi_time_t *tm,
                                                          efi_time_cap_t *tc )
{
         DebugMSG( "tm @ %px, tc @ %px", tm, tc );
         memcpy( tm, &fake_time, sizeof( current_time ) );

         return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_set_time(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_get_wakeup_time(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_set_wakeup_time(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_set_virtual_address_map(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_convert_pointer(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_get_variable(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_get_next_variable(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

 __attribute__((ms_abi)) efi_status_t efi_runtime_set_variable(
                                                CHAR16        *name,
                                                EFI_GUID      *vendor,
					        u32           attr,
                                                unsigned long data_size,
					        void          *data )

{
        char str_as_ascii[1024] = {0};
        wchar_to_ascii( str_as_ascii, sizeof( str_as_ascii ), (char*)name );

        DebugMSG( "name: %s, vendor = %s (%s), data_size = %ld",
                  str_as_ascii, GetGuidName( vendor ),
                  get_GUID_str( vendor ), data_size );

        return EFI_SUCCESS;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_get_next_high_mono_count(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_reset_system(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_update_capsule(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_query_capsule_caps(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

__attribute__((ms_abi)) efi_status_t efi_runtime_query_variable_info(void)
{
         DebugMSG( "Runtime service was called" );

         return EFI_UNSUPPORTED;
}

efi_runtime_services_t runtime_services = {
        .hdr                        = {0},
        .get_time                   = (void*)efi_runtime_get_time,
        .set_time                   = (void*)efi_runtime_set_time,
        .get_wakeup_time            = (void*)efi_runtime_get_wakeup_time,
        .set_wakeup_time            = (void*)efi_runtime_set_wakeup_time,
        .set_virtual_address_map    = (void*)efi_runtime_set_virtual_address_map,
        .convert_pointer            = (void*)efi_runtime_convert_pointer,
        .get_variable               = (void*)efi_runtime_get_variable,
        .get_next_variable          = (void*)efi_runtime_get_next_variable,
        .set_variable               = (void*)efi_runtime_set_variable,
        .get_next_high_mono_count   = (void*)efi_runtime_get_next_high_mono_count,
        .reset_system               = (void*)efi_runtime_reset_system,
        .update_capsule             = (void*)efi_runtime_update_capsule,
        .query_capsule_caps         = (void*)efi_runtime_query_capsule_caps,
        .query_variable_info        = (void*)efi_runtime_query_variable_info
};

static void hook_boot_services( efi_system_table_t *systab )

{
        efi_boot_services_t *boot_services       = &linux_bootservices;
        void                **bootServiceFuncPtr = NULL;
        int                 boot_service_idx     = 0;
        uint64_t            top_of_bootservices;

        uint64_t            *systab_blob         = (uint64_t *)systab;
        uint64_t            marker               = 0xdeadbeefcafeba00;

        /*
         * Fill boot services table with known incrementing  values
         * This will help debugging when we see RIP or other registers
         * containing theses fixed values */
        while ( (uint8_t*)systab_blob < (uint8_t*)systab + sizeof( *systab ) ) {
                *systab_blob = marker++;
                systab_blob += 1;
        }

        systab->con_in_handle  = CON_IN_HANDLE;
        systab->con_in         = 0xdeadbeefcafe0001;
        systab->con_out_handle = 0xdeadbeefcafebabe;
        systab->con_out        = (unsigned long) &con_out;
        systab->stderr_handle  = 0xdeadbeefcafe0003;
        systab->stderr         = 0xdeadbeefcafe0004;
        systab->runtime        = &runtime_services;
        /*
         * We will fill boot_services with actual function pointer, but this is
         * a precaution in case we missed a function pointer in our setup. */
        memset(boot_services, 0x43, sizeof( *boot_services ) );

        initialize_efi_boot_service_hooks();
        bootServiceFuncPtr  = &boot_services->raise_tpl; /* This is the first service */
        top_of_bootservices =
                (uint64_t)boot_services + sizeof( efi_boot_services_t );

        /* Now assign the function poointers: */
        while( (uint64_t)bootServiceFuncPtr < top_of_bootservices ) {
                *bootServiceFuncPtr = efi_boot_service_hooks[boot_service_idx];
                bootServiceFuncPtr += 1;
                boot_service_idx   += 1;
        }

        systab->boottime = boot_services;
}

typedef uint64_t (*EFI_APP_ENTRY)( void* imageHandle, void* systemTable  )
        __attribute__((ms_abi));

void launch_efi_app(EFI_APP_ENTRY efiApp, efi_system_table_t *systab)
{
        /* Fake handle */
        EFI_HANDLE          ImageHandle   = (void*)0xDEADBEEF;

        /* We need to create a large pool of EfiConventionalMemory, so Windows
         * loader will believe there is sufficient memory. Otherwise it won't
         * even call the EFI AllocatePages function and fail with error code
         * 0xC0000017 (STATUS_NO_MEMORY) */
        efi_physical_addr_t pool          = 0x100000;
        UINTN               pool_pages    = 200;

        efi_hook_AllocatePages( AllocateAnyPages, EfiConventionalMemory,
                                pool_pages, &pool );
        efiApp( ImageHandle, systab );
}

void kimage_run_pe(struct kimage *image)
{
        EFI_APP_ENTRY efiApp = (EFI_APP_ENTRY)image->raw_image_start;

        /* Print the beginning of the entry point. You can compare this to the
         * objdump output of the EFI app you're running. */
        DumpBuffer( "Entry point:", (uint8_t*) image->raw_image_start, 64 );

        hook_boot_services( &fake_systab );
        efiApp = (EFI_APP_ENTRY)image->raw_image_start;
        launch_efi_app( efiApp, &fake_systab );
}

static int do_kexec_load(unsigned long entry, unsigned long nr_segments,
		struct kexec_segment __user *segments, unsigned long flags)
{
	struct kimage **dest_image, *image;
	unsigned long i;
	int ret;

	if (flags & KEXEC_ON_CRASH) {
		dest_image = &kexec_crash_image;
		if (kexec_crash_image)
			arch_kexec_unprotect_crashkres();
	} else {
		dest_image = &kexec_image;
	}

	if (nr_segments == 0) {
		/* Uninstall image */
		kimage_free(xchg(dest_image, NULL));
		return 0;
	}
	if (flags & KEXEC_ON_CRASH) {
		/*
		 * Loading another kernel to switch to if this one
		 * crashes.  Free any current crash dump kernel before
		 * we corrupt it.
		 */
		kimage_free(xchg(&kexec_crash_image, NULL));
	}

	ret = kimage_alloc_init(&image, entry, nr_segments, segments, flags);
	if (ret)
		return ret;

        if (flags & KEXEC_RUN_PE) {
                kimage_load_pe(image, nr_segments);
                kimage_run_pe(image);

                goto out;
        }

	if (flags & KEXEC_PRESERVE_CONTEXT)
		image->preserve_context = 1;

	ret = machine_kexec_prepare(image);
	if (ret)
		goto out;

	for (i = 0; i < nr_segments; i++) {
		ret = kimage_load_segment(image, &image->segment[i]);
		if (ret)
			goto out;
	}

	kimage_terminate(image);

	/* Install the new kernel and uninstall the old */
	image = xchg(dest_image, image);

out:
	if ((flags & KEXEC_ON_CRASH) && kexec_crash_image)
		arch_kexec_protect_crashkres();

	kimage_free(image);
	return ret;
}

/*
 * Exec Kernel system call: for obvious reasons only root may call it.
 *
 * This call breaks up into three pieces.
 * - A generic part which loads the new kernel from the current
 *   address space, and very carefully places the data in the
 *   allocated pages.
 *
 * - A generic part that interacts with the kernel and tells all of
 *   the devices to shut down.  Preventing on-going dmas, and placing
 *   the devices in a consistent state so a later kernel can
 *   reinitialize them.
 *
 * - A machine specific part that includes the syscall number
 *   and then copies the image to it's final destination.  And
 *   jumps into the image at entry.
 *
 * kexec does not sync, or unmount filesystems so if you need
 * that to happen you need to do that yourself.
 */

SYSCALL_DEFINE4(kexec_load, unsigned long, entry, unsigned long, nr_segments,
		struct kexec_segment __user *, segments, unsigned long, flags)
{
	int result;

	/* We only trust the superuser with rebooting the system. */
	if (!capable(CAP_SYS_BOOT) || kexec_load_disabled)
		return -EPERM;

	/*
	 * Verify we have a legal set of flags
	 * This leaves us room for future extensions.
	 */
	if ((flags & KEXEC_FLAGS) != (flags & ~KEXEC_ARCH_MASK))
		return -EINVAL;

	/* Verify we are on the appropriate architecture */
	if (((flags & KEXEC_ARCH_MASK) != KEXEC_ARCH) &&
		((flags & KEXEC_ARCH_MASK) != KEXEC_ARCH_DEFAULT))
		return -EINVAL;

	/* Put an artificial cap on the number
	 * of segments passed to kexec_load.
	 */
	if (nr_segments > KEXEC_SEGMENT_MAX)
		return -EINVAL;

	/* Because we write directly to the reserved memory
	 * region when loading crash kernels we need a mutex here to
	 * prevent multiple crash  kernels from attempting to load
	 * simultaneously, and to prevent a crash kernel from loading
	 * over the top of a in use crash kernel.
	 *
	 * KISS: always take the mutex.
	 */
	if (!mutex_trylock(&kexec_mutex))
		return -EBUSY;

	result = do_kexec_load(entry, nr_segments, segments, flags);

	mutex_unlock(&kexec_mutex);

	return result;
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(kexec_load, compat_ulong_t, entry,
		       compat_ulong_t, nr_segments,
		       struct compat_kexec_segment __user *, segments,
		       compat_ulong_t, flags)
{
	struct compat_kexec_segment in;
	struct kexec_segment out, __user *ksegments;
	unsigned long i, result;

	/* Don't allow clients that don't understand the native
	 * architecture to do anything.
	 */
	if ((flags & KEXEC_ARCH_MASK) == KEXEC_ARCH_DEFAULT)
		return -EINVAL;

	if (nr_segments > KEXEC_SEGMENT_MAX)
		return -EINVAL;

	ksegments = compat_alloc_user_space(nr_segments * sizeof(out));
	for (i = 0; i < nr_segments; i++) {
		result = copy_from_user(&in, &segments[i], sizeof(in));
		if (result)
			return -EFAULT;

		out.buf   = compat_ptr(in.buf);
		out.bufsz = in.bufsz;
		out.mem   = in.mem;
		out.memsz = in.memsz;

		result = copy_to_user(&ksegments[i], &out, sizeof(out));
		if (result)
			return -EFAULT;
	}

	return sys_kexec_load(entry, nr_segments, ksegments, flags);
}
#endif
