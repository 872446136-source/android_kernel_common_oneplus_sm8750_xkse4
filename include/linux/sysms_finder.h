/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SYSMS_FINDER_H
#define _LINUX_SYSMS_FINDER_H

#include <linux/types.h>

enum sysms_symbol {
	SYMBOL_GAME_PID,
	NR_SYMBOLS,
};

unsigned long lookup_symbol(int symbol_index);
bool check_game_pid(void);

#endif /* _LINUX_SYSMS_FINDER_H */
