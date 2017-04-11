/*
 * Copyright (C) Intel, Inc. 2017
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
 *      Eli Qiao <liyong.qiao@intel.com>
 */

#include <config.h>
#include <stdlib.h>

#include "testutils.h"
#include "virbitmap.h"
#include "virfilewrapper.h"
#include "virresctrl.h"


#define VIR_FROM_THIS VIR_FROM_NONE

struct virResctrlData {
    const char *filename;
    virResctrlType type;
};

static void
GetSchemataStr(virResctrlSchemataPtr schemata, char **str)
{
    size_t i;

    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virBufferAsprintf(&buf, "%s:%u=%s",
                            virResctrlTypeToString(schemata->type),
                            schemata->masks[0]->cache_id,
                            virResctrlBitmap2String(schemata->masks[0]->mask));

    for (i = 1; i < schemata->n_masks; i ++)
        virBufferAsprintf(&buf, ";%u=%s",
                                schemata->masks[i]->cache_id,
                                virResctrlBitmap2String(schemata->masks[i]->mask));

    *str = virBufferContentAndReset(&buf);
}

static int
test_virResctrl(const void *opaque)
{
    struct virResctrlData *data = (struct virResctrlData *) opaque;
    char *dir = NULL;
    char *resctrl = NULL;
    int ret = -1;
    virResctrlSchemataPtr schemata = NULL;
    char *schemata_str = NULL;
    char *schemata_file;

    if (virAsprintf(&resctrl, "%s/virresctrldata/linux-%s/resctrl",
                    abs_srcdir, data->filename) < 0)
        goto cleanup;

    if (virAsprintf(&schemata_file, "%s/virresctrldata/%s-free.schemata",
                    abs_srcdir, virResctrlTypeToString(data->type)) < 0)
        goto cleanup;

    if (virFileWrapperAddPrefix("/sys/fs/resctrl", resctrl) < 0)
        goto cleanup;

    if ((schemata = virResctrlGetFreeCache(data->type)) == NULL)
        goto cleanup;

    virFileWrapperClearPrefixes();

    GetSchemataStr(schemata, &schemata_str);

    if (virTestCompareToFile(schemata_str, schemata_file) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(dir);
    VIR_FREE(resctrl);
    VIR_FREE(schemata_str);
    virResctrlFreeSchemata(schemata);
    return ret;
}

static int
mymain(void)
{
    int ret = 0;

#define DO_TEST_FULL(filename, type) \
    do {                                                                \
        struct virResctrlData data = {filename,                         \
                                      type};                            \
        if (virTestRun(filename, test_virResctrl, &data) < 0)           \
            ret = -1;                                                   \
    } while (0)

    DO_TEST_FULL("resctrl", VIR_RESCTRL_TYPE_L3);
    DO_TEST_FULL("resctrl-cdp", VIR_RESCTRL_TYPE_L3_CODE);
    DO_TEST_FULL("resctrl-cdp", VIR_RESCTRL_TYPE_L3_DATA);

    return ret;
}

VIR_TEST_MAIN(mymain)
