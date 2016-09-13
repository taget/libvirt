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
# include "testutilslxc.h"
# include "virhostcpu.h"
# include "virrscctrl.h"

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
    if(VirRscctrlAddNewPartition("p1", "L3:0=0ffff;1=fffff") < 0) {
        return -1;
    }

    return VirRscctrlRemovePartition("p1");
}


static int testVirRscctrlGetSchemas(const void *args ATTRIBUTE_UNUSED)
{
    char *schemas;

    if(VirRscctrlGetSchemas("p0", &schemas) < 0) {
        return -1;
    }
    printf("%s\n", schemas);
    VIR_FREE(schemas);
    return 0;
}

static int testVirRscctrlAddtask(const void *args ATTRIBUTE_UNUSED)
{
    char *pids;
    const char *p = "p0";

/*
    if(VirRscctrlAddtask(p, "162102") < 0) {
        return -1;
    }
*/

    if(VirRscctrlGetTasks(p, &pids) < 0) {
        return -1;
    }
    printf("%s\n", pids);
    VIR_FREE(pids);
    return 0;
}


static int testVirRscctrlGetAllPartitions(const void *args ATTRIBUTE_UNUSED)
{
    char *partitions;
    VirRscctrlGetAllPartitions(&partitions);
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

    if (virTestRun("Rscctrl add tasks", testVirRscctrlAddtask, NULL) < 0) {

        printf("-5\n");
        ret = -5;
    }


    if (virTestRun("Rscctrl add tasks", testVirRscctrlGetAllPartitions, NULL) < 0) {

        printf("-6\n");
        ret = -6;
    }

    VirRscCtrl vrc;
    VirRscInfo vri;
    VirRscCtrlType vrtype;
    vri.max_cbm_len = virRscctrlGetMaxL3Cbmle() ;
    vri.max_closid = virRscctrlGetMaxclosId();

    vrtype.type = VIR_RscCTRL_L3;
    vrtype.info = vri;
    vrc.resources[VIR_RscCTRL_L3] = vrtype;

    printf("iii %d\n", vrc.resources[VIR_RscCTRL_L3].type);

    if (ret < 0) {
        printf("failed\n");
    }
    else {
        printf("pass!\n");
    }

    return ret;

}
