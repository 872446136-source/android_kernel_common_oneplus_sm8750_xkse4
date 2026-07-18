// SPDX-License-Identifier: GPL-2.0
/*
 * Boeffla wakelock blocker
 *
 * Original implementation by andip71.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "boeffla_wl_blocker.h"

static char list_wl[LENGTH_LIST_WL];
static char list_wl_default[LENGTH_LIST_WL_DEFAULT];
static char list_wl_search[LENGTH_LIST_WL_SEARCH];
static bool wl_blocker_active;
static bool wl_blocker_debug;
static DEFINE_SPINLOCK(wl_blocker_lock);

static void build_search_string_locked(void)
{
	scnprintf(list_wl_search, sizeof(list_wl_search), ";%s;%s;",
		  list_wl_default, list_wl);
	wl_blocker_active = list_wl_default[0] || list_wl[0];
}

static int copy_sysfs_value(char *dst, size_t dst_size,
			    const char *buf, size_t count)
{
	size_t len = count;

	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
		       buf[len - 1] == ' ' || buf[len - 1] == '\t'))
		len--;

	if (len >= dst_size)
		return -E2BIG;

	memcpy(dst, buf, len);
	dst[len] = '\0';
	return 0;
}

bool boeffla_wl_blocker_should_block(const char *name)
{
	char token[BOEFFLA_WL_NAME_MAX + 3];
	unsigned long flags;
	size_t length;
	bool blocked;
	bool debug;

	if (!name)
		return false;

	length = strnlen(name, BOEFFLA_WL_NAME_MAX + 1);
	if (!length || length > BOEFFLA_WL_NAME_MAX)
		return false;

	scnprintf(token, sizeof(token), ";%s;", name);

	spin_lock_irqsave(&wl_blocker_lock, flags);
	blocked = wl_blocker_active && strstr(list_wl_search, token);
	debug = wl_blocker_debug;
	spin_unlock_irqrestore(&wl_blocker_lock, flags);

	if (debug)
		pr_info_ratelimited("Boeffla WL blocker: %s %s\n", name,
				    blocked ? "blocked" : "allowed");

	return blocked;
}

static ssize_t wakelock_blocker_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	ssize_t ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	ret = sysfs_emit(buf, "%s\n", list_wl);
	spin_unlock_irqrestore(&wl_blocker_lock, flags);
	return ret;
}

static ssize_t wakelock_blocker_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	ret = copy_sysfs_value(list_wl, sizeof(list_wl), buf, count);
	if (!ret)
		build_search_string_locked();
	spin_unlock_irqrestore(&wl_blocker_lock, flags);

	return ret ? ret : count;
}

static ssize_t wakelock_blocker_default_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	unsigned long flags;
	ssize_t ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	ret = sysfs_emit(buf, "%s\n", list_wl_default);
	spin_unlock_irqrestore(&wl_blocker_lock, flags);
	return ret;
}

static ssize_t wakelock_blocker_default_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	ret = copy_sysfs_value(list_wl_default, sizeof(list_wl_default),
			       buf, count);
	if (!ret)
		build_search_string_locked();
	spin_unlock_irqrestore(&wl_blocker_lock, flags);

	return ret ? ret : count;
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	unsigned long flags;
	ssize_t ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	ret = sysfs_emit(buf,
			 "Debug status: %d\nUser list: %s\nDefault list: %s\nActive: %d\n",
			 wl_blocker_debug, list_wl, list_wl_default,
			 wl_blocker_active);
	spin_unlock_irqrestore(&wl_blocker_lock, flags);
	return ret;
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned long flags;
	bool value;
	int ret;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	wl_blocker_debug = value;
	spin_unlock_irqrestore(&wl_blocker_lock, flags);
	return count;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%s\n", BOEFFLA_WL_BLOCKER_VERSION);
}

static DEVICE_ATTR(wakelock_blocker, 0644, wakelock_blocker_show,
		   wakelock_blocker_store);
static DEVICE_ATTR(wakelock_blocker_default, 0644,
		   wakelock_blocker_default_show,
		   wakelock_blocker_default_store);
static DEVICE_ATTR(debug, 0644, debug_show, debug_store);
static DEVICE_ATTR(version, 0444, version_show, NULL);

static struct attribute *boeffla_wl_blocker_attributes[] = {
	&dev_attr_wakelock_blocker.attr,
	&dev_attr_wakelock_blocker_default.attr,
	&dev_attr_debug.attr,
	&dev_attr_version.attr,
	NULL,
};

static const struct attribute_group boeffla_wl_blocker_control_group = {
	.attrs = boeffla_wl_blocker_attributes,
};

static struct miscdevice boeffla_wl_blocker_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "boeffla_wakelock_blocker",
};

static int __init boeffla_wl_blocker_init(void)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&wl_blocker_lock, flags);
	strscpy(list_wl_default, LIST_WL_DEFAULT, sizeof(list_wl_default));
	build_search_string_locked();
	spin_unlock_irqrestore(&wl_blocker_lock, flags);

	ret = misc_register(&boeffla_wl_blocker_control_device);
	if (ret) {
		pr_err("Boeffla WL blocker: misc registration failed: %d\n",
		       ret);
		return ret;
	}

	ret = sysfs_create_group(
		&boeffla_wl_blocker_control_device.this_device->kobj,
		&boeffla_wl_blocker_control_group);
	if (ret) {
		pr_err("Boeffla WL blocker: sysfs setup failed: %d\n", ret);
		misc_deregister(&boeffla_wl_blocker_control_device);
		return ret;
	}

	pr_info("Boeffla WL blocker: driver version %s started\n",
		BOEFFLA_WL_BLOCKER_VERSION);
	return 0;
}

static void __exit boeffla_wl_blocker_exit(void)
{
	sysfs_remove_group(
		&boeffla_wl_blocker_control_device.this_device->kobj,
		&boeffla_wl_blocker_control_group);
	misc_deregister(&boeffla_wl_blocker_control_device);
	pr_info("Boeffla WL blocker: driver stopped\n");
}

module_init(boeffla_wl_blocker_init);
module_exit(boeffla_wl_blocker_exit);

MODULE_DESCRIPTION("Boeffla wakelock blocker");
MODULE_LICENSE("GPL v2");
