/*
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
 * Author: Eli Qiao <liyong.qiao@intel.com>
 */

#include <config.h>

#include "testutils.h"

#ifdef __linux__

# include <stdlib.h>
# include <stdio.h>
# include "virstring.h"
# include "virerror.h"
# include "virlog.h"
# include "virfile.h"
# include "virbuffer.h"
# include "virhostcpu.h"
# include "virrscctrl.h"
# include "nodeinfo.h"

#endif

# define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.rscctrltest");


static int testvirRscctrlAvailable(const void *args ATTRIBUTE_UNUSED)
{
    if (virRscctrlAvailable())
        return 0;
    else
        return -1;
}

static int testvirRscctrlGetL3CbmLen(const void *args ATTRIBUTE_UNUSED)
{
    unsigned int len;
    if (virRscctrlGetMaxL3Cbmlen(&len) < 0) {
        return -1;
    }

    /*eliqiao Fix me*/
    printf("l3CbmLen is %d\n", len);

    if (virRscctrlGetMaxclosId(&len) < 0) {
        return -1;
    }

    /*eliqiao Fix me*/
    printf("l3 max closid is %d\n", len);

    return 0;
}


static int testVirRscctrlAddNewPartition(const void *args ATTRIBUTE_UNUSED)
{
    if(VirRscctrlAddNewPartition("n0", "L3:0=0ffff;1=fffff") < 0) {
        return -1;
    }

    return 0;
    //return VirRscctrlRemovePartition("n0");
}


static int testVirRscctrlGetSchemas(const void *args ATTRIBUTE_UNUSED)
{
    char *schemas;

    if(VirRscctrlGetSchemas("n0", &schemas) < 0) {
        return -1;
    }

    printf("%s\n", schemas);
    VIR_FREE(schemas);
    return 0;
}

static int testVirRscctrlAddTask(const void *args ATTRIBUTE_UNUSED)
{
    char *pids;
//    const char *p = "p0";

/*
    if(VirRscctrlAddTask(p, "162102") < 0) {
        return -1;
    }
*/
    if(VirRscctrlGetTasks("n0", &pids) < 0) {
        return -1;
    }
    printf("get tasks %s\n", pids);
    printf("strlen %zu\n", strlen(pids));
    VIR_FREE(pids);
    return 0;
}


static int testVirRscctrlGetAllPartitions(const void *args ATTRIBUTE_UNUSED)
{
    int len;
    VirRscPartitionPtr p = NULL;
    p = VirRscctrlGetAllPartitions(&len);
    printf("get length is %d\n", len);
    while(p) {
        printf("p->name :%s\n", p->name);
        printf("p->n_sockets :%d\n", p->n_sockets);
        if(p->schemas) {
           for(int i=0; i<p->n_sockets; i++) {
                printf("schemas [ %d ] = %d\n", i, p->schemas[i].schema);
           }
        }
        p = p->next;
    }
    return 0;
}

/*static int write_new(bool shared, int * schemas, char* taskid)
{
    // this function try to create a partition with
    // n-taskid for non shared partition
    // s-taskid for shared partiton
    return 0;
}
*/

static int test_reserve_cache(VirRscCtrl *pvrc)
{
    // This is a test method
    // first I want to have p
    // This should be vm->pid
    const char *taskid = "1234";
    // We need this information when define a domain
    bool shared = false;
    // We need this information when define a domain
    int cache_wanted = 1200;
    // This should be compute by max_cbm_len * non_shared_ratio
    int non_shared_bit = 10;

    // maybe still need to provide cpu ping map if has
    // example could be found:
    // cpumap = vshMalloc(ctl, ncpus * cpumaplen);
    // if ((ncpus = virDomainGetVcpuPinInfo(dom, ncpus, cpumap,
    ////////
    ////////////////////////

    int bit_used = 0;

    printf("system only allow [%d] bits for non shared cache\n", non_shared_bit);

    printf("my id is [%s], I want to reserve [%d] KB cache for [%s]\n", taskid, cache_wanted, shared ? "shared" : "none shared");

    if(pvrc->resources[VIR_RscCTRL_L3].info.l3_cache_shared_left < cache_wanted) {
        printf("not enough cache left, only [%d] left", pvrc->resources[VIR_RscCTRL_L3].info.l3_cache_shared_left);
        return -1;
    }


    // we will support cpu pin later..
    if (1) {
        bit_used = cache_wanted / pvrc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit;
        if(cache_wanted % pvrc->resources[VIR_RscCTRL_L3].info.l3_cache_per_bit > 0 ||
                bit_used == 0)
            bit_used +=1;

        int cpu_sockets = pvrc->resources[VIR_RscCTRL_L3].info.n_sockets;


        if (bit_used % cpu_sockets != 0) {
            printf("I need to increate a bit since I have %d sockets\n", cpu_sockets);
            bit_used +=1;
        }

        printf("I need to use [%d] bit(s) in the schema\n", bit_used);

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
        bit_mask = bit_mask << (pvrc->resources[VIR_RscCTRL_L3].info.max_cbm_len - bit_used);

        int schema[cpu_sockets];
        VirRscSchemaPtr p = pvrc->resources[VIR_RscCTRL_L3].info.non_shared_schemas;

        for(int i=0; i<cpu_sockets; i++) {
            schema[i] = bit_mask;
            while((p->schema & schema[i]) != 0) schema[i] = schema[i] >> 1;

            // todo need to verify the schema is valid
            if (schema[i] > (1 << (pvrc->resources[VIR_RscCTRL_L3].info.max_cbm_len - non_shared_bit)) -1)
                printf("socket %d 's schema is %x\n", i, schema[i]);
            else
                printf("Error!!\n");
            p ++;
        }
    }
    else {
        // in this case we need to rearrange schema for multiple sockets
        ;
    }
    return 0;

}


