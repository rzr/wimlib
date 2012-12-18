/*
 * mount_image.c
 *
 * This file implements mounting of WIM files using FUSE, which stands for
 * Filesystem in Userspace.  FUSE allows a filesystem to be implemented in a
 * userspace process by implementing the filesystem primitives--- read(),
 * write(), readdir(), etc.
 */

/*
 * Copyright (C) 2012 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#include "wimlib_internal.h"

#ifdef WITH_FUSE

#include "sha1.h"
#include "lookup_table.h"
#include "xml.h"
#include "buffer_io.h"
#include "timestamp.h"
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#define FUSE_USE_VERSION 26
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <fuse.h>
#include <ftw.h>
#include <mqueue.h>
#include <utime.h>

#ifdef ENABLE_XATTR
#include <attr/xattr.h>
#endif

#define MSG_VERSION_TOO_HIGH	-1
#define MSG_BREAK_LOOP		-2

/* File descriptor to a file open on the WIM filesystem. */
struct wimlib_fd {
	struct inode *f_inode;
	struct lookup_table_entry *f_lte;
	int staging_fd;
	u16 idx;
	u32 stream_id;
};

struct wimfs_context {
	/* The WIMStruct for the mounted WIM. */
	WIMStruct *wim;

	/* Name of the staging directory for a read-write mount.  Whenever a new file is
	 * created, it is done so in the staging directory.  Furthermore, whenever a
	 * file in the WIM is modified, it is extracted to the staging directory.  If
	 * changes are commited when the WIM is unmounted, the file resources are merged
	 * in from the staging directory when writing the new WIM. */
	char *staging_dir_name;
	size_t staging_dir_name_len;

	/* Flags passed to wimlib_mount(). */
	int mount_flags;

	int default_lookup_flags;

	/* Next inode number to be assigned. */
	u64 next_ino;

	/* List of lookup table entries in the staging directory */
	struct list_head staging_list;

	/* List of inodes in the mounted image */
	struct hlist_head *image_inode_list;

	/* Name and message queue descriptors for message queues between the filesystem
	 * daemon process and the unmount process.  These are used when the filesystem
	 * is unmounted and the process running wimlib_mount() (i.e. the `imagex
	 * unmount' command) needs to communicate with the filesystem daemon running
	 * fuse_main() (i.e. that spawned by the `imagex mount' or `imagex mountrw'
	 * commands */
	char *unmount_to_daemon_mq_name;
	char *daemon_to_unmount_mq_name;
	mqd_t unmount_to_daemon_mq;
	mqd_t daemon_to_unmount_mq;
};

static void init_wimfs_context(struct wimfs_context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->unmount_to_daemon_mq = (mqd_t)-1;
	ctx->daemon_to_unmount_mq = (mqd_t)-1;
	INIT_LIST_HEAD(&ctx->staging_list);
}

static inline struct wimfs_context *wimfs_get_context()
{
	return (struct wimfs_context*)fuse_get_context()->private_data;
}

static inline WIMStruct *wimfs_get_WIMStruct()
{
	return wimfs_get_context()->wim;
}

static inline bool wimfs_ctx_readonly(const struct wimfs_context *ctx)
{
	return (ctx->mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) == 0;
}

static inline int get_lookup_flags(const struct wimfs_context *ctx)
{
	return ctx->default_lookup_flags;
}

/* Returns nonzero if write permission is requested on the file open flags */
static inline int flags_writable(int open_flags)
{
	return open_flags & (O_RDWR | O_WRONLY);
}

/*
 * Allocate a file descriptor for a stream.
 *
 * @inode:	inode containing the stream we're opening
 * @stream_id:	ID of the stream we're opening
 * @lte:	Lookup table entry for the stream (may be NULL)
 * @fd_ret:	Return the allocated file descriptor if successful.
 * @readonly:	True if this is a read-only mount.
 *
 * Return 0 iff successful or error code if unsuccessful.
 */
static int alloc_wimlib_fd(struct inode *inode,
			   u32 stream_id,
			   struct lookup_table_entry *lte,
			   struct wimlib_fd **fd_ret,
			   bool readonly)
{
	static const u16 fds_per_alloc = 8;
	static const u16 max_fds = 0xffff;
	int ret;

	pthread_mutex_lock(&inode->i_mutex);

	DEBUG("Allocating fd for stream ID %u from inode %lx (open = %u, allocated = %u)",
	      stream_id, inode->ino, inode->num_opened_fds,
	      inode->num_allocated_fds);

	if (inode->num_opened_fds == inode->num_allocated_fds) {
		struct wimlib_fd **fds;
		u16 num_new_fds;

		if (inode->num_allocated_fds == max_fds) {
			ret = -EMFILE;
			goto out;
		}
		num_new_fds = min(fds_per_alloc,
				  max_fds - inode->num_allocated_fds);

		fds = REALLOC(inode->fds,
			      (inode->num_allocated_fds + num_new_fds) *
			        sizeof(inode->fds[0]));
		if (!fds) {
			ret = -ENOMEM;
			goto out;
		}
		memset(&fds[inode->num_allocated_fds], 0,
		       num_new_fds * sizeof(fds[0]));
		inode->fds = fds;
		inode->num_allocated_fds += num_new_fds;
	}
	for (u16 i = 0; ; i++) {
		if (!inode->fds[i]) {
			struct wimlib_fd *fd = CALLOC(1, sizeof(*fd));
			if (!fd) {
				ret = -ENOMEM;
				break;
			}
			fd->f_inode    = inode;
			fd->f_lte      = lte;
			fd->staging_fd = -1;
			fd->idx        = i;
			fd->stream_id  = stream_id;
			*fd_ret        = fd;
			inode->fds[i]  = fd;
			inode->num_opened_fds++;
			if (lte && !readonly)
				lte->num_opened_fds++;
			DEBUG("Allocated fd (idx = %u)", fd->idx);
			ret = 0;
			break;
		}
	}
out:
	pthread_mutex_unlock(&inode->i_mutex);
	return ret;
}

static void inode_put_fd(struct inode *inode, struct wimlib_fd *fd)
{
	wimlib_assert(inode != NULL);

	pthread_mutex_lock(&inode->i_mutex);

	wimlib_assert(fd->f_inode == inode);
	wimlib_assert(inode->num_opened_fds != 0);
	wimlib_assert(fd->idx < inode->num_allocated_fds);
	wimlib_assert(inode->fds[fd->idx] == fd);

	inode->fds[fd->idx] = NULL;
	FREE(fd);
	if (--inode->num_opened_fds == 0 && inode->link_count == 0) {
		pthread_mutex_unlock(&inode->i_mutex);
		free_inode(inode);
	} else {
		pthread_mutex_unlock(&inode->i_mutex);
	}
}

static int lte_put_fd(struct lookup_table_entry *lte, struct wimlib_fd *fd)
{
	wimlib_assert(fd->f_lte == lte);

	if (!lte) /* Empty stream with no lookup table entry */
		return 0;

	/* Close staging file descriptor if needed. */

	if (lte->resource_location == RESOURCE_IN_STAGING_FILE
	     && fd->staging_fd != -1)
	{
		if (close(fd->staging_fd) != 0) {
			ERROR_WITH_ERRNO("Failed to close staging file");
			return -errno;
		}
	}
	lte_decrement_num_opened_fds(lte);
	return 0;
}

/* Close a file descriptor. */
static int close_wimlib_fd(struct wimlib_fd *fd)
{
	int ret;
	DEBUG("Closing fd (inode = %lu, opened = %u, allocated = %u)",
	      fd->f_inode->ino, fd->f_inode->num_opened_fds,
	      fd->f_inode->num_allocated_fds);
	ret = lte_put_fd(fd->f_lte, fd);
	if (ret != 0)
		return ret;

	inode_put_fd(fd->f_inode, fd);
	return 0;
}

static int create_dentry(struct wimfs_context *ctx, const char *path,
			 struct dentry **dentry_ret)
{
	struct dentry *parent;
	struct dentry *new;
	const char *basename;

	parent = get_parent_dentry(ctx->wim, path);
	if (!parent)
		return -ENOENT;

	if (!dentry_is_directory(parent))
		return -ENOTDIR;

	basename = path_basename(path);
	if (get_dentry_child_with_name(parent, basename))
		return -EEXIST;

	new = new_dentry_with_inode(basename);
	if (!new)
		return -ENOMEM;

	new->d_inode->resolved = 1;
	new->d_inode->ino = ctx->next_ino++;
	dentry_add_child(parent, new);
	hlist_add_head(&new->d_inode->hlist, ctx->image_inode_list);
	*dentry_ret = new;
	return 0;
}

/* Remove a dentry; i.e. remove a reference to the corresponding inode.
 *
 * If there are no remaining references to the inode either through dentries or
 * open file descriptors, the inode is freed.  Otherwise, the inode is not
 * removed, but the dentry is unlinked and freed.
 *
 * Either way, all lookup table entries referenced by the inode have their
 * reference count decremented.  If a lookup table entry has no open file
 * descriptors and no references remaining, it is freed, and the staging file is
 * unlinked.
 */
static void remove_dentry(struct dentry *dentry,
			  struct lookup_table *lookup_table)
{
	struct inode *inode = dentry->d_inode;
	struct lookup_table_entry *lte;
	unsigned i;

	for (i = 0; i <= inode->num_ads; i++) {
		lte = inode_stream_lte_resolved(inode, i);
		if (lte)
			lte_decrement_refcnt(lte, lookup_table);
	}
	unlink_dentry(dentry);
	put_dentry(dentry);
}

/* Transfers file attributes from a struct inode to a `stat' buffer.
 *
 * The lookup table entry tells us which stream in the inode we are statting.
 * For a named data stream, everything returned is the same as the unnamed data
 * stream except possibly the size and block count. */
static int inode_to_stbuf(const struct inode *inode,
			  struct lookup_table_entry *lte, struct stat *stbuf)
{
	if (inode_is_symlink(inode))
		stbuf->st_mode = S_IFLNK | 0777;
	else if (inode_is_directory(inode))
		stbuf->st_mode = S_IFDIR | 0755;
	else
		stbuf->st_mode = S_IFREG | 0755;

