// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "../of_private.h"
#include "overwrite_configs.h"

struct overwrite_created_node {
	struct list_head list;
	char *path;
	struct device_node *np;
};

#ifndef CONFIG_OF_DYNAMIC
enum overwrite_action {
	OVERWRITE_ATTACH_NODE,
	OVERWRITE_DETACH_NODE,
	OVERWRITE_ADD_PROPERTY,
	OVERWRITE_REMOVE_PROPERTY,
};

struct overwrite_transaction_entry {
	struct list_head list;
	enum overwrite_action action;
	struct device_node *np;
	struct property *prop;
	struct device_node *previous;
	struct device_node *next;
	bool applied;
};
#endif

struct overwrite_transaction {
#ifdef CONFIG_OF_DYNAMIC
	struct of_changeset changeset;
#else
	struct list_head entries;
#endif
	struct list_head created_nodes;
};

static void __init overwrite_transaction_init(struct overwrite_transaction *transaction)
{
#ifdef CONFIG_OF_DYNAMIC
	of_changeset_init(&transaction->changeset);
#else
	INIT_LIST_HEAD(&transaction->entries);
#endif
	INIT_LIST_HEAD(&transaction->created_nodes);
}

static struct device_node * __init
overwrite_find_node(struct overwrite_transaction *transaction, const char *path)
{
	struct overwrite_created_node *created;

	list_for_each_entry(created, &transaction->created_nodes, list) {
		if (!strcmp(created->path, path))
			return of_node_get(created->np);
	}

	return of_find_node_by_path(path);
}

#ifndef CONFIG_OF_DYNAMIC
static void __init overwrite_free_property(struct property *prop)
{
	if (!prop)
		return;

	kfree(prop->value);
	kfree(prop->name);
	kfree(prop);
}

static struct property * __init
overwrite_alloc_string_property(const char *name, const char *value)
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

static void __init overwrite_free_created_node(struct device_node *np)
{
	struct property *prop, *next;

#if defined(CONFIG_OF_KOBJ)
	kobject_put(&np->kobj);
#endif
	for (prop = np->properties; prop; prop = next) {
		next = prop->next;
		overwrite_free_property(prop);
	}
	for (prop = np->deadprops; prop; prop = next) {
		next = prop->next;
		overwrite_free_property(prop);
	}
	kfree(np->full_name);
	kfree(np->name);
	kfree(np);
}

