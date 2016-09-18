/*
 * virrscctrl.c: methods for managing control cgroups
 *
 * Copyright Intel Corp. 2016
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
 */
#include <config.h>

#include <stdio.h>
#if defined HAVE_MNTENT_H && defined HAVE_SYS_MOUNT_H \
    && defined HAVE_GETMNTENT_R
# include <mntent.h>
# include <sys/mount.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "virutil.h"
#include "viralloc.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "virhash.h"
#include "virhashcode.h"
#include "virstring.h"
#include "virsystemd.h"
#include "virtypedparam.h"
#include "virhostcpu.h"
#include "nodeinfo.h"

#include "virrscctrl.h"

VIR_LOG_INIT("util.rscctrl");


#define VIR_FROM_THIS VIR_FROM_NONE
#define RSC_DIR "/sys/fs/rscctrl"
#define MAX_TASKS_FILE (10*1024*1024)
#define MAX_SCHEMA_LEN 1024
#define MAX_CBM_BIT_LEN 64

bool virRscctrlAvailable(void)
{
    if (!virFileExists("/sys/fs/rscctrl/info"))
        return false;
    return true;
}

int virRscctrlGetUnsignd(const char *item, unsigned int *len)
{
    int ret = 0;
    char *buf = NULL;
    char *tmp;
    char *path;

    if (asprintf(&path, "/sys/fs/rscctrl/info/l3/%s", item) < 0)
        return -1;

    if (virFileReadAll(path, 10, &buf) < 0) {
        ret = -1;
        goto cleanup;
    }

    VIR_DEBUG("Value is %s", buf);

    if ((tmp = strchr(buf, '\n')))
        *tmp = '\0';

    if (virStrToLong_ui(buf, NULL, 10, len) < 0) {
        ret = -1;
        goto cleanup;
    }

cleanup:
    VIR_FREE(buf);
    VIR_FREE(path);
    return ret;
}

int virRscctrlGetMaxclosId(unsigned int *len)
{
    return virRscctrlGetUnsignd("max_closid", len);
}


int virRscctrlGetMaxL3Cbmlen(unsigned int *len)
{
    return virRscctrlGetUnsignd("max_cbm_len", len);
}


/*
Add new directory in /sys/fs/rscctrl and set schema file
*/
int VirRscctrlAddNewPartition(const char *name, const char *schema)
{
    int rc = 0;
    char *path;
    char *schema_path;
    mode_t mode = 0755;

    if ((rc = asprintf(&path, "%s/%s", RSC_DIR, name)) < 0) {
        goto cleanup;
     }

    if ((rc = virDirCreate(path, mode, 0, 0, 0)) < 0) {
        goto cleanup;
    }

    if ((rc = asprintf(&schema_path, "%s/%s", path, "schemas")) < 0) {
        rmdir(path);
        goto cleanup;
     }

    if ((rc = virFileWriteStr(schema_path, schema, 0644)) < 0) {
        rmdir(path);
        goto cleanup;
    }

cleanup:
    VIR_FREE(path);
    VIR_FREE(schema_path);
    return rc;
}

/*
Remove the Partition, this should only success no pids in tasks
of a partition
*/

int VirRscctrlRemovePartition(const char *name)
{
    char *path = NULL;
    int rc = 0;
    if ((rc = asprintf(&path, "%s/%s", RSC_DIR, name)) < 0) {
        return rc;
     }

    rc = rmdir(path);
    VIR_FREE(path);
    return rc;
}


/*Return schemas of a Partition or Root*/
int VirRscctrlGetSchemas(const char *name, char **schemas)
{
    int rc;
    char *path;
    char *tmp;

    if(name == NULL) {
        if ((rc = asprintf(&path, "%s/schemas", RSC_DIR)) < 0) {
            return rc;
        }
    }
    else {
        if ((rc = asprintf(&path, "%s/%s/schemas", RSC_DIR, name)) < 0) {
            return rc;
        }
    }
    if (virFileReadAll(path, 100, schemas) < 0) {
        rc = -1;
        goto cleanup;
    }

    VIR_DEBUG("Value is %s", *schemas);

    if ((tmp = strchr(*schemas, '\n')))
        *tmp = '\0';

cleanup:
    VIR_FREE(path);
    return rc;
}

