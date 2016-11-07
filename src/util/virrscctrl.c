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


#define VIR_FROM_THIS VIR_FROM_RSCCTRL
#define RSC_DIR "/sys/fs/rscctrl"
#define MAX_TASKS_FILE (10*1024*1024)
#define MAX_SCHEMA_LEN 1024
#define MAX_CBM_BIT_LEN 64

/* Test How many bits is 1*/
static int VirBit_Is_1(int bits)
{
    int ret = 0;
    for(int i=0; i<MAX_CBM_BIT_LEN; i++) {
        if((bits & 0x1) == 0x1) ret ++;
        if(bits == 0) break;
        bits = bits >> 1;
    }
    return ret;
}

/*
 * VirWriteSchema to write schema
 */
static int VirWriteSchema(VirRscCtrlPtr p, unsigned long long pid, int *schemas)
{
    int ret = -1;
    char* partition_name = NULL;
    char* schema_str = NULL;
    char* pid_str = NULL;
    char* schema_path = NULL;
    int default_schema = (1 << p->resources[VIR_RscCTRL_L3].info.max_cbm_len) - 1;

    // FIXME(eliqiao): loop schema
    // if(asprintf(&schema_str, "L3:0=%x;1=%x", schema[0], schema[1]) <0)
    // goto cleanup;
    if(0 != pid) {
        // kernel interface of rscctrl doesn't allow us to set the schema of 0
        // use 1 to instead of it
        if(asprintf(&schema_str, "L3:0=%x;1=%x",
                    schemas[0]>0 ? default_schema: 1,
                    schemas[1]>0 ? default_schema: 1) <0)
            goto cleanup;

        if(asprintf(&partition_name, "n-%llu", pid) < 0)
            goto cleanup;

        if(VirRscctrlAddNewPartition(partition_name, schema_str) < 0) {
            VIR_WARN("Failed to create new partition");
            goto cleanup;
        }

        if(asprintf(&pid_str, "%llu", pid) < 0)
            goto cleanup;

        if(VirRscctrlAddTask(partition_name, pid_str) < 0) {
            VIR_WARN("Failed to add %s to partition %s", pid_str, partition_name);
        }
        VIR_FREE(schema_str);
        schema_str = NULL;
    }

    /* update default partition */
    // FIXME(eliqiao): loop schema
    if(asprintf(&schema_str, "L3:0=%x;1=%x", p->resources[VIR_RscCTRL_L3].info.default_schemas[0].schema, p->resources[VIR_RscCTRL_L3].info.default_schemas[1].schema) < 0 )
        goto cleanup;

    if(asprintf(&schema_path, "%s/schemas", RSC_DIR) < 0)
        goto cleanup;

    if (virFileWriteStr(schema_path, schema_str, 0644) < 0) {
        goto cleanup;
    }
    VIR_WARN("default schema  is %s ", schema_str);

    ret = 0;

cleanup:
    VIR_FREE(partition_name);
    VIR_FREE(schema_str);
    VIR_FREE(pid_str);
    VIR_FREE(schema_path);
    return ret;
}

/*
static int VirRscCtrlCalCache(VirRscPartitionPtr p, int l3_cache_per_bit)
{
    int i;
    int cache_sum = 0;
    for(i = 0; i < p->n_sockets; i++ ){
        cache_sum += l3_cache_per_bit * VirBit_Is_1(p->schemas[i].schema);
    }
    return cache_sum;
}
*/

