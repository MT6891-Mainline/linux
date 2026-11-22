/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_MEDIATEK_OP6893_CONN_H
#define _LINUX_SOC_MEDIATEK_OP6893_CONN_H

#include <linux/errno.h>
#include <linux/of.h>

static inline bool op6893_conn_board(void)
{
	return IS_ENABLED(CONFIG_ARM64_OP6893_CONN_PREFLIGHT) &&
	       of_machine_is_compatible("realme,op6893");
}

#if IS_ENABLED(CONFIG_ARM64_OP6893_CONN_PREFLIGHT)
int op6893_conn_mtcmos_on(void);
void op6893_conn_mtcmos_off(void);
#else
static inline int op6893_conn_mtcmos_on(void)
{
	return -EOPNOTSUPP;
}

static inline void op6893_conn_mtcmos_off(void) {}
#endif

#endif