/* Return pointer of schema and ncount of schema*/
VirRscSchemaPtr VirParseSchema(const char* schema, int *ncount)
{
    char type[MAX_SCHEMA_LEN];
    const char *p, *q;
    int pos;
    int ischema;
    VirRscSchemaPtr tmpschema;
    VirRscSchemaPtr pschema;

    unsigned int socket_no = 0;
    p = q = schema;
    pos = strchr(schema, ':') - p; 

    if(virStrncpy(type, schema, pos, strlen(schema)) == NULL) {
        return NULL;
    }

    *ncount = 1;
    while((q = strchr(p, ';')) != 0) {
        p = q + 1;
        (*ncount) ++;
    }

    // allocat an arrry to keep schema
    if(VIR_ALLOC_N_QUIET(tmpschema, *ncount) < 0) {
        return NULL;
    }

    pschema = tmpschema;

    p = q = schema + pos + 1;

    char *tmp;

    while(*p != '\0'){
        if (*p == '='){
            q = p + 1;

            if (VIR_STRDUP(tmpschema->name, type) < 0)
                goto cleanup;

            tmpschema->socket_no = socket_no++;

            while(*p != ';' && *p != '\0') p++;

            if (VIR_STRNDUP(tmp, q, p-q) < 0)
                goto cleanup;

            if (virStrToLong_i(tmp, NULL, 16, &ischema) < 0)
                goto cleanup;

            VIR_FREE(tmp);
            tmpschema->schema = ischema;
            tmpschema ++;
        }
        p++;
    }

    return pschema;

cleanup:
    // fix me
    VIR_FREE(pschema);
    return NULL;

}

/*
Get all Partitions from /sys/fs/rscctrl
Return a header pointer of VirRscPartition
In case of a partition has no task at all.
remove it.
*/
VirRscPartitionPtr VirRscctrlGetAllPartitions(int *len)
{
    struct dirent *ent;
    DIR *dp = NULL;
    int direrr;
    char *schemas;
    char *tasks = NULL;
    *len = 0;
    VirRscPartition *header, *tmp, *tmp_pre; 
    header = tmp = tmp_pre = NULL;
    if (virDirOpenQuiet(&dp, RSC_DIR) < 0) {
         if (errno == ENOENT)
             return NULL;
         VIR_ERROR(_("Unable to open %s (%d)"), RSC_DIR, errno);
         goto cleanup;
     }

    // read default partitions
    if(VIR_ALLOC(header) < 0)
        return NULL;

    *len = 1;
    header->pre = NULL;
    header->next = NULL;
    if(VIR_STRDUP(header->name, "default") < 0)
        goto cleanup;
    if (VirRscctrlGetSchemas(NULL, &schemas) < 0) {
        goto cleanup;
    }
    header->schemas = VirParseSchema(schemas, &(header->n_sockets));
    *len = 1;

    while ((direrr = virDirRead(dp, &ent, NULL)) > 0) {
        if ((ent->d_type != DT_DIR) || STREQ(ent->d_name, "info"))
            continue;
        /* here to check if the partition has tasks, if not remove
           the partition dir from host */

        if(VirRscctrlGetTasks(ent->d_name, &tasks) < 0)
            // fix me return?
            continue;

        if(strlen(tasks) == 0) {
            VirRscctrlRemovePartition(ent->d_name);
            VIR_FREE(tasks);
        }

        if(VIR_ALLOC(tmp) < 0)
            return NULL;

        tmp->next = NULL;

        if(header->next == NULL)
            header->next = tmp;

        if(tmp_pre == NULL)
            tmp->pre = header;
        else {
            tmp->pre = tmp_pre;
            tmp_pre->next = tmp;
        }

        if(VIR_STRDUP(tmp->name, ent->d_name) < 0)
            goto cleanup;

        tmp_pre = tmp;
       
        if (VirRscctrlGetSchemas(tmp->name, &schemas) < 0) {
            goto cleanup;
        }

        (*len) ++;
        tmp->schemas = VirParseSchema(schemas, &(tmp->n_sockets));
        tmp->tasks = tasks;
        VIR_FREE(schemas);
    }

    return header;

cleanup:

    VIR_DIR_CLOSE(dp);
    return NULL;
}

/* Append the pid to a tasks file of a Partition. This pid will be automaticly
Moved from the old partition */
int VirRscctrlAddTask(const char *p, const char *pid)
{
    int rc = 0;
    char *tasks_path;
    int writefd;

    /*Root schema*/
    if (p == NULL) {
        if ((rc = asprintf(&tasks_path, "%s/tasks", RSC_DIR)) < 0)
        return rc;
    }
    else {
        if ((rc = asprintf(&tasks_path, "%s/%s/tasks", RSC_DIR, p)) < 0)
        return rc;
    }

    if (!virFileExists(tasks_path)) {
        return -1;
    }
    /* Append pid to tasks file*/
    if ((writefd = open(tasks_path, O_WRONLY | O_APPEND, S_IRUSR | S_IWUSR)) < 0) {
        rc = -1;
        goto cleanup;
    }
    if (safewrite(writefd, pid, strlen(pid)) < 0) {
        rc = -1;
        goto cleanup;
    }

cleanup:
    VIR_FREE(tasks_path);
    VIR_FORCE_CLOSE(writefd);
    return rc;
}

/* Get tasks ids from a Partition*/
int VirRscctrlGetTasks(const char *p, char **pids)
{
    int rc = 0;
    char *tasks_path;
    char *buf;

     /*Root schema*/
    if (p == NULL) {
        if ((rc = asprintf(&tasks_path, "%s/tasks", RSC_DIR)) < 0)
        return rc;
    } else {
        if ((rc = asprintf(&tasks_path, "%s/%s/tasks", RSC_DIR, p)) < 0)
        return rc;
    }

    if (!virFileExists(tasks_path)) {
        return -1;
    }

    if (virFileReadAll(tasks_path, MAX_TASKS_FILE, &buf) < 0) {
        return -1;
    }

    *pids = buf;

    return rc;
}

