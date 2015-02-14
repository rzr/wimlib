#ifndef _WIMBOOT_H_
#define _WIMBOOT_H_

#include "wimlib/header.h"
#include "wimlib/sha1.h"
#include "wimlib/types.h"
#include "wimlib/win32_common.h"

struct blob_info;

extern int
wimboot_alloc_data_source_id(const wchar_t *wim_path,
			     const u8 guid[WIM_GUID_LEN], int image,
			     const wchar_t *target, u64 *data_source_id_ret,
			     bool *wof_running_ret);

extern bool
wimboot_set_pointer(HANDLE h,
		    const struct blob_info *blob,
		    u64 data_source_id,
		    const u8 lookup_table_hash[SHA1_HASH_SIZE],
		    bool wof_running);


#endif /* _WIMBOOT_H_ */
