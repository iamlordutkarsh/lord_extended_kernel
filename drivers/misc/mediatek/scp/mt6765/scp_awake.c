/*
 * Copyright (C) 2017 MediaTek Inc.
 * Copyright (C) 2018 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/module.h>       /* needed by all modules */
#include <linux/init.h>         /* needed by module macros */
#include <linux/fs.h>           /* needed by file_operations* */
#include <linux/miscdevice.h>   /* needed by miscdevice* */
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/device.h>       /* needed by device_* */
#include <linux/vmalloc.h>      /* needed by vmalloc */
#include <linux/uaccess.h>      /* needed by copy_to_user */
#include <linux/fs.h>           /* needed by file_operations* */
#include <linux/slab.h>         /* needed by kmalloc */
#include <linux/poll.h>         /* needed by poll */
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_fdt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <mt-plat/sync_write.h>
#include <mt-plat/aee.h>
#include <linux/delay.h>
#include "scp_feature_define.h"
#include "scp_ipi.h"
#include "scp_helper.h"
#include "scp_excep.h"
#include "scp_dvfs.h"

struct mutex scp_awake_mutexs[SCP_CORE_TOTAL];
int scp_awake_counts[SCP_CORE_TOTAL];


/*
 * acquire scp lock flag, keep scp awake
 * @param scp_core_id: scp core id
 * return  0 :get lock success
 *        -1 :get lock timeout
 */
