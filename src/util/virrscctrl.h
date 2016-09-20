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

#define RSC_DIR "/sys/fs/rscctrl"
#define MAX_TASKS_FILE (10*1024*1024)
#define MAX_SCHEMA_LEN 1024
#define MAX_CBM_BIT_LEN 64


typedef struct _VirRscSchema VirRscSchema;
typedef VirRscSchema *VirRscSchemaPtr;

struct _VirRscSchema {
    char *name;
    unsigned int socket_no;
    int schema;
};

typedef struct _VirRscPartition VirRscPartition;
typedef VirRscPartition *VirRscPartitionPtr;

/* A partition will contains name and some schemas*/
struct _VirRscPartition {
    char *name;
    int n_sockets; /* n_sockets will indicate length of schema*/
    VirRscSchemaPtr schemas; /* A schema list of a partition*/;
    VirRscPartitionPtr pre;
    VirRscPartitionPtr next;
    char *tasks;
};


typedef struct _VirRscInfo VirRscInfo;
struct _VirRscInfo{
    unsigned int max_cbm_len;
    unsigned int max_closid;
    unsigned l3_cache; /*l3 cache of a host,in KB*/
    unsigned l3_cache_non_shared_left; /*l3 cache left of a host*/
    unsigned l3_cache_shared_left; /*be default it shoulbe equal to l3_cache*/
    unsigned l3_cache_per_bit; /*l3_cache/ max_cbm_len / n_socket*/
    int n_sockets; /*indicate of how many schemas left*/
    VirRscSchemaPtr shared_schemas; /*schemas of currently host by OR of all schemas*/
    VirRscSchemaPtr non_shared_schemas; /*schemas of currently host by OR of all schemas*/
    unsigned int non_shared_bit; /* how many bit are use for non_shared cache*/
};

typedef struct _VirRscCtrlType VirRscCtrlType;
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
    VirRscCtrlType resources[VIR_RscCTRL_LAST];
    VirRscPartitionPtr partitions;
    int npartitions; /*indicate how many partitions have, incluing default*/
};

bool virRscctrlAvailable(void);

int virRscctrlGetUnsignd(const char *, unsigned int *);
int virRscctrlGetMaxL3Cbmlen(unsigned int *);
int virRscctrlGetMaxclosId(unsigned int *);

int VirRscctrlAddNewPartition(const char *, const char *);
int VirRscctrlRemovePartition(const char *);

int VirRscctrlGetSchemas(const char *, char **);
int VirRscctrlAddTask(const char *, const char *);
int VirRscctrlGetTasks(const char *, char **);

VirRscSchemaPtr VirParseSchema(const char* , int *);
VirRscPartitionPtr VirRscctrlGetAllPartitions(int *);
int VirBit_Is_1(int);

int VirInitRscctrl(VirRscCtrl *);
int VirRefreshSchema(VirRscCtrl *);
int VirFreeRscctrl(VirRscCtrlPtr);
#endif
