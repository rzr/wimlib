#ifndef _WIMLIB_HEADER_H
#define _WIMLIB_HEADER_H

#include "wimlib/resource.h"
#include "wimlib/types.h"

#define WIM_MAGIC_LEN  8
#define WIM_GID_LEN    16
#define WIM_UNUSED_LEN 60

/* Length of the WIM header on disk. */
#define WIM_HEADER_DISK_SIZE (148 + WIM_UNUSED_LEN)

/* Compressed resources in the WIM are divided into separated compressed chunks
 * of this size. */
#define WIM_CHUNK_SIZE 32768

/* Version of the WIM file.  There is an older version, but we don't support it
 * yet.  The differences between the versions are undocumented. */
#define WIM_VERSION 0x10d00

/* Header at the very beginning of the WIM file. */
struct wim_header {
	/* Identifies the file as WIM file. Must be exactly
	 * {'M', 'S', 'W', 'I', 'M', 0, 0, 0}  */
	//u8  magic[WIM_MAGIC_LEN];

	/* size of WIM header in bytes. */
	//u32 hdr_size;

	/* Version of the WIM file.  Microsoft provides no documentation about
	 * exactly what this field affects about the file format, other than the
	 * fact that more recent versions have a higher value. */
	//u32 version;

	/* Bitwise OR of one or more of the WIM_HDR_FLAG_* defined below. */
	u32 flags;

	/* The size of the pieces that the uncompressed files were split up into
	 * when they were compressed.  This should be the same as
	 * WIM_CHUNK_SIZE.  Microsoft incorrectly documents this as "the size of
	 * the compressed .wim file in bytes".*/
	//u32 chunk_size;

	/* A unique identifier for the WIM file. */
	u8 guid[WIM_GID_LEN];

	/* Part number of the WIM file in a spanned set. */
	u16 part_number;

	/* Total number of parts in a spanned set. */
	u16 total_parts;

	/* Number of images in the WIM file. */
	u32 image_count;

	/* Location, size, and flags of the lookup table of the WIM. */
	struct resource_entry lookup_table_res_entry;

	/* Location, size, and flags for the XML data of the WIM. */
	struct resource_entry xml_res_entry;

	/* Location, size, and flags for the boot metadata.  This means the
	 * metadata resource for the image specified by boot_idx below.  Should
	 * be zeroed out if boot_idx is 0. */
	struct resource_entry boot_metadata_res_entry;

	/* The index of the bootable image in the WIM file. If 0, there are no
	 * bootable images available. */
	u32 boot_idx;

	/* The location of the optional integrity table used to verify the
	 * integrity WIM.  Zeroed out if there is no integrity table.*/
	struct resource_entry integrity;

	/* Reserved for future disuse */
	//u8 unused[WIM_UNUSED_LEN];
};

/* Flags for the `flags' field of the struct wim_header: */

/* Reserved for future use */
#define WIM_HDR_FLAG_RESERVED           0x00000001

/* Files and metadata in the WIM are compressed. */
#define WIM_HDR_FLAG_COMPRESSION        0x00000002

/* WIM is read-only (wimlib ignores this because it's pretty much pointless) */
#define WIM_HDR_FLAG_READONLY           0x00000004

/* Resource data specified by images in this WIM may be contained in a different
 * WIM.  Or in other words, this WIM is part of a split WIM.  */
#define WIM_HDR_FLAG_SPANNED            0x00000008

/* The WIM contains resources only; no filesystem metadata.  wimlib ignores this
 * flag, as it looks for resources in all the WIMs anyway. */
#define WIM_HDR_FLAG_RESOURCE_ONLY      0x00000010

/* The WIM contains metadata only.  wimlib ignores this flag.  Note that all the
 * metadata resources for a split WIM should be in the first part. */
#define WIM_HDR_FLAG_METADATA_ONLY      0x00000020

/* Lock field to prevent multiple writers from writing the WIM concurrently.
 * wimlib ignores this flag as it uses flock() to acquire a real lock on the
 * file (if supported by the underlying filesystem). */
#define WIM_HDR_FLAG_WRITE_IN_PROGRESS  0x00000040

/* Reparse point fixup flag.  See docs for --rpfix and --norpfix in imagex, or
 * WIMLIB_ADD_FLAG_{RPFIX,NORPFIX} in wimlib.h.  Note that
 * WIM_HDR_FLAG_RP_FIX is a header flag and just sets the default behavior for
 * the WIM; it can still be overridder on a per-image basis.  But there is no
 * flag to set the default behavior for a specific image. */
#define WIM_HDR_FLAG_RP_FIX             0x00000080

/* Unused, reserved flag for another compression type */
#define WIM_HDR_FLAG_COMPRESS_RESERVED  0x00010000

/* Resources within the WIM are compressed using "XPRESS" compression, which is
 * a LZ77-based compression algorithm. */
#define WIM_HDR_FLAG_COMPRESS_XPRESS    0x00020000

/* Resources within the WIM are compressed using "LZX" compression.  This is also
 * a LZ77-based algorithm. */
#define WIM_HDR_FLAG_COMPRESS_LZX       0x00040000

#endif /* _WIMLIB_HEADER_H */