// This function should be called after vm state changes
// such as shutdown, it will update default schema
static int VirRscctrlRefreshSchema(VirRscCtrlPtr p)
{

    VirRscPartitionPtr pPar;
    pPar = p->partitions;
    while(pPar != NULL) {
        if(pPar->tasks && strlen(pPar->tasks) == 0) {
            VirRscctrlRemovePartition(pPar->name);
            for(int i = 0; i < p->resources[VIR_RscCTRL_L3].info.n_sockets; i++) {
                p->resources[VIR_RscCTRL_L3].info.default_schemas[i].schema |= pPar->schemas[i].schema;
            }
        }
        pPar = pPar->next;
    }

    return VirWriteSchema(p, 0, NULL);
}

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

        if(VirRscctrlGetTasks(ent->d_name, &tasks) < 0)
            // fix me return?
            continue;
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
    pvri->n_sockets = nodeinfo.nodes;
    // pvri->n_sockets = 2; // on 2699, it has 2 sockets
    // L3 cache:              56320K
    pvri->l3_cache = nodeinfo.l3_cache;
    pvri->l3_cache_non_shared_left = nodeinfo.l3_cache / 2;
    pvri->l3_cache_shared_left =nodeinfo.l3_cache ;
    // 1048K
    pvri->l3_cache_per_bit = nodeinfo.l3_cache / pvri->n_sockets / pvri->max_cbm_len;
    pvri->shared_schemas = NULL;
    pvri->non_shared_bit = pvri->max_cbm_len / 2;
    pvri->non_shared_schemas = NULL;
    pvri->default_schemas = NULL;

    // init l3 cache left on each socket (removed reserved cache amounts)
    for(int i = 0; i < pvri->n_sockets; i++) {
        pvri->l3_cache_left[i] = pvri->l3_cache_non_shared_left / pvri->n_sockets;
    }

    pvrtype->type = VIR_RscCTRL_L3;
    pvrtype->info = *pvri;
    pvrsc->resources[VIR_RscCTRL_L3] = *pvrtype;
    pvrsc->partitions = VirRscctrlGetAllPartitions(&(pvrsc->npartitions));

    return 0;

cleanup:
    VIR_FREE(pvri);
    VIR_FREE(pvrtype);
    return -1;
}

/*Refresh schemas for partitions*/
int VirInitSchema(VirRscCtrl *pvrsc)
{
    VirRscInfo *pvri = NULL;
    VirRscSchemaPtr pschema = NULL;
    VirRscSchemaPtr pschema_shared = NULL;
    VirRscSchemaPtr pschema_default = NULL;

    pvri = &(pvrsc->resources[VIR_RscCTRL_L3].info);
    int default_schema = (1 << pvri->max_cbm_len) - 1;

    if(VIR_ALLOC_N_QUIET(pschema, pvri->n_sockets) < 0)
        return -1;

    if(VIR_ALLOC_N_QUIET(pschema_shared, pvri->n_sockets) < 0)
        return -1;

    if(VIR_ALLOC_N_QUIET(pschema_default, pvri->n_sockets) < 0)
        return -1;

    VirRscPartitionPtr p;
    p = pvrsc->partitions;

    while(p != NULL) {
        for(int i=0 ; i<pvri->n_sockets; i++) {
            /* 'n' stands for non-shared*/
            if(p->name[0] == 'n') {
                pschema[i].schema |= p->schemas[i].schema;
                pschema[i].socket_no = i;
            }
            /* 's' stand for shared scheam*/
            else if (p->name[0] == 's') {
                pschema_shared[i].schema |= p->schemas[i].schema;
                pschema_shared[i].socket_no = i;
            }
            else {
                pschema_default[i].schema = p->schemas[i].schema;
                pschema_default[i].socket_no = i;
            }
        }
        p = p->next;
    }

    pvri->non_shared_schemas = pschema;
    pvri->shared_schemas = pschema_shared;
    pvri->default_schemas = pschema_default;

   // pvri->l3_cache_non_shared_left =
    int used = 0;
    for(int i=0; i<pvri->n_sockets; i++) {
        used += (VirBit_Is_1(pschema[i].schema) * pvri->l3_cache_per_bit);
    }

    pvri->l3_cache_non_shared_left -= used;
    pvri->l3_cache_shared_left = pvri->l3_cache - pvri->l3_cache_non_shared_left;


    // if the default schema for a socket are changed
    // l3 cache can not be allocated on that socket.
    for(int i = 0 ; i < pvri->n_sockets; i++) {
        if((pvri->default_schemas[i].schema & default_schema) != default_schema)
            pvri->l3_cache_left[i] = 0;
    }
    // refresh default schema
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
        VIR_FREE(prsc->resources[i].info.default_schemas);
    }
    return 0;
}

static int getCellfromCpuId(unsigned int cpu, virCapsPtr caps)
{
    size_t i, j;
    for(i = 0; i < caps->host.nnumaCell; i++)
    {
        for (j = 0; j < caps->host.numaCell[i]->ncpus; j++) {
            if(caps->host.numaCell[i]->cpus[j].id == cpu)
                return i;
        }
    }
    return -1;
}

