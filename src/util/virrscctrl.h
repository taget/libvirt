/*
 * virrscctrl.h: methods for managing rscctrl
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
 *  Eli Qiao <liyong.qiao@intel.com>
 */

#ifndef __VIR_RscCTRL_H__
# define __VIR_RscCTRL_H__

# include "virutil.h"
# include "virbitmap.h"

#define Rsc_DIR "/sys/fs/rscctrl"
#define MAX_TASKS_FILE (10*1024*1024)

typedef struct _VirRscPartition VirRscPartition;
typedef VirRscPartition *VirRscPartitionPtr;

/* A partition will contains name and a schema*/
struct _VirRscPartition {
    char *name;
    VirRscSchemaPtr *schema;
};

typedef struct _VirRscSchema VirRscSchema;
typedef VirRscSchema *VirRscSchemaPtr;

struct _VirRscSchema {
    char *name;
    int type;
    char *schema;
};


typedef _VirRscInfo VirRscInfo;
struct _VirRscInfo{
    int max_cbm_len;
    int max_closid;
};

typedef _VirRscCtrlType VirRscCtrlType;
struct _VirRscCtrlType {
    int type;
    VirRscInfo info;
};

typedef enum {
    VIR_RscCTRL_L3, /*l3 cache*/
    VIR_RscCTRL_LAST
 } virRscCtrltype;

typedef struct _VirRscCtrl VirRscCtrl;
typedef VirRscCtrl *VirRscCtrlPtr;

/* rscctrl struct*/
struct _VirRscCtrl {
    struct VirRscCtrlType resources[VIR_RscCTRL_LAST];
    VirRscPartitionPtr partitions;
};

bool virRscctrlAvailable(void);

int virRscctrlGetUnsignd(const char *, unsigned int *);
int virRscctrlGetMaxL3Cbmlen(unsigned int *);
int virRscctrlGetMaxclosId(unsigned int *);

int VirRscctrlAddNewPartition(const char *, const char *);
int VirRscctrlRemovePartition(const char *);

int VirRscctrlGetSchemas(const char *, char **);
int VirRscctrlAddtask(const char *, const char *);
int VirRscctrlGetTasks(const char *, char **);
int VirRscctrlGetAllPartitions(char **);
#endif
