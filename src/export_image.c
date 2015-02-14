/*
 * export_image.c
 */

/*
 * Copyright (C) 2012, 2013, 2014 Eric Biggers
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option) any
 * later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib.h"
#include "wimlib/error.h"
#include "wimlib/inode.h"
#include "wimlib/blob_table.h"
#include "wimlib/metadata.h"
#include "wimlib/xml.h"

static int
blob_set_not_exported(struct blob_info *blob, void *_ignore)
{
	blob->out_refcnt = 0;
	blob->was_exported = 0;
	return 0;
}

static int
blob_rollback_export(struct blob_info *blob, void *_blob_table)
{
	struct blob_table *blob_table = _blob_table;

	blob->refcnt -= blob->out_refcnt;
	if (blob->was_exported) {
		blob_table_unlink(blob_table, blob);
		free_blob_info(blob);
	}
	return 0;
}

static int
inode_export_streams(struct wim_inode *inode,
		     struct blob_table *src_blob_table,
		     struct blob_table *dest_blob_table,
		     bool gift)
{
	unsigned i;
	const u8 *hash;
	struct blob_info *src_blob, *dest_blob;

	inode_unresolve_streams(inode);
	for (i = 0; i <= inode->i_num_ads; i++) {

		/* Retrieve SHA1 message digest of stream to export.  */
		hash = inode_stream_hash(inode, i);
		if (is_zero_hash(hash))  /* Empty stream?  */
			continue;

		/* Search for the stream (via SHA1 message digest) in the
		 * destination WIM.  */
		dest_blob = lookup_blob(dest_blob_table, hash);
		if (!dest_blob) {
			/* Stream not yet present in destination WIM.  Search
			 * for it in the source WIM, then export it into the
			 * destination WIM.  */
			src_blob = lookup_blob(src_blob_table, hash);
			if (!src_blob)
				return stream_not_found_error(inode, hash);

			if (gift) {
				dest_blob = src_blob;
				blob_table_unlink(src_blob_table, src_blob);
			} else {
				dest_blob = clone_blob_info(src_blob);
				if (!dest_blob)
					return WIMLIB_ERR_NOMEM;
			}
			dest_blob->refcnt = 0;
			dest_blob->out_refcnt = 0;
			dest_blob->was_exported = 1;
			blob_table_insert(dest_blob_table, dest_blob);
		}

		/* Stream is present in destination WIM (either pre-existing,
		 * already exported, or just exported above).  Increment its
		 * reference count appropriately.   Note: we use 'refcnt' for
		 * the raw reference count, but 'out_refcnt' for references
		 * arising just from the export operation; this is used to roll
		 * back a failed export if needed.  */
		dest_blob->refcnt += inode->i_nlink;
		dest_blob->out_refcnt += inode->i_nlink;
	}
	return 0;
}

