// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sysctl.h>
#include <linux/types.h>

#include <linux/sysms_finder.h>

#define SYSMS_RETRY_INTERVAL	(60 * HZ)

enum symbol_state {
	SYMBOL_UNRESOLVED,
	SYMBOL_FOUND,
	SYMBOL_MISSING,
};

struct symbol_entry {
	const char *name;
	unsigned long addr;
	unsigned long retry_after;
	enum symbol_state state;
};

static struct symbol_entry symbols_status[NR_SYMBOLS] = {
	[SYMBOL_GAME_PID] = {
		.name = "game_pid",
	},
};

static DEFINE_MUTEX(symbol_lookup_lock);

static int sysms_game_pid_missing_policy;

#ifdef CONFIG_SYSCTL
static int sysms_game_pid_missing_policy_handler(struct ctl_table *table,
						 int write, void *buffer,
						 size_t *lenp, loff_t *ppos)
{
	struct ctl_table tmp = *table;
	int policy = READ_ONCE(sysms_game_pid_missing_policy);
	int ret;

	tmp.data = &policy;
	ret = proc_dointvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (!ret && write)
		WRITE_ONCE(sysms_game_pid_missing_policy, policy);

	return ret;
}

static struct ctl_table sysms_finder_sysctls[] = {
	{
		.procname	= "sysms_game_pid_missing_policy",
		.data		= NULL,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sysms_game_pid_missing_policy_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{ }
};

static int __init sysms_finder_sysctl_init(void)
{
	if (!register_sysctl("kernel", sysms_finder_sysctls))
		pr_warn_once("sysms_finder: failed to register game PID policy sysctl\n");

	return 0;
}
late_initcall(sysms_finder_sysctl_init);
#endif

unsigned long lookup_symbol(int symbol_index)
{
	struct symbol_entry *entry;
	enum symbol_state state;
	unsigned long addr = 0;

	if (symbol_index < 0 || symbol_index >= NR_SYMBOLS)
		return 0;

	entry = &symbols_status[symbol_index];
	state = smp_load_acquire(&entry->state);
	if (state == SYMBOL_FOUND)
		return READ_ONCE(entry->addr);
	if (state == SYMBOL_MISSING &&
	    time_before(jiffies, READ_ONCE(entry->retry_after)))
		return 0;

	mutex_lock(&symbol_lookup_lock);
	state = entry->state;
	if (state == SYMBOL_FOUND) {
		addr = entry->addr;
		goto out;
	}
	if (state == SYMBOL_MISSING &&
	    time_before(jiffies, entry->retry_after))
		goto out;

	addr = kallsyms_lookup_name(entry->name);
	if (addr) {
		WRITE_ONCE(entry->addr, addr);
		smp_store_release(&entry->state, SYMBOL_FOUND);
		pr_info("sysms_finder: %s found\n", entry->name);
	} else {
		WRITE_ONCE(entry->retry_after, jiffies + SYSMS_RETRY_INTERVAL);
		smp_store_release(&entry->state, SYMBOL_MISSING);
	}

out:
	mutex_unlock(&symbol_lookup_lock);
	return addr;
}

bool check_game_pid(void)
{
	pid_t *game_pid;
	unsigned long addr;

	addr = lookup_symbol(SYMBOL_GAME_PID);
	if (!addr)
		return !READ_ONCE(sysms_game_pid_missing_policy);

	game_pid = (pid_t *)addr;
	return READ_ONCE(*game_pid) == -1;
}