int main(void)
{
    int ret = 0;

    if (virTestRun("Rscctrl available", testvirRscctrlAvailable, NULL) < 0) {
        printf("-1\n");
        ret = -1;
    }

    if (virTestRun("Rscctrl get l3 cbm len", testvirRscctrlGetL3CbmLen, NULL) < 0) {

        printf("-2\n");
        ret = -2;
    }

    if (virTestRun("Rscctrl get l3 cbm len", testVirRscctrlAddNewPartition, NULL) < 0) {

        printf("-3\n");
        ret = -3;
    }

    if (virTestRun("Rscctrl get l3 cbm len", testVirRscctrlGetSchemas, NULL) < 0) {

        printf("-4\n");
        ret = -4;
    }

    if (virTestRun("Rscctrl get tasks", testVirRscctrlAddTask, NULL) < 0) {

        printf("-5\n");
        ret = -5;
    }


    if (virTestRun("Rscctrl add tasks", testVirRscctrlGetAllPartitions, NULL) < 0) {

        printf("-6\n");
        ret = -6;
    }
    VirRscCtrl vrc;
    VirInitRscctrl(&vrc);
    printf(" %d\n", vrc.resources[VIR_RscCTRL_L3].type);
    printf(" %d\n", vrc.resources[VIR_RscCTRL_L3].info.max_cbm_len);
    printf(" %d\n", vrc.resources[VIR_RscCTRL_L3].info.max_closid);
    printf(" %d\n", vrc.resources[VIR_RscCTRL_L3].info.n_sockets);
    printf(" %d\n", vrc.resources[VIR_RscCTRL_L3].info.l3_cache);
    printf(" %d\n", vrc.resources[VIR_RscCTRL_L3].info.l3_cache_per_bit);


    VirRefreshSchema(&vrc);

    for(int i=0; i<2; i++) {
        printf("non schema is %d\n",vrc.resources[VIR_RscCTRL_L3].info.non_shared_schemas[i].schema);
        printf("shared schema is %d\n",vrc.resources[VIR_RscCTRL_L3].info.shared_schemas[i].schema);
    }

    printf("left non shared cache is %d\n", vrc.resources[VIR_RscCTRL_L3].info.l3_cache_non_shared_left);
    printf("left shared cache is %d\n", vrc.resources[VIR_RscCTRL_L3].info.l3_cache_shared_left);

    VirRscPartitionPtr p = NULL;
    p = vrc.partitions;
    printf("get partition length is %d\n", vrc.npartitions);
    while(p) {
        printf("p->name :%s\n", p->name);
        printf("p->n_sockets :%d\n", p->n_sockets);
        if(p->schemas) {
           for(int j=0; j<p->n_sockets; j++) {
                printf("schemas [ %d ] = %d\n", j, p->schemas[j].schema);
           }
        }
        p = p->next;
    }

    test_reserve_cache(&vrc);

    VirFreeRscctrl(&vrc);
    /* need to expose an interface of current cbm*/
    /* need to get all l3 cache of host*/
    /* need to get cpu sockets*/

    /* refresh partitions if new qemu process start up/reboot/shutdown */

    printf("15 has 1: %d", VirBit_Is_1(15));

    if (ret < 0) {
        printf("failed\n");
    }
    else {
        printf("pass!\n");
    }

    return ret;

}