	stbuf->st_ino   = (ino_t)inode->ino;
	stbuf->st_nlink = inode->link_count;
	stbuf->st_uid   = getuid();
	stbuf->st_gid   = getgid();

	if (lte) {
		if (lte->resource_location == RESOURCE_IN_STAGING_FILE) {
			struct stat native_stat;
			if (stat(lte->staging_file_name, &native_stat) != 0) {
				DEBUG("Failed to stat `%s': %m",
				      lte->staging_file_name);
				return -errno;
			}
			stbuf->st_size = native_stat.st_size;
		} else {
			stbuf->st_size = wim_resource_size(lte);
		}
	} else {
		stbuf->st_size = 0;
	}

	stbuf->st_atime   = wim_timestamp_to_unix(inode->last_access_time);
	stbuf->st_mtime   = wim_timestamp_to_unix(inode->last_write_time);
	stbuf->st_ctime   = wim_timestamp_to_unix(inode->creation_time);
	stbuf->st_blocks  = (stbuf->st_size + 511) / 512;
	return 0;
}

/* Creates a new staging file and returns its file descriptor opened for
 * writing.
 *
 * @name_ret: A location into which the a pointer to the newly allocated name of
 * 			the staging file is stored.
 * @return:  The file descriptor for the new file.  Returns -1 and sets errno on
 * 		error, for any reason possible from the creat() function.
 */
static int create_staging_file(char **name_ret, int open_flags,
			       struct wimfs_context *ctx)
{
	size_t name_len;
	char *name;
	struct stat stbuf;
	int fd;
	int errno_save;

	name_len = ctx->staging_dir_name_len + 1 + SHA1_HASH_SIZE;
 	name = MALLOC(name_len + 1);
	if (!name) {
		errno = ENOMEM;
		return -1;
	}

	do {

		memcpy(name, ctx->staging_dir_name, ctx->staging_dir_name_len);
		name[ctx->staging_dir_name_len] = '/';
		randomize_char_array_with_alnum(name + ctx->staging_dir_name_len + 1,
						SHA1_HASH_SIZE);
		name[name_len] = '\0';


	/* Just in case, verify that the randomly generated name doesn't name an
	 * existing file, and try again if so  */
	} while (stat(name, &stbuf) == 0);

	if (errno != ENOENT)
		/* other error! */
		return -1;

	/* doesn't exist--- ok */

	DEBUG("Creating staging file `%s'", name);

	fd = open(name, open_flags | O_CREAT | O_TRUNC, 0600);
	if (fd == -1) {
		errno_save = errno;
		FREE(name);
		errno = errno_save;
	} else {
		*name_ret = name;
	}
	return fd;
}

/*
 * Extract a WIM resource to the staging directory.
 *
 * @inode:  Inode that contains the stream we are extracting
 *
 * @stream_id: Identifier for the stream (it stays constant even if the indices
 * of the stream entries are changed)
 *
 * @lte: Pointer to pointer to the lookup table entry for the stream we need to
 * extract, or NULL if there was no lookup table entry present for the stream
 *
 * @size:  Number of bytes of the stream we want to extract (this supports the
 * wimfs_truncate() function).  It may be more than the actual stream length, in
 * which case the extra space is filled with zeroes.
 *
 * @ctx:  Context for the WIM filesystem.
 *
 * Returns 0 on success or a negative error code on failure.
 */
static int extract_resource_to_staging_dir(struct inode *inode,
					   u32 stream_id,
					   struct lookup_table_entry **lte,
					   off_t size,
					   struct wimfs_context *ctx)
{
	char *staging_file_name;
	int ret;
	int fd;
	struct lookup_table_entry *old_lte, *new_lte;
	off_t extract_size;

	DEBUG("Extracting resource to staging dir: inode %"PRIu64", "
	      "stream id %"PRIu32, inode->ino, stream_id);

	old_lte = *lte;

	wimlib_assert(old_lte == NULL ||
		      old_lte->resource_location != RESOURCE_IN_STAGING_FILE);

	/* Create the staging file */
	fd = create_staging_file(&staging_file_name, O_WRONLY, ctx);
	if (fd == -1)
		return -errno;

	/* Extract the stream to the staging file (possibly truncated) */
	if (old_lte) {
		extract_size = min(wim_resource_size(old_lte), size);
		ret = extract_wim_resource_to_fd(old_lte, fd, extract_size);
	} else {
		ret = 0;
		extract_size = 0;
	}

	/* In the case of truncate() to more than the file length, extend the
	 * file with zeroes by calling ftruncate() on the underlying staging
	 * file */
	if (ret == 0 && size > extract_size)
		ret = ftruncate(fd, size);

	/* Close the staging file descriptor and check for errors.  If there's
	 * an error, unlink the staging file. */
	if (ret != 0 || close(fd) != 0) {
		if (errno != 0)
			ret = -errno;
		else
			ret = -EIO;
		close(fd);
		goto out_delete_staging_file;
	}

	/* Now deal with the lookup table entries.  We may be able to re-use the
	 * existing entry, but we may have to create a new one instead. */

	if (old_lte && inode->link_count == old_lte->refcnt) {
		/* The reference count of the existing lookup table entry is the
		 * same as the link count of the inode that contains the stream
		 * we're opening.  Therefore, ALL the references to the lookup
		 * table entry correspond to the stream we're trying to extract,
		 * so the lookup table entry can be re-used.  */
		DEBUG("Re-using lookup table entry");
		lookup_table_unlink(ctx->wim->lookup_table, old_lte);
		new_lte = old_lte;
	} else {
		if (old_lte) {
			/* There's an existing lookup table entry, but its
			 * reference count is greater than the link count for
			 * the inode containing a stream we're opening.
			 * Therefore, we need to split the lookup table entry.
			 */
			wimlib_assert(old_lte->refcnt > inode->link_count);
			DEBUG("Splitting lookup table entry "
			      "(inode->link_count = %u, old_lte->refcnt = %u)",
			      inode->link_count, old_lte->refcnt);
		}

		new_lte = new_lookup_table_entry();
		if (!new_lte) {
			ret = -ENOMEM;
			goto out_delete_staging_file;
		}

		/* There may already be open file descriptors to this stream if
		 * it's previously been opened read-only, but just now we're
		 * opening it read-write.  Identify those file descriptors and
		 * change their lookup table entry pointers to point to the new
		 * lookup table entry, and open staging file descriptors for
		 * them.
		 *
		 * At the same time, we need to count the number of these opened
		 * file descriptors to the new lookup table entry.  If there's
		 * an old lookup table entry, this number needs to be subtracted
		 * from the fd's opened to the old entry. */
		for (u16 i = 0, j = 0; j < inode->num_opened_fds; i++) {
			struct wimlib_fd *fd = inode->fds[i];
			if (fd) {
				if (fd->stream_id == stream_id) {
					wimlib_assert(fd->f_lte == old_lte);
					wimlib_assert(fd->staging_fd == -1);
					fd->f_lte = new_lte;
					new_lte->num_opened_fds++;
					fd->staging_fd = open(staging_file_name, O_RDONLY);
					if (fd->staging_fd == -1) {
						ret = -errno;
						goto out_revert_fd_changes;
					}
				}
				j++;
			}
		}
		DEBUG("%hu fd's were already opened to the file we extracted",
		      new_lte->num_opened_fds);
		if (old_lte) {
			old_lte->num_opened_fds -= new_lte->num_opened_fds;
			old_lte->refcnt -= inode->link_count;
		}
	}

	new_lte->refcnt                       = inode->link_count;
	new_lte->resource_location            = RESOURCE_IN_STAGING_FILE;
	new_lte->staging_file_name            = staging_file_name;
	new_lte->lte_inode                    = inode;
	random_hash(new_lte->hash);

	if (stream_id == 0)
		inode->lte = new_lte;
	else
		for (u16 i = 0; i < inode->num_ads; i++)
			if (inode->ads_entries[i].stream_id == stream_id)
				inode->ads_entries[i].lte = new_lte;

	lookup_table_insert(ctx->wim->lookup_table, new_lte);
	list_add(&new_lte->staging_list, &ctx->staging_list);
	*lte = new_lte;
	return 0;
out_revert_fd_changes:
	for (u16 i = 0, j = 0; j < new_lte->num_opened_fds; i++) {
		struct wimlib_fd *fd = inode->fds[i];
		if (fd && fd->stream_id == stream_id && fd->f_lte == new_lte) {
			fd->f_lte = old_lte;
			if (fd->staging_fd != -1) {
				close(fd->staging_fd);
				fd->staging_fd = -1;
			}
			j++;
		}
	}
	free_lookup_table_entry(new_lte);
out_delete_staging_file:
	unlink(staging_file_name);
	FREE(staging_file_name);
	return ret;
}

/*
 * Creates a randomly named staging directory and saves its name in the
 * filesystem context structure.
 */
