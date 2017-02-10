/*
 * virresctrl.h: header for managing resctrl control
 *
 * Copyright (C) 2016 Intel, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Authors:
 * Eli Qiao <liyong.qiao@intel.com>
 */

#ifndef __VIR_RESCTRL_H__
# define __VIR_RESCTRL_H__

# include "virbitmap.h"
# include "virutil.h"
# include "conf/domain_conf.h"
# include "virthread.h"

#define MAX_CPU_SOCKET_NUM 8

enum {
    VIR_RDT_RESOURCE_L3,
    VIR_RDT_RESOURCE_L3DATA,
    VIR_RDT_RESOURCE_L3CODE,
    VIR_RDT_RESOURCE_L2,
    /* Must be the last */
    VIR_RDT_RESOURCE_LAST
};

VIR_ENUM_DECL(virResCtrl);

typedef struct _virResCacheBank virResCacheBank;
typedef virResCacheBank *virResCacheBankPtr;
struct _virResCacheBank {
    unsigned int host_id;
    unsigned long long cache_size;
    unsigned long long cache_left;
    unsigned long long cache_min;
    virBitmapPtr cpu_mask;

    virMutex lock;
};

/**
 * struct rdt_resource - attributes of an RDT resource
 * @enabled:                    Is this feature enabled on this machine
 * @name:                       Name to use in "schemata" file
 * @num_closid:                 Number of CLOSIDs available
 * @max_cbm:                    Largest Cache Bit Mask allowed
 * @min_cbm_bits:               Minimum number of consecutive bits to be set
 *                              in a cache bit mask
 * @cache_level:                Which cache level defines scope of this domain
 * @num_banks:                  Number of cache bank on this machine.
 * @cache_banks:                Array of cache bank
 */
typedef struct _virResCtrl virResCtrl;
typedef virResCtrl *virResCtrlPtr;
struct _virResCtrl {
        bool                    enabled;
        const char              *name;
        int                     num_closid;
        int                     cbm_len;
        int                     min_cbm_bits;
        const char*             cache_level;
        int                     num_banks;
        virResCacheBankPtr      cache_banks;
};

bool virResCtrlAvailable(void);
int virResCtrlInit(void);
virResCtrlPtr virResCtrlGet(int);
int virResCtrlSetCacheBanks(virDomainCachetunePtr,
        unsigned char *, pid_t *, int);
int virResCtrlUpdate(unsigned char *);
#endif