// Calculate bit mask used for cache
// the aligned size of cache will be returned from actual_cache
static int CalCBMmask(VirRscCtrlPtr pRsc, int cache, unsigned int* actual_cache)
{
    int bit_used, bit_mask;
    bit_used = cache / pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit;
    if(cache % pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit > 0 || bit_used == 0)
        bit_used += 1;

    *actual_cache = bit_used * pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit;

    // construct bit mask by 2 ^ bit_used -1
    // bit_used of 1
    // for ex: bit_use = 2
    // bit_mask will be 11
    bit_mask = (1 << (bit_used)) - 1;
    // move these bit to high bit
    // bit_mask will be
    // 1100 0000 0000 0000 0000
    bit_mask = bit_mask << (pRsc->resources[VIR_RscCTRL_L3].info.max_cbm_len - bit_used);

    return bit_mask;
}


int VirRscCtrlSetL3Cache(unsigned long long pid, virDomainDefPtr def, virCapsPtr caps)
{
    VIR_DEBUG("%llu, %p, %p", pid, (void*)def, (void*)caps);

    size_t i, j, k;
    size_t node_count = virDomainNumaGetNodeCount(def->numa);
    virBitmapPtr p;
    virNodeInfo nodeinfo;
    unsigned int pcpus, actual_cache = 0;
    VirRscCtrl vrc;

    VirInitRscctrl(&vrc);
    VirInitSchema(&vrc);
    VirRscctrlRefreshSchema(&vrc);

    if (nodeGetInfo(&nodeinfo) < 0)
        return -1;

    // FIXME(eliqiao) nodeinfo.sockets should be 2, but it's a
    // wrong number on e5 v4 2699, need to figure out why
    //
    // pcpus = nodeinfo.sockets * nodeinfo.cores * nodeinfo.threads;
    pcpus = nodeinfo.nodes * nodeinfo.cores * nodeinfo.threads;

    int schemas[64] = {0};

    VIR_WARN("node count  %zu", node_count);

    for(i = 0; i < node_count; i++) {
        VIR_WARN("l3_cache for node %zu is %llu", i, virDomainNumaGetNodeL3CacheSize(def->numa, i));
        p = virDomainNumaGetNodeCpumask(def->numa, i);
        // loop each vcpu bitmap to find which vcpu are set
        for(j = 0; j < pcpus; j ++)
        {
            if (virBitmapIsBitSet(p, j))
            {
                virDomainVcpuDefPtr vcpu = def->vcpus[j];
                if (!vcpu->cpumask) {
                    // this should be error and should not be happen
                    continue;
                }
                // loop each pcpu bitmask to find which pcpu are set
                for(k = 0; k < pcpus; k++)
                {
                    if (virBitmapIsBitSet(vcpu->cpumask, k))
                    {
                        int cell_id = getCellfromCpuId(k, caps);
                        // todo return actual_cache for each numa cell;
                        if(cell_id < 0)
                        {
                            virReportError(VIR_ERR_INTERNAL_ERROR,
                                    _("Can't find cell id for cpu %zu"), k);
                        }
                        //TODO return the actual_cache back to vm
                        if(virDomainNumaGetNodeL3CacheSize(def->numa, i) >
                                vrc.resources[VIR_RscCTRL_L3].info.l3_cache_left[cell_id]) {
                            virReportError(VIR_ERR_NO_L3_CACHE,
                                    _("Not enough l3 cache on cell %d"), cell_id);
                            return -1;
                        }
                        else {
                            schemas[cell_id] += CalCBMmask(&vrc,
                                    virDomainNumaGetNodeL3CacheSize(def->numa, i),
                                    &actual_cache);
                            vrc.resources[VIR_RscCTRL_L3].info.l3_cache_left[cell_id] -=
                                actual_cache;
                            // Notes: we break here is assuming all cpumask are on
                            // same node
                            VIR_WARN("actual cache is %d", actual_cache);
                            break;
                        }
                    }
                }
                // Notes: we break here is assuming all cpumask are on
                // same node
                break;
            }
        }
    }

    for(i = 0; i < nodeinfo.nodes; i++) {
        vrc.resources[VIR_RscCTRL_L3].info.default_schemas[i].schema -= schemas[i];
        VIR_WARN("default schema[%zu] after minus is %x",
                 i, vrc.resources[VIR_RscCTRL_L3].info.default_schemas[i].schema);
    }
    if(node_count > 0)
        return VirWriteSchema(&vrc, pid, schemas);
    return 0;
}

int VirRscctrlRefresh(void)
{
    VirRscCtrl vrc;
    VirInitRscctrl(&vrc);
    VirInitSchema(&vrc);
    VirRscctrlRefreshSchema(&vrc);
    return 0;
}