static int make_staging_dir(struct wimfs_context *ctx, const char *user_prefix)
{
	static const size_t random_suffix_len = 10;
	static const char *common_suffix = ".staging";
	static const size_t common_suffix_len = 8;

	char *staging_dir_name = NULL;
	size_t staging_dir_name_len;
	size_t prefix_len;
	const char *wim_basename;
	char *real_user_prefix = NULL;
	int ret;

	if (user_prefix) {
		real_user_prefix = realpath(user_prefix, NULL);
		if (!real_user_prefix) {
			ERROR_WITH_ERRNO("Could not resolve `%s'",
					 real_user_prefix);
			ret = WIMLIB_ERR_NOTDIR;
			goto out;
		}
		wim_basename = path_basename(ctx->wim->filename);
		prefix_len = strlen(real_user_prefix) + 1 + strlen(wim_basename);
	} else {
		prefix_len = strlen(ctx->wim->filename);
	}

	staging_dir_name_len = prefix_len + common_suffix_len + random_suffix_len;

	staging_dir_name = MALLOC(staging_dir_name_len + 1);
	if (!staging_dir_name) {
		ret = WIMLIB_ERR_NOMEM;
		goto out;
	}

	if (real_user_prefix)
		sprintf(staging_dir_name, "%s/%s", real_user_prefix, wim_basename);
	else
		strcpy(staging_dir_name, ctx->wim->filename);

	strcat(staging_dir_name, common_suffix);

	randomize_char_array_with_alnum(staging_dir_name + prefix_len + common_suffix_len,
					random_suffix_len);

	staging_dir_name[staging_dir_name_len] = '\0';

	if (mkdir(staging_dir_name, 0700) != 0) {
		ERROR_WITH_ERRNO("Failed to create temporary directory `%s'",
				 staging_dir_name);
		ret = WIMLIB_ERR_MKDIR;
	} else {
		ret = 0;
	}
out:
	FREE(real_user_prefix);
	if (ret == 0) {
		ctx->staging_dir_name = staging_dir_name;
		ctx->staging_dir_name_len = staging_dir_name_len;
	} else {
		FREE(staging_dir_name);
	}
	return ret;
}

static int remove_file_or_directory(const char *fpath, const struct stat *sb,
				    int typeflag, struct FTW *ftwbuf)
{
	if (remove(fpath) == 0)
		return 0;
	else {
		ERROR_WITH_ERRNO("Cannot remove `%s'", fpath);
		return WIMLIB_ERR_DELETE_STAGING_DIR;
	}
}

/*
 * Deletes the staging directory and all the files contained in it.
 */
static int delete_staging_dir(struct wimfs_context *ctx)
{
	int ret;
	ret = nftw(ctx->staging_dir_name, remove_file_or_directory,
		   10, FTW_DEPTH);
	FREE(ctx->staging_dir_name);
	ctx->staging_dir_name = NULL;
	return ret;
}

static void inode_update_lte_ptr(struct inode *inode,
				 struct lookup_table_entry *old_lte,
				 struct lookup_table_entry *new_lte)
{
	if (inode->lte == old_lte) {
		inode->lte = new_lte;
	} else {
		for (unsigned i = 0; i < inode->num_ads; i++) {
			if (inode->ads_entries[i].lte == old_lte) {
				inode->ads_entries[i].lte = new_lte;
				break;
			}
		}
	}
}

static int update_lte_of_staging_file(struct lookup_table_entry *lte,
				      struct lookup_table *table)
{
	struct lookup_table_entry *duplicate_lte;
	int ret;
	u8 hash[SHA1_HASH_SIZE];
	struct stat stbuf;

	ret = sha1sum(lte->staging_file_name, hash);
	if (ret != 0)
		return ret;
	lookup_table_unlink(table, lte);
	duplicate_lte = __lookup_resource(table, hash);
	if (duplicate_lte) {
		/* Merge duplicate lookup table entries */
		duplicate_lte->refcnt += lte->refcnt;
		inode_update_lte_ptr(lte->lte_inode, lte, duplicate_lte);
		free_lookup_table_entry(lte);
	} else {
		if (stat(lte->staging_file_name, &stbuf) != 0) {
			ERROR_WITH_ERRNO("Failed to stat `%s'", lte->staging_file_name);
			return WIMLIB_ERR_STAT;
		}
		if (stbuf.st_size == 0) {
			/* Zero-length stream.  No lookup table entry needed. */
			inode_update_lte_ptr(lte->lte_inode, lte, NULL);
			free_lookup_table_entry(lte);
		} else {
			wimlib_assert(&lte->file_on_disk == &lte->staging_file_name);
			lte->resource_entry.original_size = stbuf.st_size;
			lte->resource_entry.size = stbuf.st_size;
			lte->resource_location = RESOURCE_IN_FILE_ON_DISK;
			lte->file_on_disk_fp = NULL;
			copy_hash(lte->hash, hash);
			lookup_table_insert(table, lte);
		}
	}
	return 0;
}

static int inode_close_fds(struct inode *inode)
{
	u16 num_opened_fds = inode->num_opened_fds;
	for (u16 i = 0, j = 0; j < num_opened_fds; i++) {
		struct wimlib_fd *fd = inode->fds[i];
		if (fd) {
			wimlib_assert(fd->f_inode == inode);
			int ret = close_wimlib_fd(fd);
			if (ret != 0)
				return ret;
			j++;
		}
	}
	return 0;
}

/* Overwrites the WIM file, with changes saved. */
static int rebuild_wim(struct wimfs_context *ctx, int write_flags)
{
	int ret;
	struct lookup_table_entry *lte, *tmp;
	WIMStruct *w = ctx->wim;

	DEBUG("Closing all staging file descriptors.");
	list_for_each_entry_safe(lte, tmp, &ctx->staging_list, staging_list) {
		ret = inode_close_fds(lte->lte_inode);
		if (ret != 0)
			return ret;
	}

	DEBUG("Calculating SHA1 checksums for all new staging files.");
	list_for_each_entry(lte, &ctx->staging_list, staging_list) {
		ret = update_lte_of_staging_file(lte, w->lookup_table);
		if (ret != 0)
			return ret;
	}

	xml_update_image_info(w, w->current_image);
	ret = wimlib_overwrite(w, write_flags, 0, NULL);
	if (ret != 0)
		ERROR("Failed to commit changes to mounted WIM image");
	return ret;
}



/* Simple function that returns the concatenation of 2 strings. */
static char *strcat_dup(const char *s1, const char *s2, size_t max_len)
{
	size_t len = strlen(s1) + strlen(s2);
	if (len > max_len)
		len = max_len;
	char *p = MALLOC(len + 1);
	if (!p)
		return NULL;
	snprintf(p, len + 1, "%s%s", s1, s2);
	return p;
}

static int set_message_queue_names(struct wimfs_context *ctx,
				   const char *mount_dir)
{
	static const char *u2d_prefix = "/wimlib-unmount-to-daemon-mq";
	static const char *d2u_prefix = "/wimlib-daemon-to-unmount-mq";
	char *dir_path;
	char *p;
	int ret;

 	dir_path = realpath(mount_dir, NULL);
	if (!dir_path) {
		ERROR_WITH_ERRNO("Failed to resolve path \"%s\"", mount_dir);
		if (errno == ENOMEM)
			return WIMLIB_ERR_NOMEM;
		else
			return WIMLIB_ERR_NOTDIR;
	}

	p = dir_path;
	while (*p) {
		if (*p == '/')
			*p = 0xff;
		p++;
	}

	ctx->unmount_to_daemon_mq_name = strcat_dup(u2d_prefix, dir_path,
						    NAME_MAX);
	if (!ctx->unmount_to_daemon_mq_name) {
		ret = WIMLIB_ERR_NOMEM;
		goto out_free_dir_path;
	}
	ctx->daemon_to_unmount_mq_name = strcat_dup(d2u_prefix, dir_path,
						    NAME_MAX);
	if (!ctx->daemon_to_unmount_mq_name) {
		ret = WIMLIB_ERR_NOMEM;
		goto out_free_unmount_to_daemon_mq_name;
	}

	ret = 0;
	goto out_free_dir_path;
out_free_unmount_to_daemon_mq_name:
	FREE(ctx->unmount_to_daemon_mq_name);
	ctx->unmount_to_daemon_mq_name = NULL;
out_free_dir_path:
	FREE(dir_path);
	return ret;
}

static void free_message_queue_names(struct wimfs_context *ctx)
{
	FREE(ctx->unmount_to_daemon_mq_name);
	FREE(ctx->daemon_to_unmount_mq_name);
	ctx->unmount_to_daemon_mq_name = NULL;
	ctx->daemon_to_unmount_mq_name = NULL;
}

/*
 * Opens two POSIX message queue: one for sending messages from the unmount
 * process to the daemon process, and one to go the other way.  The names of the
 * message queues, which must be system-wide unique, are be based on the mount
 * point.
 *
 * @daemon specifies whether the calling process is the filesystem daemon or the
 * unmount process.
 */
static int open_message_queues(struct wimfs_context *ctx, bool daemon)
{
	int unmount_to_daemon_mq_flags = O_WRONLY | O_CREAT;
	int daemon_to_unmount_mq_flags = O_RDONLY | O_CREAT;

	if (daemon)
		swap(unmount_to_daemon_mq_flags, daemon_to_unmount_mq_flags);

	DEBUG("Opening message queue \"%s\"", ctx->unmount_to_daemon_mq_name);
	ctx->unmount_to_daemon_mq = mq_open(ctx->unmount_to_daemon_mq_name,
					    unmount_to_daemon_mq_flags, 0700, NULL);

	if (ctx->unmount_to_daemon_mq == (mqd_t)-1) {
		ERROR_WITH_ERRNO("mq_open()");
		return WIMLIB_ERR_MQUEUE;
	}

	DEBUG("Opening message queue \"%s\"", ctx->daemon_to_unmount_mq_name);
	ctx->daemon_to_unmount_mq = mq_open(ctx->daemon_to_unmount_mq_name,
					    daemon_to_unmount_mq_flags, 0700, NULL);

	if (ctx->daemon_to_unmount_mq == (mqd_t)-1) {
		ERROR_WITH_ERRNO("mq_open()");
		mq_close(ctx->unmount_to_daemon_mq);
		mq_unlink(ctx->unmount_to_daemon_mq_name);
		ctx->unmount_to_daemon_mq = (mqd_t)-1;
		return WIMLIB_ERR_MQUEUE;
	}
	return 0;
}

/* Try to determine the maximum message size of a message queue.  The return
 * value is the maximum message size, or a guess of 8192 bytes if it cannot be
 * determined. */
