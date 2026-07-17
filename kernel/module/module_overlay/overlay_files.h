#ifndef _XKSE4_MODULE_OVERLAY_FILES_H
#define _XKSE4_MODULE_OVERLAY_FILES_H

#include <linux/types.h>

struct load_info;

struct module_overlay_file {
	const char *name;
	const unsigned char *data;
	size_t compressed_size;
	size_t original_size;
};

enum module_overlay_result {
	MODULE_OVERLAY_SKIP,
	MODULE_OVERLAY_REPLACED,
	MODULE_OVERLAY_ERROR,
};

extern const struct module_overlay_file module_overlay_files[];
extern const size_t module_overlay_file_count;

bool module_overlay_has(const char *name);
enum module_overlay_result
module_overlay_replace(struct load_info *info, const char *name);

#endif
