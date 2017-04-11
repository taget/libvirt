/*
 * virresctrl.c: methods for managing resource control
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
 *  Eli Qiao <liyong.qiao@intel.com>
 */

#include <config.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "virresctrl.h"
#include "virerror.h"
#include "virlog.h"
#include "viralloc.h"
#include "virstring.h"
#include "virfile.h"

VIR_LOG_INIT("util.resctrl");

#define VIR_FROM_THIS VIR_FROM_RESCTRL
#define SYSFS_RESCTRL_PATH "/sys/fs/resctrl"
#define MAX_CBM_LEN 20
#define VIR_RESCTRL_LOCK(fd, op) flock(fd, op)
#define VIR_RESCTRL_UNLOCK(fd) flock(fd, LOCK_UN)
#define CONSTRUCT_RESCTRL_PATH(domain_name, item_name) \
do { \
    if (NULL == domain_name) { \
        if (virAsprintf(&path, "%s/%s", SYSFS_RESCTRL_PATH, item_name) < 0) \
            return -1; \
    } else { \
        if (virAsprintf(&path, "%s/%s/%s", SYSFS_RESCTRL_PATH, domain_name, \
                        item_name) < 0) \
            return -1;  \
    } \
} while (0)

VIR_ENUM_IMPL(virResctrl, VIR_RESCTRL_TYPE_LAST,
              "L3",
              "L3CODE",
              "L3DATA")

/**
 * a virResctrlGroup represents a resource control group, it's a directory
 * under /sys/fs/resctrl.
 * e.g. /sys/fs/resctrl/CG1
 * |-- cpus
 * |-- schemata
 * `-- tasks
 * # cat schemata
 * L3DATA:0=fffff;1=fffff
 * L3CODE:0=fffff;1=fffff
 *
 * Besides, it can also represent the default resource control group of the
 * host.
 */

typedef struct _virResctrlGroup virResctrlGroup;
typedef virResctrlGroup *virResctrlGroupPtr;
struct _virResctrlGroup {
    char *name; /* resource group name, NULL for default host group */
    size_t n_tasks; /* number of tasks assigned to the resource group */
    char **tasks; /* task id list */
    virResctrlSchemataPtr schemata[VIR_RESCTRL_TYPE_LAST]; /* Array for schemata */
};

/* All resource control groups on this host, including default resource group */
typedef struct _virResctrlHost virResctrlHost;
typedef virResctrlHost *virResctrlHostPtr;
struct _virResctrlHost {
    size_t n_groups; /* number of resource control group */
    virResctrlGroupPtr *groups; /* list of resource control group */
};

void
virResctrlFreeSchemata(virResctrlSchemataPtr ptr)
{
    size_t i;

    if (!ptr)
        return;

    for (i = 0; i < ptr->n_masks; i++) {
        virBitmapFree(ptr->masks[i]->mask);
        VIR_FREE(ptr->masks[i]);
    }

    VIR_FREE(ptr);
    ptr = NULL;
}

static void
virResctrlFreeGroup(virResctrlGroupPtr ptr)
{
    size_t i;

    if (!ptr)
        return;

    for (i = 0; i < ptr->n_tasks; i++)
        VIR_FREE(ptr->tasks[i]);
    VIR_FREE(ptr->name);

    for (i = 0; i < VIR_RESCTRL_TYPE_LAST; i++)
        virResctrlFreeSchemata(ptr->schemata[i]);

    VIR_FREE(ptr);
    ptr = NULL;
}

/* Return specify type of schemata string from schematalval.
   e.g., 0=f;1=f */
static int
virResctrlGetSchemataString(virResctrlType type,
                            const char *schemataval,
                            char **schematastr)
{
    int rc = -1;
    char *prefix = NULL;
    char **lines = NULL;

    if (virAsprintf(&prefix,
                    "%s:",
                    virResctrlTypeToString(type)) < 0)
        return -1;

    lines = virStringSplit(schemataval, "\n", 0);

    if (VIR_STRDUP(*schematastr,
                   virStringListGetFirstWithPrefix(lines, prefix)) < 0)
        goto cleanup;

    if (*schematastr == NULL)
        rc = -1;
    else
        rc = 0;

 cleanup:
    VIR_FREE(prefix);
    virStringListFree(lines);
    return rc;
}