static long mq_get_msgsize(mqd_t mq)
{
	static const char *msgsize_max_file = "/proc/sys/fs/mqueue/msgsize_max";
	FILE *fp;
	struct mq_attr attr;
	long msgsize;

	if (mq_getattr(mq, &attr) == 0) {
		msgsize = attr.mq_msgsize;
	} else {
		ERROR_WITH_ERRNO("mq_getattr()");
		ERROR("Attempting to read %s", msgsize_max_file);
		fp = fopen(msgsize_max_file, "rb");
		if (fp) {
			if (fscanf(fp, "%ld", &msgsize) != 1) {
				ERROR("Assuming message size of 8192");
				msgsize = 8192;
			}
			fclose(fp);
		} else {
			ERROR_WITH_ERRNO("Failed to open the file `%s'",
					 msgsize_max_file);
			ERROR("Assuming message size of 8192");
			msgsize = 8192;
		}
	}
	return msgsize;
}

static int get_mailbox(mqd_t mq, long needed_msgsize, long *msgsize_ret,
		       void **mailbox_ret)
{
	long msgsize;
	char *mailbox;

	msgsize = mq_get_msgsize(mq);

	if (msgsize < needed_msgsize) {
		ERROR("Message queue max size must be at least %ld!",
		      needed_msgsize);
		return WIMLIB_ERR_MQUEUE;
	}

	mailbox = MALLOC(msgsize);
	if (!mailbox) {
		ERROR("Failed to allocate %ld bytes for mailbox", msgsize);
		return WIMLIB_ERR_NOMEM;
	}
	*msgsize_ret = msgsize;
	*mailbox_ret = mailbox;
	return 0;
}

static void unlink_message_queues(struct wimfs_context *ctx)
{
	mq_unlink(ctx->unmount_to_daemon_mq_name);
	mq_unlink(ctx->daemon_to_unmount_mq_name);
}

/* Closes the message queues, which are allocated in static variables */
static void close_message_queues(struct wimfs_context *ctx)
{
	DEBUG("Closing message queues");
	mq_close(ctx->unmount_to_daemon_mq);
	ctx->unmount_to_daemon_mq = (mqd_t)(-1);
	mq_close(ctx->daemon_to_unmount_mq);
	ctx->daemon_to_unmount_mq = (mqd_t)(-1);
	unlink_message_queues(ctx);
}


struct unmount_msg_hdr {
	u32 min_version;
	u32 cur_version;
	u32 msg_type;
	u32 msg_size;
} PACKED;

struct msg_unmount_request {
	struct unmount_msg_hdr hdr;
	u32 unmount_flags;
} PACKED;

struct msg_daemon_info {
	struct unmount_msg_hdr hdr;
	pid_t daemon_pid;
	u32 mount_flags;
} PACKED;

struct msg_unmount_finished {
	struct unmount_msg_hdr hdr;
	int32_t status;
} PACKED;

enum {
	MSG_TYPE_UNMOUNT_REQUEST,
	MSG_TYPE_DAEMON_INFO,
	MSG_TYPE_UNMOUNT_FINISHED,
	MSG_TYPE_MAX,
};

struct msg_handler_context {
	bool is_daemon;
	int timeout_seconds;
	union {
		struct {
			/*bool unmount_complete;*/
			pid_t daemon_pid;
			int mount_flags;
			int status;
		} unmount;
		struct {
			/*int unmount_flags;*/
			struct wimfs_context *wimfs_ctx;
		} daemon;
	};
};

static int send_unmount_request_msg(mqd_t mq, int unmount_flags)
{
	DEBUG("Sending unmount request msg");
	struct msg_unmount_request msg = {
		.hdr = {
			.min_version = WIMLIB_MAKEVERSION(1, 2, 0),
			.cur_version = WIMLIB_VERSION_CODE,
			.msg_type    = MSG_TYPE_UNMOUNT_REQUEST,
			.msg_size    = sizeof(msg),
		},
		.unmount_flags = unmount_flags,
	};

	if (mq_send(mq, (void*)&msg, sizeof(msg), 1)) {
		ERROR_WITH_ERRNO("Failed to communicate with filesystem daemon");
		return WIMLIB_ERR_MQUEUE;
	}
	return 0;
}

static int send_daemon_info_msg(mqd_t mq, pid_t pid, int mount_flags)
{
	DEBUG("Sending daemon info msg (pid = %d, mount_flags=%x)",
	      pid, mount_flags);

	struct msg_daemon_info msg = {
		.hdr = {
			.min_version = WIMLIB_MAKEVERSION(1, 2, 0),
			.cur_version = WIMLIB_VERSION_CODE,
			.msg_type = MSG_TYPE_DAEMON_INFO,
			.msg_size = sizeof(msg),
		},
		.daemon_pid = pid,
		.mount_flags = mount_flags,
	};
	if (mq_send(mq, (void*)&msg, sizeof(msg), 1)) {
		ERROR_WITH_ERRNO("Failed to send daemon info to unmount process");
		return WIMLIB_ERR_MQUEUE;
	}
	return 0;
}

static void send_unmount_finished_msg(mqd_t mq, int status)
{
	DEBUG("Sending unmount finished msg");
	struct msg_unmount_finished msg = {
		.hdr = {
			.min_version = WIMLIB_MAKEVERSION(1, 2, 0),
			.cur_version = WIMLIB_VERSION_CODE,
			.msg_type = MSG_TYPE_UNMOUNT_FINISHED,
			.msg_size = sizeof(msg),
		},
		.status = status,
	};
	if (mq_send(mq, (void*)&msg, sizeof(msg), 1))
		ERROR_WITH_ERRNO("Failed to send status to unmount process");
}

static int msg_unmount_request_handler(const void *_msg,
				       struct msg_handler_context *handler_ctx)
{
	const struct msg_unmount_request *msg;
	struct wimfs_context *wimfs_ctx;
	int status = 0;
	int ret;
	int unmount_flags;

	DEBUG("Handling unmount request msg");

	msg = _msg;
	wimfs_ctx = handler_ctx->daemon.wimfs_ctx;

	if (msg->hdr.msg_size < sizeof(*msg)) {
		status = WIMLIB_ERR_INVALID_UNMOUNT_MESSAGE;
		goto out;
	}

	unmount_flags = msg->unmount_flags;

	ret = send_daemon_info_msg(wimfs_ctx->daemon_to_unmount_mq, getpid(),
				   wimfs_ctx->mount_flags);
	if (ret != 0) {
		status = ret;
		goto out;
	}

	if (wimfs_ctx->mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) {
		if (unmount_flags & WIMLIB_UNMOUNT_FLAG_COMMIT) {
			int write_flags = 0;
			if (unmount_flags & WIMLIB_UNMOUNT_FLAG_CHECK_INTEGRITY)
				write_flags |= WIMLIB_WRITE_FLAG_CHECK_INTEGRITY;
			if (unmount_flags & WIMLIB_UNMOUNT_FLAG_REBUILD)
				write_flags |= WIMLIB_WRITE_FLAG_REBUILD;
			if (unmount_flags & WIMLIB_UNMOUNT_FLAG_RECOMPRESS)
				write_flags |= WIMLIB_WRITE_FLAG_RECOMPRESS;
			status = rebuild_wim(wimfs_ctx, write_flags);
		}
	} else {
		DEBUG("Read-only mount");
		status = 0;
	}

out:
	if (wimfs_ctx->mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) {
		ret = delete_staging_dir(wimfs_ctx);
		if (ret != 0) {
			ERROR("Failed to delete the staging directory");
			if (status == 0)
				status = ret;
		}
	}
	send_unmount_finished_msg(wimfs_ctx->daemon_to_unmount_mq, status);
	return MSG_BREAK_LOOP;
}

static int msg_daemon_info_handler(const void *_msg,
				   struct msg_handler_context *handler_ctx)
{
	const struct msg_daemon_info *msg = _msg;
	DEBUG("Handling daemon info msg");
	if (msg->hdr.msg_size < sizeof(*msg))
		return WIMLIB_ERR_INVALID_UNMOUNT_MESSAGE;
	handler_ctx->unmount.daemon_pid = msg->daemon_pid;
	handler_ctx->unmount.mount_flags = msg->mount_flags;
	handler_ctx->timeout_seconds = 1;
	DEBUG("pid of daemon is %d; mount flags were %#x",
	      handler_ctx->unmount.daemon_pid,
	      handler_ctx->unmount.mount_flags);
	return 0;
}

static int msg_unmount_finished_handler(const void *_msg,
					struct msg_handler_context *handler_ctx)
{
	const struct msg_unmount_finished *msg = _msg;
	DEBUG("Handling unmount finished message");
	if (msg->hdr.msg_size < sizeof(*msg))
		return WIMLIB_ERR_INVALID_UNMOUNT_MESSAGE;
	handler_ctx->unmount.status = msg->status;
	DEBUG("status is %d", handler_ctx->unmount.status);
	return MSG_BREAK_LOOP;
}

static int unmount_timed_out_cb(struct msg_handler_context *handler_ctx)
{
	if (handler_ctx->unmount.daemon_pid == 0) {
		goto out_crashed;
	} else {
		errno = 0;
		kill(handler_ctx->unmount.daemon_pid, 0);
		if (errno == ESRCH) {
			goto out_crashed;
		} else if (errno != 0) {
			ERROR_WITH_ERRNO("Error determining state of "
					 "filesystem daemon");
			return WIMLIB_ERR_MQUEUE;
		} else {
			DEBUG("Filesystem daemon is still alive... "
			      "Waiting another %d seconds",
			      handler_ctx->timeout_seconds);
			return 0;
		}
	}
out_crashed:
	ERROR("The filesystem daemon has crashed!  Changes to the "
	      "WIM may not have been commited.");
	return WIMLIB_ERR_FILESYSTEM_DAEMON_CRASHED;
}

static int daemon_timed_out_cb(struct msg_handler_context *handler_ctx)
{
	DEBUG("Timed out");
	return WIMLIB_ERR_TIMEOUT;
}

typedef int (*msg_handler_t)(const void *_msg,
			     struct msg_handler_context *handler_ctx);

struct msg_handler_callbacks {
	int (*timed_out)(struct msg_handler_context *);
	msg_handler_t msg_handlers[MSG_TYPE_MAX];
};

