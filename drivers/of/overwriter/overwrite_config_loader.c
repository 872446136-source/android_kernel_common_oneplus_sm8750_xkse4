// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "../of_private.h"
#include "overwrite_configs.h"

static void __init overwrite_free_property(struct property *prop)
{
	if (!prop)
		return;

	kfree(prop->value);
	kfree(prop->name);
	kfree(prop);
}

static struct property * __init overwrite_alloc_string_property(const char *name,
							 const char *value)
{
	struct property *prop;

	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		return NULL;

	prop->name = kstrdup(name, GFP_KERNEL);
	prop->value = kstrdup(value, GFP_KERNEL);
	if (!prop->name || !prop->value) {
		overwrite_free_property(prop);
		return NULL;
	}
	prop->length = strlen(value) + 1;
#if defined(CONFIG_OF_DYNAMIC) || defined(CONFIG_SPARC)
	of_property_set_flag(prop, OF_DYNAMIC);
#endif

	return prop;
}

static int __init overwrite_remove_node(const char *path)
{
	struct device_node *child, *np, *parent, *previous = NULL;
	unsigned long flags;
	bool detached = false;

	np = of_find_node_by_path(path);
	if (!np)
		return 0;
	parent = of_get_parent(np);
	if (!parent) {
		of_node_put(np);
		return -EINVAL;
	}

	mutex_lock(&of_mutex);
	raw_spin_lock_irqsave(&devtree_lock, flags);
	for (child = parent->child; child; child = child->sibling) {
		if (child != np) {
			previous = child;
			continue;
		}

		if (previous)
			previous->sibling = child->sibling;
		else
			parent->child = child->sibling;
		of_node_set_flag(child, OF_DETACHED);
		__of_phandle_cache_inv_entry(child->phandle);
		detached = true;
		break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	if (detached)
		__of_detach_node_sysfs(np);
	mutex_unlock(&of_mutex);

	of_node_put(parent);
	of_node_put(np);
	return 0;
}

static int __init overwrite_create_node(const char *path)
{
	struct device_node *np, *parent;
	char *parent_path;
	const char *node_name;
	unsigned long flags;
	int ret;

	np = of_find_node_by_path(path);
	if (np) {
		of_node_put(np);
		return 0;
	}

	node_name = strrchr(path, '/');
	if (!node_name || !node_name[1])
		return -EINVAL;

	parent_path = kstrdup(path, GFP_KERNEL);
	if (!parent_path)
		return -ENOMEM;
	parent_path[node_name - path] = '\0';
	parent = of_find_node_by_path(parent_path[0] ? parent_path : "/");
	kfree(parent_path);
	if (!parent)
		return -ENOENT;

	np = kzalloc(sizeof(*np), GFP_KERNEL);
	if (!np) {
		of_node_put(parent);
		return -ENOMEM;
	}
	np->name = kstrdup(node_name + 1, GFP_KERNEL);
	np->full_name = kstrdup(path, GFP_KERNEL);
	if (!np->name || !np->full_name) {
		kfree(np->full_name);
		kfree(np->name);
		kfree(np);
		of_node_put(parent);
		return -ENOMEM;
	}
	of_node_set_flag(np, OF_DYNAMIC);
	of_node_set_flag(np, OF_DETACHED);
	of_node_init(np);

	mutex_lock(&of_mutex);
	raw_spin_lock_irqsave(&devtree_lock, flags);
	np->parent = of_node_get(parent);
	np->sibling = parent->child;
	parent->child = np;
	of_node_clear_flag(np, OF_DETACHED);
	np->fwnode.flags |= FWNODE_FLAG_NOT_DEVICE;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	ret = __of_attach_node_sysfs(np);
	mutex_unlock(&of_mutex);

	of_node_put(parent);
	return ret;
}

static int __init overwrite_split_property_path(char *path, char **node_path,
						 char **prop_name)
{
	char *slash;

	slash = strrchr(path, '/');
	if (!slash || slash == path || !slash[1])
		return -EINVAL;

	*slash = '\0';
	*node_path = path;
	*prop_name = slash + 1;
	return 0;
}

static int __init overwrite_remove_property(char *path)
{
	struct device_node *np;
	struct property *prop;
	char *node_path, *prop_name;
	int ret;

	ret = overwrite_split_property_path(path, &node_path, &prop_name);
	if (ret)
		return ret;

	np = of_find_node_by_path(node_path);
	if (!np)
		return 0;

	prop = of_find_property(np, prop_name, NULL);
	ret = prop ? of_remove_property(np, prop) : 0;
	of_node_put(np);
	return ret;
}

static int __init overwrite_add_string_property(char *path, const char *value)
{
	struct device_node *np;
	struct property *prop;
	char *node_path, *prop_name;
	int ret;

	ret = overwrite_split_property_path(path, &node_path, &prop_name);
	if (ret)
		return ret;

	np = of_find_node_by_path(node_path);
	if (!np)
		return -ENOENT;
	prop = overwrite_alloc_string_property(prop_name, value);
	if (!prop) {
		of_node_put(np);
		return -ENOMEM;
	}

	ret = of_add_property(np, prop);
	if (ret)
		overwrite_free_property(prop);
	of_node_put(np);
	return ret;
}

static int __init overwrite_apply_command(const char *command)
{
	char *copy, *arg, *value = NULL;
	int ret;

	if (!command || !command[0] || command[1] != ' ')
		return -EINVAL;

	copy = kstrdup(command + 2, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	arg = strim(copy);

	if (command[0] == 'a') {
		value = strpbrk(arg, " \t");
		if (!value) {
			ret = -EINVAL;
			goto out;
		}
		*value++ = '\0';
		value = skip_spaces(value);
		if (!value[0]) {
			ret = -EINVAL;
			goto out;
		}
	}

	switch (command[0]) {
	case 'r':
		ret = overwrite_remove_node(arg);
		break;
	case 'c':
		ret = overwrite_create_node(arg);
		break;
	case 'd':
		ret = overwrite_remove_property(arg);
		break;
	case 'a':
		ret = overwrite_add_string_property(arg, value);
		break;
	default:
		ret = -EINVAL;
		break;
	}

out:
	kfree(copy);
	return ret;
}

static int __init overwrite_config_init(void)
{
	const struct overwrite_config_group *group;
	unsigned int i, j;
	int ret;

	for (i = 0; i < overwrite_config_group_count; i++) {
		group = &overwrite_config_groups[i];
		if (strcmp(group->prefix, "common"))
			continue;

		for (j = 0; j < group->count; j++) {
			ret = overwrite_apply_command(group->values[j]);
			if (ret)
				pr_warn_once("OF overwriter: failed to apply configuration (%d)\n",
					     ret);
		}
	}

	return 0;
}
early_initcall(overwrite_config_init);