static void __init overwrite_unlink_dead_property(struct device_node *np,
						  struct property *prop)
{
	struct property **next;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for (next = &np->deadprops; *next; next = &(*next)->next) {
		if (*next != prop)
			continue;
		*next = prop->next;
		prop->next = NULL;
		break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
}

static int __init
overwrite_add_entry(struct overwrite_transaction *transaction,
		    enum overwrite_action action, struct device_node *np,
		    struct property *prop)
{
	struct overwrite_transaction_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->action = action;
	entry->np = of_node_get(np);
	entry->prop = prop;
	list_add_tail(&entry->list, &transaction->entries);
	return 0;
}

static int __init
overwrite_unlink_node(struct overwrite_transaction_entry *entry,
		      bool remember_position)
{
	struct device_node *np = entry->np;
	struct device_node *child, *previous = NULL;
	struct device_node *parent = np->parent;
	unsigned long flags;
	int ret = -ENOENT;

	if (!parent)
		return -EINVAL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for (child = parent->child; child; child = child->sibling) {
		if (child != np) {
			previous = child;
			continue;
		}

		if (remember_position) {
			entry->previous = previous;
			entry->next = child->sibling;
		}
		if (previous)
			previous->sibling = child->sibling;
		else
			parent->child = child->sibling;
		of_node_set_flag(child, OF_DETACHED);
		__of_phandle_cache_inv_entry(child->phandle);
		ret = 0;
		break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!ret)
		__of_detach_node_sysfs(np);
	return ret;
}

static int __init
overwrite_attach_new_node(struct overwrite_transaction_entry *entry)
{
	struct device_node *np = entry->np;
	struct device_node *parent = np->parent;
	unsigned long flags;
	int ret;

	if (!parent || of_node_check_flag(parent, OF_DETACHED))
		return -ENOENT;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np->sibling = parent->child;
	parent->child = np;
	of_node_clear_flag(np, OF_DETACHED);
	np->fwnode.flags |= FWNODE_FLAG_NOT_DEVICE;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	ret = __of_attach_node_sysfs(np);
	if (ret)
		overwrite_unlink_node(entry, false);
	return ret;
}

static int __init
overwrite_restore_detached_node(struct overwrite_transaction_entry *entry)
{
	struct device_node *np = entry->np;
	struct device_node *parent = np->parent;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	if (!of_node_check_flag(np, OF_DETACHED)) {
		ret = -EINVAL;
		goto unlock;
	}
	if (entry->previous) {
		if (entry->previous->sibling != entry->next) {
			ret = -EINVAL;
			goto unlock;
		}
		entry->previous->sibling = np;
	} else {
		if (parent->child != entry->next) {
			ret = -EINVAL;
			goto unlock;
		}
		parent->child = np;
	}
	np->sibling = entry->next;
	of_node_clear_flag(np, OF_DETACHED);
	np->fwnode.flags |= FWNODE_FLAG_NOT_DEVICE;

unlock:
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	if (!ret)
		ret = __of_attach_node_sysfs(np);
	return ret;
}

static int __init
overwrite_apply_entry(struct overwrite_transaction_entry *entry)
{
	int ret;

	switch (entry->action) {
	case OVERWRITE_ATTACH_NODE:
		ret = overwrite_attach_new_node(entry);
		break;
	case OVERWRITE_DETACH_NODE:
		ret = overwrite_unlink_node(entry, true);
		break;
	case OVERWRITE_ADD_PROPERTY:
		ret = __of_add_property(entry->np, entry->prop);
		break;
	case OVERWRITE_REMOVE_PROPERTY:
		ret = __of_remove_property(entry->np, entry->prop);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret)
		entry->applied = true;
	return ret;
}

static int __init
overwrite_revert_entry(struct overwrite_transaction_entry *entry)
{
	int ret;

	switch (entry->action) {
	case OVERWRITE_ATTACH_NODE:
		ret = overwrite_unlink_node(entry, false);
		break;
	case OVERWRITE_DETACH_NODE:
		ret = overwrite_restore_detached_node(entry);
		break;
	case OVERWRITE_ADD_PROPERTY:
		ret = __of_remove_property(entry->np, entry->prop);
		break;
	case OVERWRITE_REMOVE_PROPERTY:
		ret = __of_add_property(entry->np, entry->prop);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret)
		entry->applied = false;
	return ret;
}
#endif

static void __init
overwrite_transaction_destroy(struct overwrite_transaction *transaction,
			      bool applied)
{
	struct overwrite_created_node *created, *next;

#ifdef CONFIG_OF_DYNAMIC
	of_changeset_destroy(&transaction->changeset);
#else
	struct overwrite_transaction_entry *entry, *entry_next;

	list_for_each_entry_safe(entry, entry_next,
				 &transaction->entries, list) {
		list_del(&entry->list);
		if (entry->action == OVERWRITE_ADD_PROPERTY &&
		    !applied && !entry->applied) {
			overwrite_unlink_dead_property(entry->np, entry->prop);
			overwrite_free_property(entry->prop);
		}
		of_node_put(entry->np);
		kfree(entry);
	}
#endif
	list_for_each_entry_safe_reverse(created, next,
					 &transaction->created_nodes, list) {
		list_del(&created->list);
#ifdef CONFIG_OF_DYNAMIC
		if (!applied)
			of_node_put(created->np);
#else
		if (!applied &&
		    of_node_check_flag(created->np, OF_DETACHED))
			overwrite_free_created_node(created->np);
#endif
		kfree(created->path);
		kfree(created);
	}
}

static int __init overwrite_remove_node(struct overwrite_transaction *transaction,
					const char *path)
{
	struct device_node *np, *parent;
	int ret;

	np = overwrite_find_node(transaction, path);
	if (!np)
		return 0;
	parent = of_get_parent(np);
	if (!parent) {
		of_node_put(np);
		return -EINVAL;
	}
	of_node_put(parent);

#ifdef CONFIG_OF_DYNAMIC
	ret = of_changeset_detach_node(&transaction->changeset, np);
#else
	ret = overwrite_add_entry(transaction, OVERWRITE_DETACH_NODE, np,
				  NULL);
#endif
	of_node_put(np);
	return ret;
}

static int __init overwrite_create_node(struct overwrite_transaction *transaction,
					const char *path)
{
	struct overwrite_created_node *created;
	struct device_node *np, *parent;
	char *parent_path;
	const char *node_name;

	np = overwrite_find_node(transaction, path);
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
	parent = overwrite_find_node(transaction,
				     parent_path[0] ? parent_path : "/");
	kfree(parent_path);
	if (!parent)
		return -ENOENT;

	created = kzalloc(sizeof(*created), GFP_KERNEL);
	if (!created) {
		of_node_put(parent);
		return -ENOMEM;
	}
	created->path = kstrdup(path, GFP_KERNEL);
	if (!created->path) {
		kfree(created);
		of_node_put(parent);
		return -ENOMEM;
	}

#ifdef CONFIG_OF_DYNAMIC
	np = of_changeset_create_node(&transaction->changeset, parent, path);
	if (!np) {
		kfree(created->path);
		kfree(created);
		of_node_put(parent);
		return -ENOMEM;
	}
	np->fwnode.flags |= FWNODE_FLAG_NOT_DEVICE;
#else
	np = kzalloc(sizeof(*np), GFP_KERNEL);
	if (!np)
		goto free_created;
	np->name = kstrdup(node_name + 1, GFP_KERNEL);
	np->full_name = kstrdup(path, GFP_KERNEL);
	if (!np->name || !np->full_name) {
		kfree(np->full_name);
		kfree(np->name);
		kfree(np);
		goto free_created;
	}
	np->parent = parent;
	of_node_set_flag(np, OF_DYNAMIC);
	of_node_set_flag(np, OF_DETACHED);
	np->fwnode.flags |= FWNODE_FLAG_NOT_DEVICE;
	of_node_init(np);

	if (overwrite_add_entry(transaction, OVERWRITE_ATTACH_NODE, np, NULL)) {
		overwrite_free_created_node(np);
		goto free_created;
	}
#endif
	of_node_put(parent);
	created->np = np;
	list_add_tail(&created->list, &transaction->created_nodes);

	return 0;

#ifndef CONFIG_OF_DYNAMIC
free_created:
	kfree(created->path);
	kfree(created);
	of_node_put(parent);
	return -ENOMEM;
#endif
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

static int __init
overwrite_remove_property(struct overwrite_transaction *transaction, char *path)
{
	struct device_node *np;
	struct property *prop;
	char *node_path, *prop_name;
	int ret;

	ret = overwrite_split_property_path(path, &node_path, &prop_name);
	if (ret)
		return ret;

	np = overwrite_find_node(transaction, node_path);
	if (!np)
		return 0;

	prop = of_find_property(np, prop_name, NULL);
#ifdef CONFIG_OF_DYNAMIC
	ret = prop ? of_changeset_remove_property(&transaction->changeset,
						  np, prop) : 0;
#else
	ret = prop ? overwrite_add_entry(transaction,
					 OVERWRITE_REMOVE_PROPERTY, np, prop) : 0;
#endif
	of_node_put(np);
	return ret;
}

static int __init
overwrite_add_string_property(struct overwrite_transaction *transaction,
			      char *path, const char *value)
{
	struct device_node *np;
	char *node_path, *prop_name;
	int ret;
#ifndef CONFIG_OF_DYNAMIC
	struct property *prop;
#endif

	ret = overwrite_split_property_path(path, &node_path, &prop_name);
	if (ret)
		return ret;

	np = overwrite_find_node(transaction, node_path);
	if (!np)
		return -ENOENT;

#ifdef CONFIG_OF_DYNAMIC
	ret = of_changeset_add_prop_string(&transaction->changeset, np,
					   prop_name, value);
#else
	prop = overwrite_alloc_string_property(prop_name, value);
	if (!prop) {
		ret = -ENOMEM;
		goto put_node;
	}
	ret = overwrite_add_entry(transaction, OVERWRITE_ADD_PROPERTY, np,
				  prop);
	if (ret)
		overwrite_free_property(prop);

put_node:
#endif
	of_node_put(np);
	return ret;
}

static int __init
overwrite_prepare_command(struct overwrite_transaction *transaction,
			  const char *command)
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
		ret = overwrite_remove_node(transaction, arg);
		break;
	case 'c':
		ret = overwrite_create_node(transaction, arg);
		break;
	case 'd':
		ret = overwrite_remove_property(transaction, arg);
		break;
	case 'a':
		ret = overwrite_add_string_property(transaction, arg, value);
		break;
	default:
		ret = -EINVAL;
		break;
	}

out:
	kfree(copy);
	return ret;
}