static const struct msg_handler_callbacks unmount_msg_handler_callbacks = {
	.timed_out = unmount_timed_out_cb,
	.msg_handlers = {
		[MSG_TYPE_DAEMON_INFO] = msg_daemon_info_handler,
		[MSG_TYPE_UNMOUNT_FINISHED] = msg_unmount_finished_handler,
	},
};

static const struct msg_handler_callbacks daemon_msg_handler_callbacks = {
	.timed_out = daemon_timed_out_cb,
	.msg_handlers = {
		[MSG_TYPE_UNMOUNT_REQUEST] = msg_unmount_request_handler,
	},
};

static int receive_message(mqd_t mq, struct msg_handler_context *handler_ctx,
			   const msg_handler_t msg_handlers[],
			   long mailbox_size, void *mailbox)
{
	struct timeval now;
	struct timespec timeout;
	ssize_t bytes_received;
	struct unmount_msg_hdr *hdr;
	int ret;

	gettimeofday(&now, NULL);
	timeout.tv_sec = now.tv_sec + handler_ctx->timeout_seconds;
	timeout.tv_nsec = now.tv_usec * 1000;

	bytes_received = mq_timedreceive(mq, mailbox,
					 mailbox_size, NULL, &timeout);
	hdr = mailbox;
	if (bytes_received == -1) {
		ERROR_WITH_ERRNO("mq_timedreceive()");
		if (errno == ETIMEDOUT)
			ret = WIMLIB_ERR_TIMEOUT;
		else
			ret = WIMLIB_ERR_MQUEUE;
	} else if (bytes_received < sizeof(*hdr) ||
		   bytes_received != hdr->msg_size) {
		ret = WIMLIB_ERR_INVALID_UNMOUNT_MESSAGE;
	} else if (WIMLIB_VERSION_CODE < hdr->min_version) {
		/*ERROR("Cannot understand the received message. "*/
		      /*"Please upgrade wimlib to at least v%d.%d.%d",*/
		      /*WIMLIB_GET_MAJOR_VERSION(hdr->min_version),*/
		      /*WIMLIB_GET_MINOR_VERSION(hdr->min_version),*/
		      /*WIMLIB_GET_PATCH_VERSION(hdr->min_version));*/
		ret = MSG_VERSION_TOO_HIGH;
	} else if (hdr->msg_type >= MSG_TYPE_MAX) {
		ret = WIMLIB_ERR_INVALID_UNMOUNT_MESSAGE;
	} else if (msg_handlers[hdr->msg_type] == NULL) {
		ret = WIMLIB_ERR_INVALID_UNMOUNT_MESSAGE;
	} else {
		ret = msg_handlers[hdr->msg_type](mailbox, handler_ctx);
	}
	return ret;
}

static int message_loop(mqd_t mq,
			const struct msg_handler_callbacks *callbacks,
			struct msg_handler_context *handler_ctx)
{
	static const size_t MAX_MSG_SIZE = 512;
	long msgsize;
	void *mailbox;
	int ret;

	DEBUG("Entering message loop");

	ret = get_mailbox(mq, MAX_MSG_SIZE, &msgsize, &mailbox);
	if (ret != 0)
		return ret;
	while (1) {
		ret = receive_message(mq, handler_ctx,
				      callbacks->msg_handlers,
				      msgsize, mailbox);
		if (ret == 0 || ret == MSG_VERSION_TOO_HIGH) {
			continue;
		} else if (ret == MSG_BREAK_LOOP) {
			ret = 0;
			break;
		} else if (ret == WIMLIB_ERR_TIMEOUT) {
			if (callbacks->timed_out)
				ret = callbacks->timed_out(handler_ctx);
			if (ret == 0)
				continue;
			else
				break;
		} else {
			ERROR_WITH_ERRNO("Error communicating with "
					 "filesystem daemon");
			break;
		}
	}
	FREE(mailbox);
	DEBUG("Exiting message loop");
	return ret;
}

/* Execute `fusermount -u', which is installed setuid root, to unmount the WIM.
 *
 * FUSE does not yet implement synchronous unmounts.  This means that fusermount
 * -u will return before the filesystem daemon returns from wimfs_destroy().
 *  This is partly what we want, because we need to send a message from this
 *  process to the filesystem daemon telling whether --commit was specified or
 *  not.  However, after that, the unmount process must wait for the filesystem
 *  daemon to finish writing the WIM file.
 */
static int execute_fusermount(const char *dir)
{
	pid_t pid;
	int ret;
	int status;

	pid = fork();
	if (pid == -1) {
		ERROR_WITH_ERRNO("Failed to fork()");
		return WIMLIB_ERR_FORK;
	}
	if (pid == 0) {
		/* Child */
		execlp("fusermount", "fusermount", "-u", dir, NULL);
		ERROR_WITH_ERRNO("Failed to execute `fusermount'");
		exit(WIMLIB_ERR_FUSERMOUNT);
	}

	/* Parent */
	ret = wait(&status);
	if (ret == -1) {
		ERROR_WITH_ERRNO("Failed to wait for fusermount process to "
				 "terminate");
		return WIMLIB_ERR_FUSERMOUNT;
	}

	if (status != 0) {
		if (status == WIMLIB_ERR_FUSERMOUNT)
			ERROR("Could not find the `fusermount' program");
		else
			ERROR("fusermount exited with status %d", status);

		/* Try again, but with the `umount' program.  This is required
		 * on other FUSE implementations such as FreeBSD's that do not
		 * have a `fusermount' program. */

		pid = fork();
		if (pid == -1) {
			ERROR_WITH_ERRNO("Failed to fork()");
			return WIMLIB_ERR_FORK;
		}
		if (pid == 0) {
			/* Child */
			execlp("umount", "umount", dir, NULL);
			ERROR_WITH_ERRNO("Failed to execute `umount'");
			exit(WIMLIB_ERR_FUSERMOUNT);
		}

		/* Parent */
		ret = wait(&status);
		if (ret == -1) {
			ERROR_WITH_ERRNO("Failed to wait for `umount' process to "
					 "terminate");
			return WIMLIB_ERR_FUSERMOUNT;
		}
		if (status != 0) {
			ERROR("`umount' exited with failure status");
			return WIMLIB_ERR_FUSERMOUNT;
		}
	}
	return 0;
}

static int wimfs_access(const char *path, int mask)
{
	/* Permissions not implemented */
	return 0;
}

static int wimfs_chmod(const char *path, mode_t mask)
{
	struct dentry *dentry;
	struct wimfs_context *ctx = wimfs_get_context();
	struct inode *inode;
	struct stat stbuf;
	int ret;

	ret = lookup_resource(ctx->wim, path,
			      get_lookup_flags(ctx) | LOOKUP_FLAG_DIRECTORY_OK,
			      &dentry, NULL, NULL);
	if (ret != 0)
		return ret;
	inode = dentry->d_inode;
	inode_to_stbuf(inode, NULL, &stbuf);
	if (mask == stbuf.st_mode)
		return 0;
	else
		return -EPERM;
}

/* Called when the filesystem is unmounted. */
static void wimfs_destroy(void *p)
{
	struct wimfs_context *wimfs_ctx;

	wimfs_ctx = wimfs_get_context();

	if (open_message_queues(wimfs_ctx, true))
		return;

	struct msg_handler_context handler_ctx = {
		.is_daemon = true,
		.timeout_seconds = 5,
		.daemon = {
			.wimfs_ctx = wimfs_ctx,
		},
	};

	message_loop(wimfs_ctx->unmount_to_daemon_mq,
		     &daemon_msg_handler_callbacks,
		     &handler_ctx);

	close_message_queues(wimfs_ctx);
}

#if 0
static int wimfs_fallocate(const char *path, int mode,
			   off_t offset, off_t len, struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	wimlib_assert(fd->staging_fd != -1);
	return fallocate(fd->staging_fd, mode, offset, len);
}

#endif

static int wimfs_fgetattr(const char *path, struct stat *stbuf,
			  struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	return inode_to_stbuf(fd->f_inode, fd->f_lte, stbuf);
}

static int wimfs_ftruncate(const char *path, off_t size,
			   struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	int ret = ftruncate(fd->staging_fd, size);
	if (ret != 0)
		return -errno;
	if (fd->f_lte && size < fd->f_lte->resource_entry.original_size)
		fd->f_lte->resource_entry.original_size = size;
	return 0;
}

/*
 * Fills in a `struct stat' that corresponds to a file or directory in the WIM.
 */
static int wimfs_getattr(const char *path, struct stat *stbuf)
{
	struct dentry *dentry;
	struct lookup_table_entry *lte;
	int ret;
	struct wimfs_context *ctx = wimfs_get_context();

	ret = lookup_resource(ctx->wim, path,
			      get_lookup_flags(ctx) | LOOKUP_FLAG_DIRECTORY_OK,
			      &dentry, &lte, NULL);
	if (ret != 0)
		return ret;
	return inode_to_stbuf(dentry->d_inode, lte, stbuf);
}

#ifdef ENABLE_XATTR
/* Read an alternate data stream through the XATTR interface, or get its size */
static int wimfs_getxattr(const char *path, const char *name, char *value,
			  size_t size)
{
	int ret;
	struct inode *inode;
	struct ads_entry *ads_entry;
	size_t res_size;
	struct lookup_table_entry *lte;
	struct wimfs_context *ctx = wimfs_get_context();

	if (!(ctx->mount_flags & WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_XATTR))
		return -ENOTSUP;

	if (strlen(name) < 5 || memcmp(name, "user.", 5) != 0)
		return -ENOATTR;
	name += 5;

	inode = wim_pathname_to_inode(ctx->wim, path);
	if (!inode)
		return -ENOENT;

	ads_entry = inode_get_ads_entry(inode, name, NULL);
	if (!ads_entry)
		return -ENOATTR;

	lte = ads_entry->lte;
	res_size = wim_resource_size(lte);

	if (size == 0)
		return res_size;

	if (res_size > size)
		return -ERANGE;

	ret = read_full_wim_resource(lte, (u8*)value,
				     WIMLIB_RESOURCE_FLAG_MULTITHREADED);
	if (ret != 0)
		return -EIO;

	return res_size;
}
#endif