int scp_awake_lock(enum scp_core_id scp_id)
{
	unsigned long spin_flags;
	struct mutex *scp_awake_mutex;
	char *core_id;
	int *scp_awake_count;
	int count = 0;
	int ret = -1;

	if (scp_id >= SCP_CORE_TOTAL) {
		pr_err("scp_awake_lock: SCP ID >= SCP_CORE_TOTAL\n");
		return ret;
	}

	scp_awake_mutex = &scp_awake_mutexs[scp_id];
	scp_awake_count = (int *)&scp_awake_counts[scp_id];
	core_id = core_ids[scp_id];

	if (is_scp_ready(scp_id) == 0) {
		pr_err("scp_awake_lock: %s not enabled\n", core_id);
		return ret;
	}

	/* scp unlock awake */
	spin_lock_irqsave(&scp_awake_spinlock, spin_flags);
	if (*scp_awake_count > 0) {
		*scp_awake_count = *scp_awake_count + 1;
		spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);
		return 0;
	}
	spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

	mutex_lock(scp_awake_mutex);

	/* spinlock context safe */
	spin_lock_irqsave(&scp_awake_spinlock, spin_flags);

	/*set a direct IPI to awake SCP */
	/*pr_debug("scp_awake_lock: try to awake %s\n", core_id);*/
	writel((1 << AP_AWAKE_LOCK), INFRA_IRQ_SET);

	count = 0;
	while (++count != SCP_AWAKE_TIMEOUT) {
		if (!(readl(INFRA_IRQ_SET) & (1 << AP_AWAKE_LOCK))) {
			ret = 0;
			break;
		}
		udelay(10);
	}

	/* scp lock awake success*/
	if (ret != -1)
		*scp_awake_count = *scp_awake_count + 1;

	/* spinlock context safe */
	spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

	if (ret == -1) {
		pr_err("scp_awake_lock: awake %s fail..\n", core_id);
		WARN_ON(1);
#if SCP_RECOVERY_SUPPORT
		if (scp_set_reset_status() == RESET_STATUS_STOP) {
			pr_notice("scp_awake_lock: start to reset scp...\n");
			scp_send_reset_wq(RESET_TYPE_AWAKE);
		} else
			pr_notice("scp_awake_lock: scp resetting\n");
#endif
	}

	/* scp awake */
	mutex_unlock(scp_awake_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(scp_awake_lock);

/*
 * release scp awake lock flag
 * @param scp_core_id: scp core id
 * return  0 :release lock success
 *        -1 :release lock fail
 */
int scp_awake_unlock(enum scp_core_id scp_id)
{
	struct mutex *scp_awake_mutex;
	unsigned long spin_flags;
	int *scp_awake_count;
	char *core_id;
	int count = 0;
	int ret = -1;

	if (scp_id >= SCP_CORE_TOTAL) {
		pr_err("scp_awake_unlock: SCP ID >= SCP_CORE_TOTAL\n");
		return ret;
	}

	scp_awake_mutex = &scp_awake_mutexs[scp_id];
	scp_awake_count = (int *)&scp_awake_counts[scp_id];
	core_id = core_ids[scp_id];

	if (is_scp_ready(scp_id) == 0) {
		pr_err("scp_awake_unlock: %s not enabled\n", core_id);
		return ret;
	}

	/* scp unlock awake */
	spin_lock_irqsave(&scp_awake_spinlock, spin_flags);
	if (*scp_awake_count > 1) {
		*scp_awake_count = *scp_awake_count - 1;
		spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);
		return 0;
	}
	spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

	mutex_lock(scp_awake_mutex);

	ret = 0;

	/* spinlock context safe */
	spin_lock_irqsave(&scp_awake_spinlock, spin_flags);

	/* set a direct IPI to release awake SCP */
	/*pr_debug("scp_awake_lock: try to awake %s\n", core_id);*/
	writel((1 << AP_AWAKE_UNLOCK), INFRA_IRQ_SET);

	count = 0;
	while (++count != SCP_AWAKE_TIMEOUT) {
		if (!(readl(INFRA_IRQ_SET) & (1 << AP_AWAKE_UNLOCK))) {
			ret = 0;
			break;
		}
		udelay(10);
	}
	/* scp unlock awake success*/
	if (ret != -1) {
		if (*scp_awake_count <= 0)
			pr_err("scp_awake_unlock:%sawake_count=%d NOT SYNC!\n",
						 core_id, *scp_awake_count);

		if (*scp_awake_count > 0)
			*scp_awake_count = *scp_awake_count - 1;
	}

	/* spinlock context safe */
	spin_unlock_irqrestore(&scp_awake_spinlock, spin_flags);

	mutex_unlock(scp_awake_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(scp_awake_unlock);

void scp_reset_awake_counts(void)
{
	int i = 0;

	/* scp ready static flag initialise */
	for (i = 0; i < SCP_CORE_TOTAL ; i++)
		scp_awake_counts[i] = 0;
}

void scp_awake_init(void)
{
	int i = 0;
	/* scp ready static flag initialise */
	for (i = 0; i < SCP_CORE_TOTAL ; i++)
		scp_awake_counts[i] = 0;

	for (i = 0; i < SCP_CORE_TOTAL ; i++)
		mutex_init(&scp_awake_mutexs[i]);

}

void scp_enable_sram(void)
{
	uint32_t reg_temp;

	/*enable sram, enable 1 block per time*/
	for (reg_temp = 0xffffffff; reg_temp != 0;) {
		reg_temp = reg_temp >> 1;
		writel(reg_temp, SCP_SRAM_PDN);
	}
	/*enable scp all TCM*/
	writel(0, SCP_CLK_CTRL_L1_SRAM_PD);
	writel(0, SCP_CLK_CTRL_TCM_TAIL_SRAM_PD);
}

/*
 * scp_sys_reset, reset scp
 */
int scp_sys_full_reset(void)
{
	pr_debug("[SCP]reset\n");

	/*copy loader to scp sram*/
	pr_debug("[SCP]copy to sram\n");
	memcpy_to_scp(SCP_TCM, (const void *)(size_t)scp_loader_base_virt
			, scp_loader_size);
	/*set info to sram*/
	pr_debug("[SCP]set firmware info to sram\n");
	writel(scp_fw_base_phys, SCP_TCM + 0x408);
	writel(scp_fw_size, SCP_TCM + 0x40C);

	return 0;
}

