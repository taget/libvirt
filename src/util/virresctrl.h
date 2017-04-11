/*
 * virresctrl.h: header for managing resctrl control
 *
 * Copyright (C) 2017 Intel, Inc.
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

#include "virutil.h"
#include "virbitmap.h"
#include "conf/domain_conf.h"

#define MAX_CACHE_ID 16

typedef enum {
    VIR_RESCTRL_TYPE_L3,
    VIR_RESCTRL_TYPE_L3_CODE,
    VIR_RESCTRL_TYPE_L3_DATA,

    VIR_RESCTRL_TYPE_LAST
} virResctrlType;

VIR_ENUM_DECL(virResctrl);

/*
 * a virResctrlMask represents one of mask object in a
 * resource control group.
 * e.g., 0=f
 */
typedef struct _virResctrlMask virResctrlMask;
typedef virResctrlMask *virResctrlMaskPtr;
struct _virResctrlMask {
    unsigned int cache_id; /* cache resource id */
    virBitmapPtr mask; /* the cbm mask */
};

/*
 * a virResctrlSchemata represents schemata objects of specific type of
 * resource in a resource control group.
 * eg: L3:0=f,1=ff
 */
typedef struct _virResctrlSchemata virResctrlSchemata;
typedef virResctrlSchemata *virResctrlSchemataPtr;
struct _virResctrlSchemata {
    virResctrlType type; /* resource control type, e.g., L3 */
    size_t n_masks; /* number of masks */
    virResctrlMaskPtr masks[MAX_CACHE_ID]; /* array of mask, use array for easy index */
};

/* Get free cache of the host, result saved in schemata */
virResctrlSchemataPtr virResctrlGetFreeCache(virResctrlType type);

/* Get mask string from Bitmap */
char *virResctrlBitmap2String(virBitmapPtr bitmap);

void virResctrlFreeSchemata(virResctrlSchemataPtr ptr);
int virResctrlSetCachetunes(virCapsHostPtr caps,
                            virDomainCachetunePtr cachetune,
                            unsigned char* uuid, pid_t *pids, int npid);
int virResctrlRemoveCachetunes(unsigned char* uuid);
#endif
