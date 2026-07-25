/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _OF_OVERWRITE_CONFIGS_H
#define _OF_OVERWRITE_CONFIGS_H

struct overwrite_config_group {
	const char *prefix;
	const char *const *values;
	unsigned int count;
};

extern const struct overwrite_config_group overwrite_config_groups[];
extern const unsigned int overwrite_config_group_count;

#endif /* _OF_OVERWRITE_CONFIGS_H */
