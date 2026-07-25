/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#ifndef _OPLUS_HMBIRD_VERSION_H_
#define _OPLUS_HMBIRD_VERSION_H_
#include <linux/of.h>
#include <linux/string.h>
#include <linux/printk.h>

enum hmbird_version {
	HMBIRD_UNINIT,
	HMBIRD_GKI_VERSION,
	HMBIRD_OGKI_VERSION,
	HMBIRD_UNKNOW_VERSION,
};

static enum hmbird_version hmbird_version_type = HMBIRD_UNINIT;

#define HMBIRD_VERSION_TYPE_CONFIG_PATH "/soc/oplus,hmbird/version_type"

static inline enum hmbird_version get_hmbird_version_type(void)
{
	struct device_node *np;
	const char *hmbird_version_str;

	if (hmbird_version_type != HMBIRD_UNINIT)
		return hmbird_version_type;

	np = of_find_node_by_path(HMBIRD_VERSION_TYPE_CONFIG_PATH);
	if (np) {
		if (!of_property_read_string(np, "type",
					     &hmbird_version_str)) {
			if (strncmp(hmbird_version_str, "HMBIRD_OGKI", strlen("HMBIRD_OGKI")) == 0) {
				hmbird_version_type = HMBIRD_OGKI_VERSION;
				pr_debug("hmbird version use HMBIRD_OGKI_VERSION, set by dtsi");
			} else if (strncmp(hmbird_version_str, "HMBIRD_GKI", strlen("HMBIRD_GKI")) == 0) {
				hmbird_version_type = HMBIRD_GKI_VERSION;
				pr_debug("hmbird version use HMBIRD_GKI_VERSION, set by dtsi");
			} else {
				hmbird_version_type = HMBIRD_UNKNOW_VERSION;
				pr_debug("hmbird version use default HMBIRD_UNKNOW_VERSION, set by dtsi");
			}
			of_node_put(np);
			return hmbird_version_type;
		}
		of_node_put(np);
	}

	hmbird_version_type = HMBIRD_UNKNOW_VERSION;
	pr_debug("hmbird version use default HMBIRD_UNKNOW_VERSION");
	return hmbird_version_type;
}

#endif /*_OPLUS_HMBIRD_VERSION_H_ */