/* Create a hard link */
static int wimfs_link(const char *to, const char *from)
{
	struct dentry *from_dentry, *from_dentry_parent;
	const char *link_name;
	struct inode *inode;
	struct lookup_table_entry *lte;
	WIMStruct *w = wimfs_get_WIMStruct();
	u16 i;

	inode = wim_pathname_to_inode(w, to);
	if (!inode)
		return -ENOENT;

	if (inode->attributes & FILE_ATTRIBUTE_REPARSE_POINT)
		return -EEXIST;

	if (inode->attributes & FILE_ATTRIBUTE_DIRECTORY)
		return -EPERM;

	from_dentry_parent = get_parent_dentry(w, from);
	if (!from_dentry_parent)
		return -ENOENT;
	if (!dentry_is_directory(from_dentry_parent))
		return -ENOTDIR;

	link_name = path_basename(from);
	if (get_dentry_child_with_name(from_dentry_parent, link_name))
		return -EEXIST;
	from_dentry = new_dentry(link_name);
	if (!from_dentry)
		return -ENOMEM;

	inode_add_dentry(from_dentry, inode);
	from_dentry->d_inode = inode;
	inode->link_count++;

	for (i = 0; i <= inode->num_ads; i++) {
		lte = inode_stream_lte_resolved(inode, i);
		if (lte)
			lte->refcnt++;
	}
	dentry_add_child(from_dentry_parent, from_dentry);
	return 0;
}

#ifdef ENABLE_XATTR
static int wimfs_listxattr(const char *path, char *list, size_t size)
{
	size_t needed_size;
	struct inode *inode;
	struct wimfs_context *ctx = wimfs_get_context();
	u16 i;
	char *p;

	if (!(ctx->mount_flags & WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_XATTR))
		return -ENOTSUP;

	/* List alternate data streams, or get the list size */

	inode = wim_pathname_to_inode(ctx->wim, path);
	if (!inode)
		return -ENOENT;

	if (size == 0) {
		needed_size = 0;
		for (i = 0; i < inode->num_ads; i++)
			needed_size += inode->ads_entries[i].stream_name_utf8_len + 6;
		return needed_size;
	} else {
		p = list;
		for (i = 0; i < inode->num_ads; i++) {
			needed_size = inode->ads_entries[i].stream_name_utf8_len + 6;
			if (needed_size > size)
				return -ERANGE;
			p += sprintf(p, "user.%s",
				     inode->ads_entries[i].stream_name_utf8) + 1;
			size -= needed_size;
		}
		return p - list;
	}
}
#endif


/* Create a directory in the WIM.
 * @mode is currently ignored.  */
static int wimfs_mkdir(const char *path, mode_t mode)
{
	struct dentry *dentry;
	int ret;

	ret = create_dentry(wimfs_get_context(), path, &dentry);
	if (ret == 0)
		dentry->d_inode->attributes = FILE_ATTRIBUTE_DIRECTORY;
	return ret;
}

/* Create a regular file in the WIM.
 * @mode is currently ignored.  */
static int wimfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	const char *stream_name;
	struct wimfs_context *ctx = wimfs_get_context();
	if ((ctx->mount_flags & WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_WINDOWS)
	     && (stream_name = path_stream_name(path))) {
		/* Make an alternate data stream */
		struct ads_entry *new_entry;
		struct inode *inode;

		char *p = (char*)stream_name - 1;
		wimlib_assert(*p == ':');
		*p = '\0';

		inode = wim_pathname_to_inode(ctx->wim, path);
		if (!inode)
			return -ENOENT;
		if (inode->attributes &
		    (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
			return -ENOENT;
		if (inode_get_ads_entry(inode, stream_name, NULL))
			return -EEXIST;
		new_entry = inode_add_ads(inode, stream_name);
		if (!new_entry)
			return -ENOMEM;
		return 0;
	} else {
		struct dentry *dentry;
		int ret;

		/* Make a normal file (not an alternate data stream) */
		ret = create_dentry(ctx, path, &dentry);
		if (ret == 0)
			dentry->d_inode->attributes = FILE_ATTRIBUTE_NORMAL;
		return ret;
	}
}


/* Open a file.  */
static int wimfs_open(const char *path, struct fuse_file_info *fi)
{
	struct dentry *dentry;
	struct lookup_table_entry *lte;
	int ret;
	struct wimlib_fd *fd;
	struct inode *inode;
	u16 stream_idx;
	u32 stream_id;
	struct wimfs_context *ctx = wimfs_get_context();

	ret = lookup_resource(ctx->wim, path, get_lookup_flags(ctx),
			      &dentry, &lte, &stream_idx);
	if (ret != 0)
		return ret;

	inode = dentry->d_inode;

	if (stream_idx == 0)
		stream_id = 0;
	else
		stream_id = inode->ads_entries[stream_idx - 1].stream_id;

	/* The file resource may be in the staging directory (read-write mounts
	 * only) or in the WIM.  If it's in the staging directory, we need to
	 * open a native file descriptor for the corresponding file.  Otherwise,
	 * we can read the file resource directly from the WIM file if we are
	 * opening it read-only, but we need to extract the resource to the
	 * staging directory if we are opening it writable. */

	if (flags_writable(fi->flags) &&
            (!lte || lte->resource_location != RESOURCE_IN_STAGING_FILE)) {
		u64 size = (lte) ? wim_resource_size(lte) : 0;
		ret = extract_resource_to_staging_dir(inode, stream_id,
						      &lte, size, ctx);
		if (ret != 0)
			return ret;
	}

	ret = alloc_wimlib_fd(inode, stream_id, lte, &fd,
			      wimfs_ctx_readonly(ctx));
	if (ret != 0)
		return ret;

	if (lte && lte->resource_location == RESOURCE_IN_STAGING_FILE) {
		fd->staging_fd = open(lte->staging_file_name, fi->flags);
		if (fd->staging_fd == -1) {
			int errno_save = errno;
			close_wimlib_fd(fd);
			return -errno_save;
		}
	}
	fi->fh = (uintptr_t)fd;
	return 0;
}

/* Opens a directory. */
static int wimfs_opendir(const char *path, struct fuse_file_info *fi)
{
	struct inode *inode;
	int ret;
	struct wimlib_fd *fd = NULL;
	struct wimfs_context *ctx = wimfs_get_context();
	WIMStruct *w = ctx->wim;

	inode = wim_pathname_to_inode(w, path);
	if (!inode)
		return -ENOENT;
	if (!inode_is_directory(inode))
		return -ENOTDIR;
	ret = alloc_wimlib_fd(inode, 0, NULL, &fd, wimfs_ctx_readonly(ctx));
	fi->fh = (uintptr_t)fd;
	return ret;
}


/*
 * Read data from a file in the WIM or in the staging directory.
 */
static int wimfs_read(const char *path, char *buf, size_t size,
		      off_t offset, struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	ssize_t ret;

	if (!fd)
		return -EBADF;

	if (!fd->f_lte) /* Empty stream with no lookup table entry */
		return 0;

	if (fd->f_lte->resource_location == RESOURCE_IN_STAGING_FILE) {
		/* Read from staging file */

		wimlib_assert(fd->f_lte->staging_file_name);
		wimlib_assert(fd->staging_fd != -1);

		DEBUG("Seek to offset %zu", offset);

		if (lseek(fd->staging_fd, offset, SEEK_SET) == -1)
			return -errno;
		ret = read(fd->staging_fd, buf, size);
		if (ret == -1)
			return -errno;
		return ret;
	} else {
		/* Read from WIM */
		u64 res_size = wim_resource_size(fd->f_lte);
		if (offset > res_size)
			return -EOVERFLOW;
		size = min(size, res_size - offset);
		if (read_wim_resource(fd->f_lte, (u8*)buf,
				      size, offset,
				      WIMLIB_RESOURCE_FLAG_MULTITHREADED) != 0)
			return -EIO;
		return size;
	}
}

struct fill_params {
	void *buf;
	fuse_fill_dir_t filler;
};

static int dentry_fuse_fill(struct dentry *dentry, void *arg)
{
	struct fill_params *fill_params = arg;
	return fill_params->filler(fill_params->buf, dentry->file_name_utf8,
				   NULL, 0);
}

/* Fills in the entries of the directory specified by @path using the
 * FUSE-provided function @filler.  */
static int wimfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	struct inode *inode;

	if (!fd)
		return -EBADF;

	inode = fd->f_inode;

	struct fill_params fill_params = {
		.buf = buf,
		.filler = filler,
	};

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	return for_dentry_in_rbtree(inode->children.rb_node,
				    dentry_fuse_fill, &fill_params);
}


static int wimfs_readlink(const char *path, char *buf, size_t buf_len)
{
	struct wimfs_context *ctx = wimfs_get_context();
	struct inode *inode = wim_pathname_to_inode(ctx->wim, path);
	int ret;
	if (!inode)
		return -ENOENT;
	if (!inode_is_symlink(inode))
		return -EINVAL;

	ret = inode_readlink(inode, buf, buf_len, ctx->wim,
			     WIMLIB_RESOURCE_FLAG_MULTITHREADED);
	if (ret > 0)
		ret = 0;
	return ret;
}

/* Close a file. */
static int wimfs_release(const char *path, struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	return close_wimlib_fd(fd);
}

/* Close a directory */
static int wimfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	return close_wimlib_fd(fd);
}

#ifdef ENABLE_XATTR
/* Remove an alternate data stream through the XATTR interface */
static int wimfs_removexattr(const char *path, const char *name)
{
	struct inode *inode;
	struct ads_entry *ads_entry;
	u16 ads_idx;
	struct wimfs_context *ctx = wimfs_get_context();

	if (!(ctx->mount_flags & WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_XATTR))
		return -ENOTSUP;

	if (strlen(name) < 5 || memcmp(name, "user.", 5) != 0)
		return -ENOATTR;
	name += 5;

	inode = wim_pathname_to_inode(ctx->wim, path);
	if (!inode)
		return -ENOENT;

	ads_entry = inode_get_ads_entry(inode, name, &ads_idx);
	if (!ads_entry)
		return -ENOATTR;
	inode_remove_ads(inode, ads_idx, ctx->wim->lookup_table);
	return 0;
}
#endif

