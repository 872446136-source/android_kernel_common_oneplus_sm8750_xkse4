/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_THERMAL_OFFSET_H
#define _LINUX_THERMAL_OFFSET_H

#include <linux/errno.h>
#include <linux/types.h>

struct power_supply;
struct thermal_zone_device;

enum thermal_runtime_offset_domain {
	THERMAL_RUNTIME_OFFSET_NONE,
	THERMAL_RUNTIME_OFFSET_CPU,
	THERMAL_RUNTIME_OFFSET_GPU,
	THERMAL_RUNTIME_OFFSET_DDR,
	THERMAL_RUNTIME_OFFSET_SKIN,
	THERMAL_RUNTIME_OFFSET_SHELL,
	THERMAL_RUNTIME_OFFSET_SYSTEM,
	THERMAL_RUNTIME_OFFSET_BATTERY,
	THERMAL_RUNTIME_OFFSET_DOMAIN_COUNT,
};

#ifdef CONFIG_TEMP_OFFSET
enum thermal_runtime_offset_domain
thermal_runtime_offset_domain_for_type(const char *type);
int thermal_runtime_offset_get_mc(enum thermal_runtime_offset_domain domain);
int thermal_runtime_offset_apply(enum thermal_runtime_offset_domain domain,
				 int raw_temp, int *effective_temp);
int thermal_runtime_offset_to_raw(enum thermal_runtime_offset_domain domain,
				  int effective_temp, int *raw_temp);
bool thermal_runtime_offset_is_battery_supply(const struct power_supply *psy);
int thermal_runtime_offset_get_raw_temp(struct thermal_zone_device *tz,
					int *temp);
#else
static inline enum thermal_runtime_offset_domain
thermal_runtime_offset_domain_for_type(const char *type)
{
	return THERMAL_RUNTIME_OFFSET_NONE;
}

static inline int
thermal_runtime_offset_get_mc(enum thermal_runtime_offset_domain domain)
{
	return 0;
}

static inline int
thermal_runtime_offset_apply(enum thermal_runtime_offset_domain domain,
			     int raw_temp, int *effective_temp)
{
	*effective_temp = raw_temp;
	return 0;
}

static inline int
thermal_runtime_offset_to_raw(enum thermal_runtime_offset_domain domain,
			      int effective_temp, int *raw_temp)
{
	*raw_temp = effective_temp;
	return 0;
}

static inline bool
thermal_runtime_offset_is_battery_supply(const struct power_supply *psy)
{
	return false;
}

static inline int
thermal_runtime_offset_get_raw_temp(struct thermal_zone_device *tz, int *temp)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_THERMAL_OFFSET_H */