/* Test How many bits is 1*/
int VirBit_Is_1(int bits)
{
    int ret = 0;
    for(int i=0; i<MAX_CBM_BIT_LEN; i++) {
        if((bits & 0x1) == 0x1) ret ++;
        if(bits == 0) break;
        bits = bits >> 1;
    }
    return ret;
}


int VirInitRscctrl(VirRscCtrl *pvrsc)
{
    VirRscInfo *pvri = NULL;
    VirRscCtrlType *pvrtype = NULL;
    unsigned int max_l3_cbm_len;
    unsigned int max_closed_id;
    virNodeInfo nodeinfo;

    if(VIR_ALLOC(pvri) < 0)
        goto cleanup;
    if(VIR_ALLOC(pvrtype) < 0)
        goto cleanup;
 
    if (virRscctrlGetMaxL3Cbmlen(&max_l3_cbm_len) < 0)
        goto cleanup;
    pvri->max_cbm_len = max_l3_cbm_len;
    if (virRscctrlGetMaxclosId(&max_closed_id) < 0)
        goto cleanup;
    pvri->max_closid = max_closed_id;

    // get system node information
    if (nodeGetInfo(&nodeinfo) < 0)
        goto cleanup;

    // this should be right, but a bug on host, but for now, please work around it first.
    //pvri->n_sockets = nodeinfo.sockets;
    pvri->n_sockets = 2; // on 2699, it has 2 sockets
    // L3 cache:              56320K
    pvri->l3_cache = 56320;
    pvri->l3_cache_non_shared_left = 56320 / 2;
    pvri->l3_cache_shared_left = 56320;
    pvri->l3_cache_per_bit = 56320 / pvri->n_sockets / pvri->max_cbm_len;
    pvri->shared_schemas = NULL;
    pvri->non_shared_bit = pvri->max_cbm_len / 2;
    pvri->non_shared_schemas = NULL;

    pvrtype->type = VIR_RscCTRL_L3;
    pvrtype->info = *pvri;
    pvrsc->resources[VIR_RscCTRL_L3] = *pvrtype;
    pvrsc->partitions = VirRscctrlGetAllPartitions(&(pvrsc->npartitions));

    /* load resources for VIR_RscCTRL_L3*/
    return 0;

cleanup:
    VIR_FREE(pvri);
    VIR_FREE(pvrtype);
    return -1;
}

/*Refresh schemas for partitions*/
int VirRefreshSchema(VirRscCtrl *pvrsc)
{
    VirRscInfo *pvri = NULL;
    VirRscSchemaPtr pschema = NULL;
    VirRscSchemaPtr pschema_shared = NULL;
    
    pvri = &(pvrsc->resources[VIR_RscCTRL_L3].info);
   
    if(VIR_ALLOC_N_QUIET(pschema, pvri->n_sockets) < 0)
        return -1;

    if(VIR_ALLOC_N_QUIET(pschema_shared, pvri->n_sockets) < 0)
        return -1;

    VirRscPartitionPtr p;
    p = pvrsc->partitions;

    while(p != NULL) {
        for(int i=0; i<pvri->n_sockets; i++) {
            /* 'n' stands for non-shard*/
            if(p->name[0] == 'n') {
                pschema[i].schema |= p->schemas[i].schema;
                pschema[i].socket_no = i;
            }
            else {
                pschema_shared[i].schema |= p->schemas[i].schema;
                pschema_shared[i].socket_no = i;
            }
        }
        p = p->next;
    }
    pvri->non_shared_schemas = pschema; 
    pvri->shared_schemas = pschema_shared; 

   // pvri->l3_cache_non_shared_left = 
    int used = 0;
    for(int i=0; i<pvri->n_sockets; i++) {
        used += (VirBit_Is_1(pschema[i].schema) * pvri->l3_cache_per_bit);
    }

    pvri->l3_cache_non_shared_left -= used; 
    pvri->l3_cache_shared_left = pvri->l3_cache - pvri->l3_cache_non_shared_left; 
    return 0; 
}

// free allocated memory for partitions
int VirFreeRscctrl(VirRscCtrlPtr prsc)
{
    if(NULL == prsc)
        return 0;

    VirRscPartitionPtr p, pre;
    p = prsc->partitions;

    while(p != NULL) {
        VIR_FREE(p->schemas);
        pre = p;
        p = p->next;
        VIR_FREE(pre);
    }
    
    for(int i=0; i<VIR_RscCTRL_LAST; i++) {
        VIR_FREE(prsc->resources[i].info.shared_schemas);
        VIR_FREE(prsc->resources[i].info.non_shared_schemas);
    }
    return 0; 
}