/* Renames a file or directory.  See rename (3) */
static int wimfs_rename(const char *from, const char *to)
{
	struct dentry *src;
	struct dentry *dst;
	struct dentry *parent_of_dst;
	char *file_name_utf16 = NULL, *file_name_utf8 = NULL;
	u16 file_name_utf16_len, file_name_utf8_len;
	WIMStruct *w = wimfs_get_WIMStruct();
	int ret;

	/* This rename() implementation currently only supports actual files
	 * (not alternate data streams) */

	src = get_dentry(w, from);
	if (!src)
		return -ENOENT;

	dst = get_dentry(w, to);

	ret = get_names(&file_name_utf16, &file_name_utf8,
			&file_name_utf16_len, &file_name_utf8_len,
			path_basename(to));
	if (ret != 0)
		return -ENOMEM;

	if (dst) {
		/* Destination file exists */

		if (src == dst) /* Same file */
			return 0;

		if (!dentry_is_directory(src)) {
			/* Cannot rename non-directory to directory. */
			if (dentry_is_directory(dst))
				return -EISDIR;
		} else {
			/* Cannot rename directory to a non-directory or a non-empty
			 * directory */
			if (!dentry_is_directory(dst))
				return -ENOTDIR;
			if (inode_has_children(dst->d_inode))
				return -ENOTEMPTY;
		}
		parent_of_dst = dst->parent;
		remove_dentry(dst, w->lookup_table);
	} else {
		/* Destination does not exist */
		parent_of_dst = get_parent_dentry(w, to);
		if (!parent_of_dst)
			return -ENOENT;

		if (!dentry_is_directory(parent_of_dst))
			return -ENOTDIR;
	}

	FREE(src->file_name);
	FREE(src->file_name_utf8);
	src->file_name          = file_name_utf16;
	src->file_name_utf8     = file_name_utf8;
	src->file_name_len      = file_name_utf16_len;
	src->file_name_utf8_len = file_name_utf8_len;

	unlink_dentry(src);
	dentry_add_child(parent_of_dst, src);
	return 0;
}

/* Remove a directory */
static int wimfs_rmdir(const char *path)
{
	struct dentry *dentry;
	WIMStruct *w = wimfs_get_WIMStruct();

	dentry = get_dentry(w, path);
	if (!dentry)
		return -ENOENT;

	if (!dentry_is_empty_directory(dentry))
		return -ENOTEMPTY;

	remove_dentry(dentry, w->lookup_table);
	return 0;
}

#ifdef ENABLE_XATTR
/* Write an alternate data stream through the XATTR interface */
static int wimfs_setxattr(const char *path, const char *name,
			  const char *value, size_t size, int flags)
{
	struct ads_entry *existing_ads_entry;
	struct ads_entry *new_ads_entry;
	struct lookup_table_entry *existing_lte;
	struct lookup_table_entry *lte;
	struct inode *inode;
	u8 value_hash[SHA1_HASH_SIZE];
	u16 ads_idx;
	struct wimfs_context *ctx = wimfs_get_context();

	if (!(ctx->mount_flags & WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_XATTR))
		return -ENOTSUP;

	if (strlen(name) < 5 || memcmp(name, "user.", 5) != 0)
		return -ENOATTR;
	name += 5;

	inode = wim_pathname_to_inode(ctx->wim, path);
	if (!inode)
		return -ENOENT;

	existing_ads_entry = inode_get_ads_entry(inode, name, &ads_idx);
	if (existing_ads_entry) {
		if (flags & XATTR_CREATE)
			return -EEXIST;
		inode_remove_ads(inode, ads_idx, ctx->wim->lookup_table);
	} else {
		if (flags & XATTR_REPLACE)
			return -ENOATTR;
	}
	new_ads_entry = inode_add_ads(inode, name);
	if (!new_ads_entry)
		return -ENOMEM;

	sha1_buffer((const u8*)value, size, value_hash);

	existing_lte = __lookup_resource(ctx->wim->lookup_table, value_hash);

	if (existing_lte) {
		lte = existing_lte;
		lte->refcnt++;
	} else {
		u8 *value_copy;
		lte = new_lookup_table_entry();
		if (!lte)
			return -ENOMEM;
		value_copy = MALLOC(size);
		if (!value_copy) {
			FREE(lte);
			return -ENOMEM;
		}
		memcpy(value_copy, value, size);
		lte->resource_location            = RESOURCE_IN_ATTACHED_BUFFER;
		lte->attached_buffer              = value_copy;
		lte->resource_entry.original_size = size;
		lte->resource_entry.size          = size;
		lte->resource_entry.flags         = 0;
		copy_hash(lte->hash, value_hash);
		lookup_table_insert(ctx->wim->lookup_table, lte);
	}
	new_ads_entry->lte = lte;
	return 0;
}
#endif

static int wimfs_symlink(const char *to, const char *from)
{
	struct wimfs_context *ctx = wimfs_get_context();
	struct dentry *dentry;
	int ret;

	ret = create_dentry(ctx, from, &dentry);
	if (ret == 0) {
		dentry->d_inode->attributes = FILE_ATTRIBUTE_REPARSE_POINT;
		dentry->d_inode->reparse_tag = WIM_IO_REPARSE_TAG_SYMLINK;
		if (inode_set_symlink(dentry->d_inode, to,
				      ctx->wim->lookup_table, NULL))
		{
			unlink_dentry(dentry);
			free_dentry(dentry);
			ret = -ENOMEM;
		}
	}
	return ret;
}


/* Reduce the size of a file */
static int wimfs_truncate(const char *path, off_t size)
{
	struct dentry *dentry;
	struct lookup_table_entry *lte;
	int ret;
	u16 stream_idx;
	u32 stream_id;
	struct inode *inode;
	struct wimfs_context *ctx = wimfs_get_context();

	ret = lookup_resource(ctx->wim, path, get_lookup_flags(ctx),
			      &dentry, &lte, &stream_idx);

	if (ret != 0)
		return ret;

	if (lte == NULL && size == 0)
		return 0;

	inode = dentry->d_inode;
	if (stream_idx == 0)
		stream_id = 0;
	else
		stream_id = inode->ads_entries[stream_idx - 1].stream_id;

	if (lte->resource_location == RESOURCE_IN_STAGING_FILE) {
		ret = truncate(lte->staging_file_name, size);
		if (ret != 0)
			ret = -errno;
	} else {
		/* File in WIM.  Extract it to the staging directory, but only
		 * the first @size bytes of it. */
		ret = extract_resource_to_staging_dir(inode, stream_id,
						      &lte, size, ctx);
	}
	return ret;
}

/* Unlink a non-directory or alternate data stream */
static int wimfs_unlink(const char *path)
{
	struct dentry *dentry;
	struct lookup_table_entry *lte;
	int ret;
	u16 stream_idx;
	struct wimfs_context *ctx = wimfs_get_context();

	ret = lookup_resource(ctx->wim, path, get_lookup_flags(ctx),
			      &dentry, &lte, &stream_idx);

	if (ret != 0)
		return ret;

	if (stream_idx == 0)
		remove_dentry(dentry, ctx->wim->lookup_table);
	else
		inode_remove_ads(dentry->d_inode, stream_idx - 1,
				 ctx->wim->lookup_table);
	return 0;
}

#ifdef HAVE_UTIMENSAT
/*
 * Change the timestamp on a file dentry.
 *
 * Note that alternate data streams do not have their own timestamps.
 */
static int wimfs_utimens(const char *path, const struct timespec tv[2])
{
	struct dentry *dentry;
	struct inode *inode;
	WIMStruct *w = wimfs_get_WIMStruct();

 	dentry = get_dentry(w, path);
	if (!dentry)
		return -ENOENT;
	inode = dentry->d_inode;

	if (tv[0].tv_nsec != UTIME_OMIT) {
		if (tv[0].tv_nsec == UTIME_NOW)
			inode->last_access_time = get_wim_timestamp();
		else
			inode->last_access_time = timespec_to_wim_timestamp(&tv[0]);
	}
	if (tv[1].tv_nsec != UTIME_OMIT) {
		if (tv[1].tv_nsec == UTIME_NOW)
			inode->last_write_time = get_wim_timestamp();
		else
			inode->last_write_time = timespec_to_wim_timestamp(&tv[1]);
	}
	return 0;
}
#else
static int wimfs_utime(const char *path, struct utimbuf *times)
{
	struct dentry *dentry;
	struct inode *inode;
	WIMStruct *w = wimfs_get_WIMStruct();

 	dentry = get_dentry(w, path);
	if (!dentry)
		return -ENOENT;
	inode = dentry->d_inode;

	inode->last_write_time = unix_timestamp_to_wim(times->modtime);
	inode->last_access_time = unix_timestamp_to_wim(times->actime);
	return 0;
}
#endif

/* Writes to a file in the WIM filesystem.
 * It may be an alternate data stream, but here we don't even notice because we
 * just get a lookup table entry. */
static int wimfs_write(const char *path, const char *buf, size_t size,
		       off_t offset, struct fuse_file_info *fi)
{
	struct wimlib_fd *fd = (struct wimlib_fd*)(uintptr_t)fi->fh;
	int ret;
	u64 now;

	if (!fd)
		return -EBADF;

	wimlib_assert(fd->f_lte);
	wimlib_assert(fd->f_lte->staging_file_name);
	wimlib_assert(fd->staging_fd != -1);
	wimlib_assert(fd->f_inode);

	/* Seek to the requested position */
	if (lseek(fd->staging_fd, offset, SEEK_SET) == -1)
		return -errno;

	/* Write the data. */
	ret = write(fd->staging_fd, buf, size);
	if (ret == -1)
		return -errno;

	now = get_wim_timestamp();
	fd->f_inode->last_write_time = now;
	fd->f_inode->last_access_time = now;
	return ret;
}