int VirSetL3Cache(unsigned long long pid, unsigned long long cache, int shared)
{
    /*
     * TODO(eliqiao): make this as a global variable then only Do refresh
     * when we need to invoke it.
     */

    /*
     * TODO(eliqiao): need to consider shared and non-shared mode
     * */
    VirRscCtrl vrc;
    VirInitRscctrl(&vrc);
    VirInitSchema(&vrc);
    if(0 == shared)
        return VirRscCtrlSetUnsharedCache(&vrc, pid, cache);
    else
        return VirRscCtrlSetSharedCache(&vrc, pid, cache);
}
/*
int VirRscCtrlSetPowerfulCache(VirRscCtrlPtr pRsc, unsigned long long cache, int cell)
{
    VIR_WARN("cache = %llu, cell = %d", cache, cell);
    int default_schema = (1 << pRsc->resources[VIR_RscCTRL_L3].info.max_cbm_len) - 1;
    if(pRsc->resources[VIR_RscCTRL_L3].info.non_shared_schemas[cellchema != 0)
    {
        VIR_ERROR("Can not set llc for cell %d has an VM setting already", cell);
        return -1;
    }

    if(pRsc->resources[VIR_RscCTRL_L3].info.non_shared_schemas[cellchema != 0)


}
*/
int VirRscCtrlSetUnsharedCache(VirRscCtrlPtr pRsc, unsigned long long pid, unsigned long long cache)
{
    int bit_used;
    int non_shared_bit = 10;

    VIR_WARN("Aabout to set cbm for taskid = %llu", pid);
    VIR_WARN("cache = %llu.", cache);

    if(pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_shared_left < cache) {
        VIR_WARN("not enough cache left");
        return -1;
    }

    bit_used = cache / pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit;
    if(cache % pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit > 0 || bit_used == 0)
        bit_used += 1;

    int cpu_sockets = pRsc->resources[VIR_RscCTRL_L3].info.n_sockets;
    if (bit_used % cpu_sockets != 0) {
        VIR_WARN("increace 1 bit");
        bit_used += 1;
    }

    VIR_WARN("I need to use [%d] bit(s) in the schema", bit_used);

    // sockets
    int bit_used_per_socket = bit_used / cpu_sockets ;

    // construct bit mask by 2 ^ bit_used -1
    // bit_used of 1
    // for ex: bit_use = 2
    // bit_mask will be 11
    int bit_mask = (1 << (bit_used_per_socket)) - 1;

    // move these bit to high bit
    // bit_mask will be
    // 1100000000000000
    bit_mask = bit_mask << (pRsc->resources[VIR_RscCTRL_L3].info.max_cbm_len - bit_used + 1);

    int schema[cpu_sockets];
    VirRscSchemaPtr p = pRsc->resources[VIR_RscCTRL_L3].info.non_shared_schemas;

    for(int i=0; i<cpu_sockets; i++) {
        schema[i] = bit_mask;
         while((p->schema & schema[i]) != 0) schema[i] = schema[i] >> 1;
         // todo need to verify the schema is valid
        if (schema[i] > (1 << (pRsc->resources[VIR_RscCTRL_L3].info.max_cbm_len - non_shared_bit)) -1)
            VIR_WARN("socket %d 's schema is %x", i, schema[i]);
        else {
            /*
             * TODO(eliqiao):
             * */
            printf("Error!!\n");
        }

        /* update non_shared_schemas then later we will use this to
         * update default schema */
        p->schema = p->schema | schema[i];
        p ++;
    }

    return VirWriteSchema(pRsc, pid, NULL);
}

int VirRscCtrlSetSharedCache(VirRscCtrlPtr pRsc, unsigned long long pid, unsigned long long cache)
{
    VIR_WARN("p=%p, pid=%llu, cache=%llu",pRsc, pid, cache);

    if(pRsc->resources[VIR_RscCTRL_L3].info.l3_cache_shared_left < cache) {
        VIR_WARN("not enough cache left");
        return -1;
    }
    /*
     * Loop for each shared schema and find a most fit one
     * */
/*
    VirRscSchemaPtr p = pRsc->resources[VIR_RscCTRL_L3].info.shared_schemas;

    for(int i = 0; i < pRsc->resources[VIR_RscCTRL_L3].info.n_sockets; i++) {
        VIR_WARN("p[i]=%p", &p[i]);
    }
*/
    VirRscPartitionPtr pp = pRsc->partitions;
    /* don't check default partition */
    for(int i = 1; i < pRsc->npartitions; i++) {
        /* Only care about shared partiton*/
        if(pp->name[0] == 'n')
            continue;
    }
    return 0;
}
