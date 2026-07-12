#ifndef _FIRMWARE_OVERLAY_FILES_H
#define _FIRMWARE_OVERLAY_FILES_H

#include <linux/types.h>

enum intercept_status {
	INTERCEPT_STATUS_SUCCESS,
	INTERCEPT_STATUS_ERROR,
	INTERCEPT_STATUS_SKIP,
};

struct firmware;

struct overlay_file {
	const char *name;
	const unsigned char *data;
	size_t len;
	size_t orig_size;
};

extern const struct overlay_file firmware_file_list[];
extern const int firmware_file_list_count;

bool should_intercept_firmware(const char *name);
enum intercept_status intercept_firmware_load(struct firmware *fw,
					      const char *name);

#endif /* _FIRMWARE_OVERLAY_FILES_H */