/* API function documented in wimlib.h  */
WIMLIBAPI int
wimlib_export_image(WIMStruct *src_wim,
		    int src_image,
		    WIMStruct *dest_wim,
		    const tchar *dest_name,
		    const tchar *dest_description,
		    int export_flags)
{
	int ret;
	int start_src_image;
	int end_src_image;
	int orig_dest_image_count;
	int image;
	bool all_images = (src_image == WIMLIB_ALL_IMAGES);

	/* Check for sane parameters.  */
	if (export_flags & ~(WIMLIB_EXPORT_FLAG_BOOT |
			     WIMLIB_EXPORT_FLAG_NO_NAMES |
			     WIMLIB_EXPORT_FLAG_NO_DESCRIPTIONS |
			     WIMLIB_EXPORT_FLAG_GIFT |
			     WIMLIB_EXPORT_FLAG_WIMBOOT))
		return WIMLIB_ERR_INVALID_PARAM;

	if (src_wim == NULL || dest_wim == NULL)
		return WIMLIB_ERR_INVALID_PARAM;

	if (!wim_has_metadata(src_wim) || !wim_has_metadata(dest_wim))
		return WIMLIB_ERR_METADATA_NOT_FOUND;

	if (all_images) {
		/* Multi-image export.  */
		if ((!(export_flags & WIMLIB_EXPORT_FLAG_NO_NAMES) &&
			dest_name) ||
		    (!(export_flags & WIMLIB_EXPORT_FLAG_NO_DESCRIPTIONS) &&
			dest_description))
		{
			ERROR("Image name and description must be "
			      "left NULL for multi-image export");
			return WIMLIB_ERR_INVALID_PARAM;
		}
		start_src_image = 1;
		end_src_image = src_wim->hdr.image_count;
	} else {
		start_src_image = src_image;
		end_src_image = src_image;
	}
	orig_dest_image_count = dest_wim->hdr.image_count;

	/* Stream checksums must be known before proceeding.  */
	ret = wim_checksum_unhashed_blobs(src_wim);
	if (ret)
		return ret;
	ret = wim_checksum_unhashed_blobs(dest_wim);
	if (ret)
		return ret;

	/* Enable rollbacks  */
	for_blob_info(dest_wim->blob_table, blob_set_not_exported, NULL);

	/* Export each requested image.  */
	for (src_image = start_src_image;
	     src_image <= end_src_image;
	     src_image++)
	{
		const tchar *next_dest_name, *next_dest_description;
		struct wim_image_metadata *src_imd;
		struct wim_inode *inode;

		/* Determine destination image name and description.  */

		if (export_flags & WIMLIB_EXPORT_FLAG_NO_NAMES)
			next_dest_name = T("");
		else if (dest_name)
			next_dest_name = dest_name;
		else
			next_dest_name = wimlib_get_image_name(src_wim, src_image);

		if (export_flags & WIMLIB_EXPORT_FLAG_NO_DESCRIPTIONS)
			next_dest_description = T("");
		else if (dest_description)
			next_dest_description = dest_description;
		else
			next_dest_description = wimlib_get_image_description(src_wim, src_image);

		/* Check for name conflict.  */
		if (wimlib_image_name_in_use(dest_wim, next_dest_name)) {
			ERROR("There is already an image named \"%"TS"\" "
			      "in the destination WIM", next_dest_name);
			ret = WIMLIB_ERR_IMAGE_NAME_COLLISION;
			goto out_rollback;
		}

		/* Load metadata for source image into memory.  */
		ret = select_wim_image(src_wim, src_image);
		if (ret)
			goto out_rollback;

		src_imd = wim_get_current_image_metadata(src_wim);

		/* Iterate through inodes in the source image and export their
		 * streams into the destination WIM.  */
		image_for_each_inode(inode, src_imd) {
			ret = inode_export_streams(inode,
						   src_wim->blob_table,
						   dest_wim->blob_table,
						   export_flags & WIMLIB_EXPORT_FLAG_GIFT);
			if (ret)
				goto out_rollback;
		}

		/* Export XML information into the destination WIM.  */
		ret = xml_export_image(src_wim->wim_info, src_image,
				       &dest_wim->wim_info, next_dest_name,
				       next_dest_description);
		if (ret)
			goto out_rollback;

		/* Reference the source image metadata from the destination WIM.
		 */
		ret = append_image_metadata(dest_wim, src_imd);
		if (ret)
			goto out_rollback;
		src_imd->refcnt++;

		/* Lock the metadata into memory.  XXX: need better solution for
		 * this.  */
		src_imd->modified = 1;

	}

	/* Image export complete.  Finish by setting any needed special metadata
	 * on the destination WIM.  */

	if (src_wim->hdr.flags & WIM_HDR_FLAG_RP_FIX)
		dest_wim->hdr.flags |= WIM_HDR_FLAG_RP_FIX;

	for (src_image = start_src_image;
	     src_image <= end_src_image;
	     src_image++)
	{
		int dst_image = orig_dest_image_count + 1 +
				(src_image - start_src_image);

		if (export_flags & WIMLIB_EXPORT_FLAG_WIMBOOT)
			wim_info_set_wimboot(dest_wim->wim_info, dst_image, true);

		if ((export_flags & WIMLIB_EXPORT_FLAG_BOOT) &&
		    (!all_images || src_image == src_wim->hdr.boot_idx))
			dest_wim->hdr.boot_idx = dst_image;
	}

	if (export_flags & WIMLIB_EXPORT_FLAG_GIFT) {
		free_blob_table(src_wim->blob_table);
		src_wim->blob_table = NULL;
	}
	return 0;

out_rollback:
	while ((image = wim_info_get_num_images(dest_wim->wim_info))
	       > orig_dest_image_count)
	{
		xml_delete_image(&dest_wim->wim_info, image);
	}
	while (dest_wim->hdr.image_count > orig_dest_image_count)
	{
		put_image_metadata(dest_wim->image_metadata[
					--dest_wim->hdr.image_count], NULL);
	}
	for_blob_info(dest_wim->blob_table, blob_rollback_export,
			       dest_wim->blob_table);
	return ret;
}
