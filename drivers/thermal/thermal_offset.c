// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/init.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/power_supply.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/thermal.h>
#include <linux/thermal_offset.h>

#include "thermal_core.h"

#define THERMAL_RUNTIME_OFFSET_MIN_MC	(-10000)
#define THERMAL_RUNTIME_OFFSET_MAX_MC	0
#define THERMAL_RUNTIME_OFFSET_STEP_MC	100

static DEFINE_MUTEX(thermal_runtime_offset_lock);
static int thermal_runtime_offsets_mc[THERMAL_RUNTIME_OFFSET_DOMAIN_COUNT];

enum thermal_runtime_offset_domain
thermal_runtime_offset_domain_for_type(const char *type)
{
	if (!type)
		return THERMAL_RUNTIME_OFFSET_NONE;

	if (!strncmp(type, "cpu-", sizeof("cpu-") - 1))
		return THERMAL_RUNTIME_OFFSET_CPU;

	if (!strncmp(type, "gpuss-", sizeof("gpuss-") - 1))
		return THERMAL_RUNTIME_OFFSET_GPU;

	if (!strcmp(type, "ddr"))
		return THERMAL_RUNTIME_OFFSET_DDR;

	if (!strcmp(type, "sys-therm-2"))
		return THERMAL_RUNTIME_OFFSET_SKIN;

	if (!strcmp(type, "shell_front") ||
	    !strcmp(type, "shell_frame") ||
	    !strcmp(type, "shell_back"))
		return THERMAL_RUNTIME_OFFSET_SHELL;

	if (!strcmp(type, "sys-therm-0"))
		return THERMAL_RUNTIME_OFFSET_SYSTEM;

	if (!strcmp(type, "battery"))
		return THERMAL_RUNTIME_OFFSET_BATTERY;

	return THERMAL_RUNTIME_OFFSET_NONE;
}

int thermal_runtime_offset_get_mc(enum thermal_runtime_offset_domain domain)
{
	if (domain <= THERMAL_RUNTIME_OFFSET_NONE ||
	    domain >= THERMAL_RUNTIME_OFFSET_DOMAIN_COUNT)
		return 0;

	return READ_ONCE(thermal_runtime_offsets_mc[domain]);
}

int thermal_runtime_offset_apply(enum thermal_runtime_offset_domain domain,
				 int raw_temp, int *effective_temp)
{
	int converted;

	if (raw_temp == THERMAL_TEMP_INVALID) {
		*effective_temp = raw_temp;
		return 0;
	}

	if (check_add_overflow(raw_temp,
			       thermal_runtime_offset_get_mc(domain),
			       &converted))
		return -EOVERFLOW;

	*effective_temp = converted;
	return 0;
}

int thermal_runtime_offset_to_raw(enum thermal_runtime_offset_domain domain,
				  int effective_temp, int *raw_temp)
{
	int converted;

	if (effective_temp == INT_MAX || effective_temp == -INT_MAX ||
	    effective_temp == THERMAL_TEMP_INVALID) {
		*raw_temp = effective_temp;
		return 0;
	}

	if (check_sub_overflow(effective_temp,
			       thermal_runtime_offset_get_mc(domain),
			       &converted))
		return -EOVERFLOW;

	*raw_temp = converted;
	return 0;
}

bool thermal_runtime_offset_is_battery_supply(const struct power_supply *psy)
{
	return psy && psy->desc &&
		psy->desc->type == POWER_SUPPLY_TYPE_BATTERY &&
		psy->desc->name && !strcmp(psy->desc->name, "battery");
}

int thermal_runtime_offset_get_raw_temp(struct thermal_zone_device *tz,
					int *temp)
{
	enum thermal_runtime_offset_domain domain;

	lockdep_assert_held(&tz->lock);

	domain = thermal_runtime_offset_domain_for_type(tz->type);
	if (domain == THERMAL_RUNTIME_OFFSET_NONE)
		return -EOPNOTSUPP;

	if (domain == THERMAL_RUNTIME_OFFSET_BATTERY) {
		union power_supply_propval value;
		struct power_supply *psy;
		int raw_temp;
		int ret;

		psy = power_supply_get_by_name("battery");
		if (!psy)
			return -ENODEV;

		if (!thermal_runtime_offset_is_battery_supply(psy))
			ret = -ENODEV;
		else
			ret = power_supply_get_property_raw(
				psy, POWER_SUPPLY_PROP_TEMP, &value);

		power_supply_put(psy);
		if (ret)
			return ret;

		if (check_mul_overflow(value.intval, 100, &raw_temp))
			return -EOVERFLOW;

		*temp = raw_temp;
		return 0;
	}

	if (!tz->ops->get_temp)
		return -EINVAL;

	return tz->ops->get_temp(tz, temp);
}