static int __init
overwrite_transaction_apply(struct overwrite_transaction *transaction)
{
#ifdef CONFIG_OF_DYNAMIC
	return of_changeset_apply(&transaction->changeset);
#else
	struct overwrite_transaction_entry *entry, *rollback;
	int ret = 0, rollback_ret;

	mutex_lock(&of_mutex);
	list_for_each_entry(entry, &transaction->entries, list) {
		ret = overwrite_apply_entry(entry);
		if (ret)
			break;
	}
	if (ret) {
		list_for_each_entry_reverse(rollback, &transaction->entries,
					    list) {
			if (!rollback->applied)
				continue;
			rollback_ret = overwrite_revert_entry(rollback);
			if (rollback_ret)
				pr_err("OF overwriter: rollback action=%u failed: %d\n",
				       rollback->action, rollback_ret);
		}
	}
	mutex_unlock(&of_mutex);

	return ret;
#endif
}

static int __init overwrite_config_init(void)
{
	struct overwrite_transaction transaction;
	const struct overwrite_config_group *group;
	const char *last_command = NULL;
	const char *last_prefix = NULL;
	unsigned int i, j;
	unsigned int last_index = 0;
	bool applied = false;
	int ret = 0;

	overwrite_transaction_init(&transaction);
	for (i = 0; i < overwrite_config_group_count; i++) {
		group = &overwrite_config_groups[i];
		if (strcmp(group->prefix, "common"))
			continue;

		for (j = 0; j < group->count; j++) {
			ret = overwrite_prepare_command(&transaction,
							group->values[j]);
			if (ret) {
				pr_err("OF overwriter: group=%s command[%u]=\"%s\" prepare failed: %d\n",
				       group->prefix, j, group->values[j], ret);
				goto out;
			}
			last_prefix = group->prefix;
			last_index = j;
			last_command = group->values[j];
		}
	}

	if (last_command) {
		ret = overwrite_transaction_apply(&transaction);
		if (ret) {
			pr_err("OF overwriter: group=%s command[%u]=\"%s\" apply failed: %d\n",
			       last_prefix, last_index, last_command, ret);
			goto out;
		}
		applied = true;
	}

out:
	overwrite_transaction_destroy(&transaction, applied);
	return ret;
}
early_initcall(overwrite_config_init);
