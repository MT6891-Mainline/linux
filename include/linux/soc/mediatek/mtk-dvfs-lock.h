/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared voltage-tracking serialization between MediaTek CPUFreq and CCI
 * devfreq drivers when a CPU cluster and the CCI/DSU share Vproc + Vsram rails.
 */
#ifndef __SOC_MEDIATEK_DVFS_LOCK_H
#define __SOC_MEDIATEK_DVFS_LOCK_H

#include <linux/kconfig.h>
#include <linux/mutex.h>

#if IS_REACHABLE(CONFIG_ARM_MEDIATEK_CPUFREQ)
extern struct mutex mtk_dvfs_vtrack_lock;

static inline void mtk_dvfs_vtrack_mutex_lock(void)
{
	mutex_lock(&mtk_dvfs_vtrack_lock);
}

static inline void mtk_dvfs_vtrack_mutex_unlock(void)
{
	mutex_unlock(&mtk_dvfs_vtrack_lock);
}
#else
static inline void mtk_dvfs_vtrack_mutex_lock(void)
{
}

static inline void mtk_dvfs_vtrack_mutex_unlock(void)
{
}
#endif

#endif /* __SOC_MEDIATEK_DVFS_LOCK_H */