static int thermal_runtime_offset_refresh_zone(
	struct thermal_zone_device *tz, void *data)
{
	enum thermal_runtime_offset_domain domain =
		*(enum thermal_runtime_offset_domain *)data;

	if (thermal_runtime_offset_domain_for_type(tz->type) != domain)
		return 0;

	mutex_lock(&tz->lock);

	if (device_is_registered(&tz->device)) {
		tz->prev_low_trip = INT_MAX;
		tz->prev_high_trip = -INT_MAX;

		if (tz->ops->get_temp)
			__thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);
	}

	mutex_unlock(&tz->lock);

	return 0;
}

static void thermal_runtime_offset_refresh(
	enum thermal_runtime_offset_domain domain)
{
	struct power_supply *psy;

	if (domain == THERMAL_RUNTIME_OFFSET_BATTERY) {
		psy = power_supply_get_by_name("battery");
		if (psy) {
			if (thermal_runtime_offset_is_battery_supply(psy))
				power_supply_changed(psy);
			power_supply_put(psy);
		}
	}

	for_each_thermal_zone(thermal_runtime_offset_refresh_zone, &domain);
}

static int thermal_runtime_offset_handler(struct ctl_table *table, int write,
					  void *buffer, size_t *lenp,
					  loff_t *ppos)
{
	int *offset = table->data;
	enum thermal_runtime_offset_domain domain;
	struct ctl_table tmp = *table;
	int value;
	int ret;

	domain = offset - thermal_runtime_offsets_mc;
	if (domain <= THERMAL_RUNTIME_OFFSET_NONE ||
	    domain >= THERMAL_RUNTIME_OFFSET_DOMAIN_COUNT)
		return -EINVAL;

	mutex_lock(&thermal_runtime_offset_lock);

	value = READ_ONCE(*offset);
	tmp.data = &value;

	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		goto unlock;

	if (value < THERMAL_RUNTIME_OFFSET_MIN_MC ||
	    value > THERMAL_RUNTIME_OFFSET_MAX_MC) {
		ret = -ERANGE;
		goto unlock;
	}

	if (value % THERMAL_RUNTIME_OFFSET_STEP_MC) {
		ret = -EINVAL;
		goto unlock;
	}

	if (value == READ_ONCE(*offset))
		goto unlock;

	WRITE_ONCE(*offset, value);
	thermal_runtime_offset_refresh(domain);

unlock:
	mutex_unlock(&thermal_runtime_offset_lock);

	return ret;
}

#define THERMAL_RUNTIME_OFFSET_SYSCTL(_name, _domain)		\
	{							\
		.procname	= _name,			\
		.data		= &thermal_runtime_offsets_mc[_domain], \
		.maxlen		= sizeof(int),			\
		.mode		= 0644,				\
		.proc_handler	= thermal_runtime_offset_handler,	\
	}

static struct ctl_table thermal_runtime_offset_sysctls[] = {
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_cpu_offset_mc",
				     THERMAL_RUNTIME_OFFSET_CPU),
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_gpu_offset_mc",
				     THERMAL_RUNTIME_OFFSET_GPU),
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_ddr_offset_mc",
				     THERMAL_RUNTIME_OFFSET_DDR),
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_skin_offset_mc",
				     THERMAL_RUNTIME_OFFSET_SKIN),
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_shell_offset_mc",
				     THERMAL_RUNTIME_OFFSET_SHELL),
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_system_offset_mc",
				     THERMAL_RUNTIME_OFFSET_SYSTEM),
	THERMAL_RUNTIME_OFFSET_SYSCTL("thermal_battery_offset_mc",
				     THERMAL_RUNTIME_OFFSET_BATTERY),
	{ }
};

static int __init thermal_runtime_offset_init(void)
{
	register_sysctl_init("kernel", thermal_runtime_offset_sysctls);
	return 0;
}
late_initcall(thermal_runtime_offset_init);
