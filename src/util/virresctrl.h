/*
 *  * virrscctrl.h: methods for managing rscctrl
 *  *
 *  * Copyright (C) 2016 Intel, Inc.
 *  *
 *  * This library is free software; you can redistribute it and/or
 *  * modify it under the terms of the GNU Lesser General Public
 *  * License as published by the Free Software Foundation; either
 *  * version 2.1 of the License, or (at your option) any later version.
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

# include "virutil.h"
# include "virbitmap.h"
# include "domain_conf.h"

#define RESCTRL_DIR "/sys/fs/resctrl"
#define RESCTRL_INFO_DIR "/sys/fs/resctrl/info"

// FIXME
#define MAX_CPU_SOCKET_NUM 8
#define MAX_CBM_BIT_LEN 32
#define MAX_SCHEMATA_LEN 1024
#define MAX_FILE_LEN ( 10 * 1024 * 1024)

enum {
    RDT_RESOURCE_L3,
    RDT_RESOURCE_L3DATA,
    RDT_RESOURCE_L3CODE,
    RDT_RESOURCE_L2,
    /* Must be the last */
    RDT_NUM_RESOURCES,
};

typedef struct _virResSchemata virResSchemata;
typedef virResSchemata *virResSchemataPtr;

struct _virResSchemata {
    char *name;
    unsigned int socket_no;
    int schemata;
};

typedef struct _virResDomain virResDomain;
typedef virResDomain *virResDomainPtr;

/**
 * @capable:                    Is this feature available on this machine
 * @name:                       Name to use in "schemata" file
 */

struct _virResDomain {
    char* name;
    virResSchemataPtr schematas;
    char* tasks;
    int n_sockets;
    virResDomainPtr pre;
    virResDomainPtr next;
};

/**
 * struct rdt_resource - attributes of an RDT resource
 * @enabled:                    Is this feature enabled on this machine
 * @capable:                    Is this feature available on this machine
 * @name:                       Name to use in "schemata" file
 * @num_closid:                 Number of CLOSIDs available
 * @max_cbm:                    Largest Cache Bit Mask allowed
 * @min_cbm_bits:               Minimum number of consecutive bits to be set
 *                              in a cache bit mask
 * @domains:                    All domains for this resource
 * @num_domains:                Number of domains active
 * @msr_base:                   Base MSR address for CBMs
 * @tmp_cbms:                   Scratch space when updating schemata
 * @num_tmp_cbms:               Number of CBMs in tmp_cbms
 * @cache_level:                Which cache level defines scope of this domain
 * @cbm_idx_multi:              Multiplier of CBM index
 * @cbm_idx_offset:             Offset of CBM index. CBM index is computed by:
 *                              closid * cbm_idx_multi + cbm_idx_offset
 */

typedef struct _virResCtrl virResCtrl;
typedef virResCtrl *virResCtrlPtr;

struct _virResCtrl {
        bool                    enabled;
        // FIXME should we use all of them?
        bool                    capable;
        const char              *name;
        int                     num_closid;
        int                     cbm_len;
        int                     min_cbm_bits;
        virResDomainPtr         domains;
        int                     num_domains;
        int                     cache_level;
        int                     num_sockets;
        int                     cache_left[MAX_CPU_SOCKET_NUM];
};


bool virResCtrlAvailable(void);
int virResCtrlInit(void);
#endif