static int
virResctrlRemoveSysGroup(const char* name)
{
    char *path = NULL;
    int ret = -1;

    if ((ret = virAsprintf(&path, "%s/%s", SYSFS_RESCTRL_PATH, name)) < 0)
        return ret;

    ret = rmdir(path);

    VIR_FREE(path);
    return ret;
}

static int
virResctrlNewSysGroup(const char *name)
{
    char *path = NULL;
    int ret = -1;
    mode_t mode = 0755;

    if (virAsprintf(&path, "%s/%s", SYSFS_RESCTRL_PATH, name) < 0)
        return -1;

    if (virDirCreate(path, mode, 0, 0, 0) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(path);
    return ret;
}

static int
virResctrlWrite(const char *name, const char *item, const char *content)
{
    char *path;
    int writefd;
    int rc = -1;

    CONSTRUCT_RESCTRL_PATH(name, item);

    if (!virFileExists(path))
        goto cleanup;

    if ((writefd = open(path, O_WRONLY | O_APPEND, S_IRUSR | S_IWUSR)) < 0)
        goto cleanup;

    if (safewrite(writefd, content, strlen(content)) < 0)
        goto cleanup;
    rc = 0;

 cleanup:
    VIR_FREE(path);
    VIR_FORCE_CLOSE(writefd);
    return rc;
}

static
virBitmapPtr virResctrlMask2Bitmap(const char *mask)
{
    virBitmapPtr bitmap;
    unsigned int tmp;
    size_t i;

    if (virStrToLong_ui(mask, NULL, 16, &tmp) < 0)
        return NULL;

    bitmap = virBitmapNewEmpty();

    for (i = 0; i < MAX_CBM_LEN; i++) {
        if (((tmp & 0x1) == 0x1) &&
                (virBitmapSetBitExpand(bitmap, i) < 0))
            goto error;
        tmp = tmp >> 1;
    }
    return bitmap;

 error:
    virBitmapFree(bitmap);
    return NULL;
}

char *virResctrlBitmap2String(virBitmapPtr bitmap)
{
    char *tmp;
    char *ret = NULL;
    char *p;
    tmp = virBitmapString(bitmap);
    /* skip "0x" */
    p = tmp + 2;

    /* first non-0 position */
    while (*++p == '0');

    if (VIR_STRDUP(ret, p) < 0)
        ret = NULL;

    VIR_FREE(tmp);
    return ret;
}

static int
virResctrlParseSchemata(const char* schemata_str,
                        virResctrlSchemataPtr schemata)
{
    VIR_DEBUG("schemata_str=%s, schemata=%p", schemata_str, schemata);

    int ret = -1;
    size_t i;
    virResctrlMaskPtr mask;
    char **schemata_list;
    char *mask_str;

    /* parse 0=fffff;1=f */
    schemata_list = virStringSplit(schemata_str, ";", 0);

    if (!schemata_list)
        goto cleanup;

    for (i = 0; schemata_list[i] != NULL; i++) {
        /* parse 0=fffff */
        mask_str = strchr(schemata_list[i], '=');

        if (!mask_str)
            goto cleanup;

        if (VIR_ALLOC(mask) < 0)
            goto cleanup;

        mask->cache_id = i;
        mask->mask = virResctrlMask2Bitmap(mask_str + 1);
        schemata->n_masks += 1;
        schemata->masks[i] = mask;

    }
    ret = 0;

 cleanup:
    virStringListFree(schemata_list);
    return ret;
}

static int
virResctrlLoadGroup(const char *name,
                    virResctrlHostPtr host)
{
    VIR_DEBUG("name=%s, host=%p\n", name, host);

    int ret = -1;
    char *schemataval = NULL;
    char *schemata_str = NULL;
    virResctrlType i;
    int rv;
    virResctrlGroupPtr grp;
    virResctrlSchemataPtr schemata;

    rv = virFileReadValueString(&schemataval,
                                SYSFS_RESCTRL_PATH "/%s/schemata",
                                name ? name : "");

    if (rv < 0)
        return -1;

    if (VIR_ALLOC(grp) < 0)
        goto cleanup;

    if (VIR_STRDUP(grp->name, name) < 0)
        goto cleanup;

    for (i = 0; i < VIR_RESCTRL_TYPE_LAST; i++) {
        rv = virResctrlGetSchemataString(i, schemataval, &schemata_str);

        if (rv < 0)
            continue;

        if (VIR_ALLOC(schemata) < 0)
            goto cleanup;

        schemata->type = i;

        if (virResctrlParseSchemata(schemata_str, schemata) < 0) {
            VIR_FREE(schemata);
            VIR_FREE(schemata_str);
            goto cleanup;
        }

        grp->schemata[i] = schemata;
        VIR_FREE(schemata_str);
    }

    if (VIR_APPEND_ELEMENT(host->groups,
                           host->n_groups,
                           grp) < 0) {
        virResctrlFreeGroup(grp);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(schemataval);
    return ret;
}

static int
virResctrlLoadHost(virResctrlHostPtr host)
{
    int rv = -1;
    DIR *dirp = NULL;
    char *path = NULL;
    struct dirent *ent;

    rv = virDirOpenIfExists(&dirp, SYSFS_RESCTRL_PATH);
    if (rv < 0)
        return -1;

    /* load default group first */
    if (virResctrlLoadGroup(NULL, host) < 0)
        return -1;

    while ((rv = virDirRead(dirp, &ent, path)) > 0) {
        /* resctrl is not hierarchical, only read directory under
           /sys/fs/resctrl */
        if ((ent->d_type != DT_DIR) || STREQ(ent->d_name, "info"))
            continue;

        if (virResctrlLoadGroup(ent->d_name, host) < 0)
            return -1;
    }
    return 0;
}

static void
virResctrlRefreshHost(virResctrlHostPtr host)
{
    virResctrlGroupPtr default_grp = NULL;
    virResctrlSchemataPtr schemata = NULL;
    size_t i, j;
    virResctrlType t;

    default_grp = host->groups[0];

    for (t = 0; t < VIR_RESCTRL_TYPE_LAST; t++) {
        if (default_grp->schemata[t] != NULL) {
            for (i = 0; i < default_grp->schemata[t]->n_masks; i++) {
                /* Reset default group's mask */
                virBitmapSetAll(default_grp->schemata[t]->masks[i]->mask);
                /* Loop each other resource group except default group */
                for (j = 1; j < host->n_groups; j++) {
                    schemata = host->groups[j]->schemata[t];
                    virBitmapSubtract(default_grp->schemata[t]->masks[i]->mask,
                                      schemata->masks[i]->mask);
                }
            }
        }
    }
}

static virResctrlGroupPtr
virResctrlGetFreeGroup(void)
{
    size_t i;
    virResctrlHostPtr host = NULL;
    virResctrlGroupPtr grp = NULL;

    if (VIR_ALLOC(host) < 0)
        return NULL;

    if (virResctrlLoadHost(host) < 0)
        goto error;

    virResctrlRefreshHost(host);

    for (i = 1; i < host->n_groups; i++)
        virResctrlFreeGroup(host->groups[i]);

    grp = host->groups[0];
    VIR_FREE(host);

    return grp;

 error:
    virResctrlFreeGroup(grp);
    return NULL;
}

virResctrlSchemataPtr
virResctrlGetFreeCache(virResctrlType type)
{
    VIR_DEBUG("type=%d", type);

    virResctrlType t;
    virResctrlGroupPtr grp = NULL;
    virResctrlSchemataPtr schemata = NULL;
    int lockfd = -1;

    lockfd = open(SYSFS_RESCTRL_PATH, O_DIRECTORY);
    if (lockfd < 0)
        return NULL;

    VIR_RESCTRL_LOCK(lockfd, LOCK_SH);

    if ((grp = virResctrlGetFreeGroup()) == NULL)
        goto cleanup;

    for (t = 0; t < VIR_RESCTRL_TYPE_LAST; t++) {
        if (t == type)
            schemata = grp->schemata[t];
        else
            virResctrlFreeSchemata(grp->schemata[t]);

    }

 cleanup:
    VIR_RESCTRL_UNLOCK(lockfd);
    VIR_FORCE_CLOSE(lockfd);
    return schemata;
}

static int
virResctrlCalculateCbm(int cbm_len,
                       virBitmapPtr defaultcbm,
                       virBitmapPtr newcbm)
{
    VIR_DEBUG("cbm_len=%d, defaultcbm=%p, newcbm=%p",
              cbm_len, defaultcbm, newcbm);

    ssize_t pos = -1;
    size_t i;

    /* not enough cache way to be allocated */
    if (virBitmapCountBits(defaultcbm) < cbm_len + 1)
        return -1;

    while ((pos = virBitmapNextSetBit(defaultcbm, pos)) >= 0) {
        for (i = 0; i < cbm_len; i++)
            ignore_value(virBitmapSetBitExpand(newcbm, i + pos));
        /* Test if newcbm is sub set of defaultcbm */
        if (virBitmapNextClearBit(defaultcbm, pos) > i + pos) {
            break;
        } else {
            pos = pos + i - 1;
            virBitmapClearAll(newcbm);
        }
    }

    if (virBitmapCountBits(newcbm) != cbm_len)
        return -1;

    /* consume default cbm after allocation */
    virBitmapSubtract(defaultcbm, newcbm);

    return 0;
}

/* Fill mask value for newly created resource group base on hostcachebank
 * and domcachebank */
static int
virResctrlFillMask(virResctrlGroupPtr grp,
                 virResctrlGroupPtr free_grp,
                 virCapsHostCacheBankPtr hostcachebank,
                 virDomainCacheBankPtr domcachebank)
{
    VIR_DEBUG("grp=%p, free_grp=%p, hostcachebank=%p, domcachebank=%p",
              grp, free_grp, hostcachebank, domcachebank);

    size_t i;
    int cbm_candidate_len;
    unsigned int cache_id;
    unsigned int cache_type;
    virCapsHostCacheControlPtr control = NULL;
    virResctrlMaskPtr mask;
    virResctrlSchemataPtr schemata = NULL;

    /* Find control information for that kind type of cache */
    for (i = 0; i < hostcachebank->ncontrols; i++) {
        if (hostcachebank->controls[i]->scope == domcachebank->type) {
            control = hostcachebank->controls[i];
            break;
        }
    }

    if (control == NULL)
        return -1;

    cache_type = domcachebank->type;
    cache_id = domcachebank->cache_id;
    schemata = grp->schemata[cache_type];

    if ((schemata == NULL) && (VIR_ALLOC(schemata) < 0))
        return -1;

    if (VIR_ALLOC(mask) < 0)
        return -1;

    mask->cache_id = cache_id;
    mask->mask = virBitmapNewEmpty();

    /* here should be control->granularity and control->min
       also domcachebank size should be checked while define domain xml */
    cbm_candidate_len = domcachebank->size / control->min;
    VIR_DEBUG("cbm_len = %d", cbm_candidate_len);
    if (virResctrlCalculateCbm(cbm_candidate_len,
                              free_grp->schemata[cache_type]->masks[cache_id]->mask,
                              mask->mask) < 0)
        goto error;

    schemata->type = cache_type;
    schemata->n_masks += 1;
    schemata->masks[cache_id] = mask;
    grp->schemata[cache_type] = schemata;

    return 0;

 error:
    VIR_FREE(schemata);
    return -1;
}

/* only keep the highest consecutive bits  */
static void
virResctrlTrimMask(virResctrlMaskPtr mask)
{
    size_t i;
    ssize_t setbit = -1;
    ssize_t clearbit = -1;

    clearbit = virBitmapNextClearBit(mask->mask, -1);
    setbit = virBitmapNextSetBit(mask->mask, -1);
    for (i = setbit; i < clearbit; i++)
        ignore_value(virBitmapClearBit(mask->mask, i));
}

static int
virResctrlCompleteMask(virResctrlSchemataPtr schemata,
                       virResctrlSchemataPtr defaultschemata)
{
    size_t i;
    virResctrlMaskPtr mask;

    if (schemata == NULL && VIR_ALLOC(schemata) < 0)
        return -1;

    if (schemata->n_masks == defaultschemata->n_masks)
        return 0;

    for (i = 0; i < defaultschemata->n_masks; i++) {
        if (schemata->masks[i] == NULL) {
            if (VIR_ALLOC(mask) < 0)
                goto error;

            mask->cache_id = i;
            mask->mask = virBitmapNewEmpty();
            schemata->n_masks += 1;
            schemata->masks[i] = mask;
            /* resctrl doesn't allow mask to be zero
               use higher bits to fill up the cbm which
               domaincache bank doens't provide */
            ignore_value(virBitmapSetBitExpand(mask->mask,
                         virBitmapLastSetBit(defaultschemata->masks[i]->mask)));
        }
        /* only keep the highest consecutive bits for default group */
        virResctrlTrimMask(defaultschemata->masks[i]);
    }

    return 0;

 error:
    VIR_FREE(schemata);
    return -1;
}

/* complete the schemata in the resrouce group before it can be write back
   to resctrl */
static int
virResctrlCompleteGroup(virResctrlGroupPtr grp,
                        virResctrlGroupPtr default_grp)
{
    virResctrlType t;
    virResctrlSchemataPtr schemata;
    virResctrlSchemataPtr defaultschemata;


    /* NOTES: resctrl system require we need provide all cache's cbm mask */
    for (t = 0; t < VIR_RESCTRL_TYPE_LAST; t++) {
        defaultschemata = default_grp->schemata[t];
        if (defaultschemata != NULL) {
            schemata = grp->schemata[t];
            if (virResctrlCompleteMask(schemata, defaultschemata) < 0)
                return -1;
            /* only keep the highest consecutive bits for default group */
        }
    }
    return 0;
}

static
char *virResctrlGetSchemataStr(virResctrlSchemataPtr schemata)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    virBufferAsprintf(&buf, "%s:%u=%s",
                      virResctrlTypeToString(schemata->type),
                      schemata->masks[0]->cache_id,
                      virResctrlBitmap2String(schemata->masks[0]->mask));

    for (i = 1; i < schemata->n_masks; i ++)
        virBufferAsprintf(&buf, ";%u=%s",
                          schemata->masks[i]->cache_id,
                          virResctrlBitmap2String(schemata->masks[i]->mask));

    return virBufferContentAndReset(&buf);
}

static int
virResctrlFlushGroup(virResctrlGroupPtr grp)
{
    int ret = -1;
    size_t i;
    char *schemata_str = NULL;
    virResctrlType t;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    VIR_DEBUG("grp=%p", grp);

    if (grp->name != NULL && virResctrlNewSysGroup(grp->name) < 0)
        return -1;

    for (t = 0; t < VIR_RESCTRL_TYPE_LAST; t++) {
        if (grp->schemata[t] != NULL) {
            schemata_str = virResctrlGetSchemataStr(grp->schemata[t]);
            virBufferAsprintf(&buf, "%s\n", schemata_str);
            VIR_FREE(schemata_str);
        }
    }

    schemata_str = virBufferContentAndReset(&buf);

    if (virResctrlWrite(grp->name, "schemata", schemata_str) < 0)
        goto cleanup;

    for (i = 0; i < grp->n_tasks; i++) {
        if (virResctrlWrite(grp->name, "tasks", grp->tasks[i]) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(schemata_str);
    return ret;
}

int virResctrlSetCachetunes(virCapsHostPtr caps,
                            virDomainCachetunePtr cachetune,
                            unsigned char* uuid, pid_t *pids, int npid)
{
    size_t i;
    size_t j;
    int ret = -1;
    char name[VIR_UUID_STRING_BUFLEN];
    char *tmp;
    int lockfd = -1;
    virResctrlGroupPtr grp = NULL;
    virResctrlGroupPtr default_grp = NULL;
    virCapsHostCacheBankPtr hostcachebank;
    virDomainCacheBankPtr domcachebank;

    virUUIDFormat(uuid, name);

    if (cachetune->n_banks < 1)
        return 0;

    /* create new resource group */
    if (VIR_ALLOC(grp) < 0)
        goto error;

    if (VIR_STRDUP(grp->name, name) < 0)
        goto error;

    /* allocate file lock */
    lockfd = open(SYSFS_RESCTRL_PATH, O_DIRECTORY);
    if (lockfd < 0)
        goto error;

    VIR_RESCTRL_LOCK(lockfd, LOCK_EX);

    if ((default_grp = virResctrlGetFreeGroup()) == NULL)
        goto error;

    /* Allocate cache for each cache bank defined in cache tune */
    for (i = 0; i < cachetune->n_banks; i++) {
        domcachebank = &cachetune->cache_banks[i];
        hostcachebank = NULL;
        /* find the host cache bank to be allocated on */
        for (j = 0; j < caps->ncaches; j++) {
            if (caps->caches[j]->id == domcachebank->cache_id) {
                hostcachebank = caps->caches[j];
                break;
            }
        }
        /* fill up newly crated grp and consume from default_grp */
        if (virResctrlFillMask(grp, default_grp, hostcachebank, domcachebank) < 0)
            goto error;
    }

    /* Add tasks to grp */
    for (i = 0; i < npid; i++) {
        if (virAsprintf(&tmp, "%llu", (long long)pids[i]) < 0)
            goto error;

        if (VIR_APPEND_ELEMENT(grp->tasks,
                               grp->n_tasks,
                               tmp) < 0) {
            VIR_FREE(tmp);
            goto error;
        }
    }

    if (virResctrlCompleteGroup(grp, default_grp) < 0) {
        VIR_WARN("Failed to complete group");
        goto error;
    }

    if (virResctrlFlushGroup(grp) < 0)
        goto error;

    if (virResctrlFlushGroup(default_grp) < 0) {
        virResctrlRemoveSysGroup(grp->name);
        goto error;
    }

    ret = 0;

 error:
    VIR_RESCTRL_UNLOCK(lockfd);
    VIR_FORCE_CLOSE(lockfd);
    virResctrlFreeGroup(grp);
    virResctrlFreeGroup(default_grp);
    return ret;
}

int virResctrlRemoveCachetunes(unsigned char* uuid)
{
    int ret = -1;
    int lockfd = -1;
    size_t i;
    virResctrlType t;
    virResctrlSchemataPtr schemata;
    char name[VIR_UUID_STRING_BUFLEN];
    virResctrlGroupPtr default_grp = NULL;

    virUUIDFormat(uuid, name);

    VIR_DEBUG("name=%s", name);

    lockfd = open(SYSFS_RESCTRL_PATH, O_DIRECTORY);
    if (lockfd < 0)
        return -1;

    VIR_RESCTRL_LOCK(lockfd, LOCK_SH);

    if (virResctrlRemoveSysGroup(name) < 0)
        goto cleanup;

    if ((default_grp = virResctrlGetFreeGroup()) == NULL)
        goto cleanup;

    for (t = 0; t < VIR_RESCTRL_TYPE_LAST; t++) {
        schemata = default_grp->schemata[t];
        if (schemata != NULL) {
            for (i = 0; i < schemata->n_masks; i++)
                virResctrlTrimMask(schemata->masks[i]);
        }
    }

    if (virResctrlFlushGroup(default_grp) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_RESCTRL_UNLOCK(lockfd);
    VIR_FORCE_CLOSE(lockfd);
    return ret;
}
