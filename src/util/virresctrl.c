/*
 * virresctrl.c: methods for managing resource control
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
#include <config.h>

#include "virresctrl.h"
#include "viralloc.h"
#include "virerror.h"
#include "virfile.h"
#include "virhostcpu.h"
#include "virlog.h"
#include "virstring.h"
#include "virarch.h"

VIR_LOG_INIT("util.resctrl");

#define VIR_FROM_THIS VIR_FROM_RESCTRL
#define MAX_CPU_SOCKET_NUM 8
#define MAX_CBM_BIT_LEN 32
#define MAX_SCHEMATA_LEN 1024
#define MAX_FILE_LEN (10 * 1024 * 1024)
#define RESCTRL_DIR "/sys/fs/resctrl"
#define RESCTRL_INFO_DIR "/sys/fs/resctrl/info"
#define SYSFS_SYSTEM_PATH "/sys/devices/system"

VIR_ENUM_IMPL(virResCtrl, VIR_RDT_RESOURCE_LAST,
              "l3", "l3data", "l3code", "l2");

#define CONSTRUCT_RESCTRL_PATH(domain_name, item_name) \
do { \
    if (NULL == domain_name) { \
        if (virAsprintf(&path, "%s/%s", RESCTRL_DIR, item_name) < 0)\
            return -1; \
    } else { \
        if (virAsprintf(&path, "%s/%s/%s", RESCTRL_DIR, \
                                        domain_name, \
                                        item_name) < 0) \
            return -1;  \
    } \
} while (0)

#define VIR_RESCTRL_ENABLED(type) \
    resctrlall[type].enabled

#define VIR_RESCTRL_GET_SCHEMATA(count) ((1 << count) - 1)

static unsigned int host_id;

static virResCtrl resctrlall[] = {
    {
        .name = "L3",
        .cache_level = "l3",
    },
    {
        .name = "L3DATA",
        .cache_level = "l3",
    },
    {
        .name = "L3CODE",
        .cache_level = "l3",
    },
    {
        .name = "L2",
        .cache_level = "l2",
    },
};

static int virResCtrlGetInfoStr(const int type, const char *item, char **str)
{
    int ret = 0;
    char *tmp;
    char *path;

    if (virAsprintf(&path, "%s/%s/%s", RESCTRL_INFO_DIR, resctrlall[type].name, item) < 0)
        return -1;
    if (virFileReadAll(path, 10, str) < 0) {
        ret = -1;
        goto cleanup;
    }

    if ((tmp = strchr(*str, '\n'))) *tmp = '\0';

 cleanup:
    VIR_FREE(path);
    return ret;
}

static int virResCtrlReadConfig(virArch arch, int type)
{
    int ret;
    size_t i, nbanks;
    char *str;

    /* Read num_closids from resctrl.
       eg: /sys/fs/resctrl/info/L3/num_closids */
    if ((ret = virResCtrlGetInfoStr(type, "num_closids", &str)) < 0)
        goto error;

    if ((ret = virStrToLong_i(str, NULL, 10, &resctrlall[type].num_closid)) < 0)
        goto error;

    VIR_FREE(str);

    /* Read min_cbm_bits from resctrl.
       eg: /sys/fs/resctrl/info/L3/cbm_mask */
    if ((ret = virResCtrlGetInfoStr(type, "min_cbm_bits", &str)) < 0)
        goto error;

    if ((ret = virStrToLong_i(str, NULL, 10, &resctrlall[type].min_cbm_bits)) < 0)
        goto error;

    VIR_FREE(str);

    /* Read cbm_mask string from resctrl.
       eg: /sys/fs/resctrl/info/L3/cbm_mask */
    if ((ret = virResCtrlGetInfoStr(type, "cbm_mask", &str)) < 0)
        goto error;

    /* cbm_mask is in hex, eg: "fffff", calculate cbm length from the default
       cbm_mask. */
    resctrlall[type].cbm_len = strlen(str) * 4;

    /* Get all cache bank informations */
    resctrlall[type].cache_banks = virHostCPUGetCacheBanks(arch,
                                                           type,
                                                           &nbanks, resctrlall[type].cbm_len);

    if (resctrlall[type].cache_banks == NULL)
        goto error;

    resctrlall[type].num_banks = nbanks;

    for (i = 0; i < resctrlall[type].num_banks; i++) {
        /* L3CODE and L3DATA shares same L3 resource, so they should
         * have same host_id. */
        if (type == VIR_RDT_RESOURCE_L3CODE)
            resctrlall[type].cache_banks[i].host_id = resctrlall[VIR_RDT_RESOURCE_L3DATA].cache_banks[i].host_id;
        else
            resctrlall[type].cache_banks[i].host_id = host_id++;
    }

    resctrlall[type].enabled = true;

    ret = 0;

 error:
    VIR_FREE(str);
    return ret;
}

int
virResCtrlInit(void)
{
    size_t i = 0;
    char *tmp;
    int rc = 0;

    virArch hostarch;

    hostarch = virArchFromHost();

    for (i = 0; i < VIR_RDT_RESOURCE_LAST; i++) {
        if ((rc = virAsprintf(&tmp, "%s/%s", RESCTRL_INFO_DIR, resctrlall[i].name)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Failed to initialize resource control config"));
            goto cleanup;
        }

        if (virFileExists(tmp)) {
            if ((rc = virResCtrlReadConfig(hostarch, i)) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Failed to get resource control config"));
                goto cleanup;
            }
        }
        VIR_FREE(tmp);
    }

 cleanup:
    VIR_FREE(tmp);
    return rc;
}

/*
 * Test whether the host support resource control
 */
bool
virResCtrlAvailable(void)
{
    if (!virFileExists(RESCTRL_INFO_DIR))
        return false;
    return true;
}

/*
 * Return an virResCtrlPtr point to virResCtrl object,
 * We should not modify it out side of virresctrl.c
 */
virResCtrlPtr
virResCtrlGet(int type)
{
    return &resctrlall[type];
}
