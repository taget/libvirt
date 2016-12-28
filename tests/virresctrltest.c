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
# include "virresctrl.h"

#endif

# define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.resctrltest");


static int testvirRscctrlAvailable(const void *args ATTRIBUTE_UNUSED)
{
    if (virResCtrlAvailable())
        return 0;
    else
        return -1;
}

static int testfoo(const void * args ATTRIBUTE_UNUSED)
{
    return 0;
}

int main(void)
{
    int ret = 0;

    if (virTestRun("Rscctrl available", testvirRscctrlAvailable, NULL) < 0) {
        ret = -1;
    }
    if (virTestRun("foo", testfoo, NULL) < 0) {
        ret = -2;
    }
    printf("%d\n", ret);
    return ret;
}