static struct fuse_operations wimfs_operations = {
	.access      = wimfs_access,
	.chmod       = wimfs_chmod,
	.destroy     = wimfs_destroy,
#if 0
	.fallocate   = wimfs_fallocate,
#endif
	.fgetattr    = wimfs_fgetattr,
	.ftruncate   = wimfs_ftruncate,
	.getattr     = wimfs_getattr,
#ifdef ENABLE_XATTR
	.getxattr    = wimfs_getxattr,
#endif
	.link        = wimfs_link,
#ifdef ENABLE_XATTR
	.listxattr   = wimfs_listxattr,
#endif
	.mkdir       = wimfs_mkdir,
	.mknod       = wimfs_mknod,
	.open        = wimfs_open,
	.opendir     = wimfs_opendir,
	.read        = wimfs_read,
	.readdir     = wimfs_readdir,
	.readlink    = wimfs_readlink,
	.release     = wimfs_release,
	.releasedir  = wimfs_releasedir,
#ifdef ENABLE_XATTR
	.removexattr = wimfs_removexattr,
#endif
	.rename      = wimfs_rename,
	.rmdir       = wimfs_rmdir,
#ifdef ENABLE_XATTR
	.setxattr    = wimfs_setxattr,
#endif
	.symlink     = wimfs_symlink,
	.truncate    = wimfs_truncate,
	.unlink      = wimfs_unlink,
#ifdef HAVE_UTIMENSAT
	.utimens     = wimfs_utimens,
#else
	.utime       = wimfs_utime,
#endif
	.write       = wimfs_write,

#if FUSE_MAJOR_VERSION > 2 || (FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION >= 8)
	.flag_nullpath_ok = 1,
#endif
#if FUSE_MAJOR_VERSION > 2 || (FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION >= 9)
	.flag_nopath = 1,
	.flag_utime_omit_ok = 1,
#endif
};


/* Mounts an image from a WIM file. */
WIMLIBAPI int wimlib_mount_image(WIMStruct *wim, int image, const char *dir,
				 int mount_flags, WIMStruct **additional_swms,
				 unsigned num_additional_swms,
				 const char *staging_dir)
{
	int argc = 0;
	char *argv[16];
	int ret;
	char *dir_copy;
	struct lookup_table *joined_tab, *wim_tab_save;
	struct image_metadata *imd;
	struct wimfs_context ctx;
	struct hlist_node *cur_node;
	struct inode *inode;

	DEBUG("Mount: wim = %p, image = %d, dir = %s, flags = %d, ",
	      wim, image, dir, mount_flags);

	if (!wim || !dir)
		return WIMLIB_ERR_INVALID_PARAM;

	ret = verify_swm_set(wim, additional_swms, num_additional_swms);
	if (ret != 0)
		return ret;

	if ((mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) && (wim->hdr.total_parts != 1)) {
		ERROR("Cannot mount a split WIM read-write");
		return WIMLIB_ERR_SPLIT_UNSUPPORTED;
	}

	if (num_additional_swms) {
		ret = new_joined_lookup_table(wim, additional_swms,
					      num_additional_swms,
					      &joined_tab);
		if (ret != 0)
			return ret;
		wim_tab_save = wim->lookup_table;
		wim->lookup_table = joined_tab;
	}

	if (mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) {
		ret = wim_run_full_verifications(wim);
		if (ret != 0)
			goto out;
	}

	ret = select_wim_image(wim, image);
	if (ret != 0)
		goto out;

	DEBUG("Selected image %d", image);

	imd = wim_get_current_image_metadata(wim);

	if (imd->root_dentry->refcnt != 1) {
		ERROR("Cannot mount image that was just exported with "
		      "wimlib_export_image()");
		ret = WIMLIB_ERR_INVALID_PARAM;
		goto out;
	}

	if (imd->modified) {
		ERROR("Cannot mount image that was added "
		      "with wimlib_add_image()");
		ret = WIMLIB_ERR_INVALID_PARAM;
		goto out;
	}

	if (mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) {
		ret = lock_wim(wim, wim->fp);
		if (ret != 0)
			goto out;
	}

	if (!(mount_flags & (WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_NONE |
		       WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_XATTR |
		       WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_WINDOWS)))
		mount_flags |= WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_XATTR;


	DEBUG("Initializing struct wimfs_context");
	init_wimfs_context(&ctx);
	ctx.wim = wim;
	ctx.mount_flags = mount_flags;
	ctx.image_inode_list = &imd->inode_list;

	if (mount_flags & WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_WINDOWS)
		ctx.default_lookup_flags = LOOKUP_FLAG_ADS_OK;

	DEBUG("Unlinking message queues in case they already exist");
	ret = set_message_queue_names(&ctx, dir);
	if (ret != 0)
		goto out_unlock;
	unlink_message_queues(&ctx);

	DEBUG("Preparing arguments to fuse_main()");

	dir_copy = STRDUP(dir);
	if (!dir_copy)
		goto out_free_message_queue_names;

	argv[argc++] = "imagex";
	argv[argc++] = dir_copy;

 	/* disable multi-threaded operation for read-write mounts */
	if (mount_flags & WIMLIB_MOUNT_FLAG_READWRITE)
		argv[argc++] = "-s";

	if (mount_flags & WIMLIB_MOUNT_FLAG_DEBUG)
		argv[argc++] = "-d";

	/*
	 * We provide the use_ino option because we are going to assign inode
	 * numbers oursides.  The inodes will be given unique numbers in the
	 * assign_inode_numbers() function, and the static variable @next_ino is
	 * set to the next available inode number.
	 */
	char optstring[256] =
		"use_ino"
		",subtype=wimfs"
		",attr_timeout=0"
#if FUSE_MAJOR_VERSION > 2 || (FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION >= 8)
		",hard_remove"
#endif
		;
	argv[argc++] = "-o";
	argv[argc++] = optstring;
	if ((mount_flags & WIMLIB_MOUNT_FLAG_READWRITE)) {
		/* Read-write mount.  Make the staging directory */
		ret = make_staging_dir(&ctx, staging_dir);
		if (ret != 0)
			goto out_free_dir_copy;
	} else {
		/* Read-only mount */
		strcat(optstring, ",ro");
	}
	argv[argc] = NULL;

#ifdef ENABLE_DEBUG
	{
		int i;
		DEBUG("FUSE command line (argc = %d): ", argc);
		for (i = 0; i < argc; i++) {
			fputs(argv[i], stdout);
			putchar(' ');
		}
		putchar('\n');
		fflush(stdout);
	}
#endif

	/* Mark dentry tree as modified if read-write mount. */
	if (mount_flags & WIMLIB_MOUNT_FLAG_READWRITE) {
		imd->modified = 1;
		imd->has_been_mounted_rw = 1;
	}

	/* Resolve the lookup table entries for every inode in the image, and
	 * assign inode numbers */
	DEBUG("Resolving lookup table entries and assigning inode numbers");

	ctx.next_ino = 1;
	hlist_for_each_entry(inode, cur_node, &imd->inode_list, hlist) {
		inode_resolve_ltes(inode, wim->lookup_table);
		inode->ino = ctx.next_ino++;
	}
	/*ctx.next_ino = assign_inode_numbers(&imd->inode_list);*/
	DEBUG("(next_ino = %"PRIu64")", ctx.next_ino);

	DEBUG("Calling fuse_main()");

	ret = fuse_main(argc, argv, &wimfs_operations, &ctx);

	DEBUG("Returned from fuse_main() (ret = %d)", ret);
	if (ret)
		ret = WIMLIB_ERR_FUSE;
out_free_dir_copy:
	FREE(dir_copy);
out_unlock:
	wim->wim_locked = 0;
out_free_message_queue_names:
	free_message_queue_names(&ctx);
out:
	if (num_additional_swms) {
		free_lookup_table(wim->lookup_table);
		wim->lookup_table = wim_tab_save;
	}
	return ret;
}

/*
 * Unmounts the WIM file that was previously mounted on @dir by using
 * wimlib_mount_image().
 */
WIMLIBAPI int wimlib_unmount_image(const char *dir, int unmount_flags,
				   wimlib_progress_func_t progress_func)
{
	int ret;
	struct wimfs_context wimfs_ctx;

	init_wimfs_context(&wimfs_ctx);

	ret = set_message_queue_names(&wimfs_ctx, dir);
	if (ret != 0)
		goto out;

	ret = open_message_queues(&wimfs_ctx, false);
	if (ret != 0)
		goto out_free_message_queue_names;

	ret = send_unmount_request_msg(wimfs_ctx.unmount_to_daemon_mq,
				       unmount_flags);
	if (ret != 0)
		goto out_close_message_queues;

	ret = execute_fusermount(dir);
	if (ret != 0)
		goto out_close_message_queues;

	struct msg_handler_context handler_ctx = {
		.is_daemon = false,
		.timeout_seconds = 5,
		.unmount = {
			.daemon_pid = 0,
		},
	};

	ret = message_loop(wimfs_ctx.daemon_to_unmount_mq,
			   &unmount_msg_handler_callbacks,
			   &handler_ctx);
	if (ret == 0)
		ret = handler_ctx.unmount.status;
out_close_message_queues:
	close_message_queues(&wimfs_ctx);
out_free_message_queue_names:
	free_message_queue_names(&wimfs_ctx);
out:
	return ret;
}

#else /* WITH_FUSE */


static inline int mount_unsupported_error()
{
	ERROR("wimlib was compiled with --without-fuse, which disables support "
	      "for mounting WIMs.");
	return WIMLIB_ERR_UNSUPPORTED;
}

WIMLIBAPI int wimlib_unmount_image(const char *dir, int unmount_flags,
				   wimlib_progress_func_t progress_func)
{
	return mount_unsupported_error();
}

WIMLIBAPI int wimlib_mount_image(WIMStruct *wim_p, int image, const char *dir,
				 int mount_flags, WIMStruct **additional_swms,
				 unsigned num_additional_swms,
				 const char *staging_dir)
{
	return mount_unsupported_error();
}

#endif /* WITH_FUSE */