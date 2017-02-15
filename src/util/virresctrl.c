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
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

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

#define VIR_RESCTRL_LOCK(fd, op) flock(fd, op)
#define VIR_RESCTRL_UNLOCK(fd) flock(fd, LOCK_UN)

/**
 * a virResSchemata represents a schemata object under a resource control
 * domain.
 */
typedef struct _virResSchemataItem virResSchemataItem;
typedef virResSchemataItem *virResSchemataItemPtr;
struct _virResSchemataItem {
    unsigned int socket_no;
    unsigned schemata;
};

typedef struct _virResSchemata virResSchemata;
typedef virResSchemata *virResSchemataPtr;
struct _virResSchemata {
    unsigned int n_schemata_items;
    virResSchemataItemPtr schemata_items;
};

/**
 * a virResDomain represents a resource control domain. It's a double linked
 * list.
 */

typedef struct _virResDomain virResDomain;
typedef virResDomain *virResDomainPtr;

struct _virResDomain {
    char *name;
    virResSchemataPtr schematas[VIR_RDT_RESOURCE_LAST];
    char **tasks;
    size_t n_tasks;
    size_t n_sockets;
    virResDomainPtr pre;
    virResDomainPtr next;
};

/* All resource control domains on this host*/
typedef struct _virResCtrlDomain virResCtrlDomain;
typedef virResCtrlDomain *virResCtrlDomainPtr;

struct _virResCtrlDomain {
    unsigned int num_domains;
    virResDomainPtr domains;

    virMutex lock;
};

static unsigned int host_id;

/* Global static struct to be maintained which is a interface */
static virResCtrlDomain domainall;

/* Global static struct array to be maintained which indicate
 * resource status on a host */
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

/*
 * How many bits is set in schemata
 * eg:
 * virResCtrlBitsNum(10110) = 2 */
static int virResCtrlBitsContinuesNum(unsigned schemata)
{
    size_t i;
    int ret = 0;
    for (i = 0; i < MAX_CBM_BIT_LEN; i ++) {
        if ((schemata & 0x1) == 0x1)
            ret++;
        else
            if (ret > 0 || schemata == 0) break;

        schemata = schemata >> 1;
    }
    return ret;
}

/* Position of the highest continue 1 bit of in schemata
 * eg:
 * virResctrlBitsContinuesPos(10110) = 3 */
static int virResCtrlBitsContinuesPos(unsigned schemata)
{
    size_t i;
    int flag = 0;
    for (i = 0; i < MAX_CBM_BIT_LEN; i ++) {
        if ((schemata & 0x1) == 0x0 && flag == 1)
            return i;
        else if ((schemata & 0x1) == 0x1) flag = 1;

        schemata = schemata >> 1;
    }
    return 0;
}

static int virResCtrlGetStr(const char *domain_name, const char *item_name, char **ret)
{
    char *path;
    int rc = 0;

    CONSTRUCT_RESCTRL_PATH(domain_name, item_name);

    if (!virFileExists(path))
        goto cleanup;

    if (virFileReadAll(path, MAX_FILE_LEN, ret) < 0) {
        rc = -1;
        goto cleanup;
    }

    rc = 0;

 cleanup:
    VIR_FREE(path);
    return rc;
}

static int virResCtrlGetSchemata(const int type, const char *name, char **schemata)
{
    int rc;
    char *tmp, *end;
    char *buf;

    if ((rc = virResCtrlGetStr(name, "schemata", &buf)) < 0)
        return rc;

    tmp = strstr(buf, resctrlall[type].name);
    end = strchr(tmp, '\n');
    *end = '\0';
    if (VIR_STRDUP(*schemata, tmp) < 0)
        rc = -1;

    VIR_FREE(buf);
    return rc;
}

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

/* Return pointer of and ncount of schemata*/
static virResSchemataPtr virParseSchemata(const char *schemata_str, size_t *ncount)
{
    const char *p, *q;
    int pos;
    int ischemata;
    virResSchemataPtr schemata;
    virResSchemataItemPtr schemataitems, tmpitem;
    unsigned int socket_no = 0;
    char *tmp;

    if (VIR_ALLOC(schemata) < 0)
        goto cleanup;

    p = q = schemata_str;
    pos = strchr(schemata_str, ':') - p;

    /* calculate cpu socket count */
    *ncount = 1;
    while ((q = strchr(p, ';')) != 0) {
        p = q + 1;
        (*ncount)++;
    }

    /* allocat an arrry to store schemata for each socket*/
    if (VIR_ALLOC_N_QUIET(tmpitem, *ncount) < 0)
        goto cleanup;

    schemataitems = tmpitem;

    p = q = schemata_str + pos + 1;

    while (*p != '\0') {
        if (*p == '=') {
            q = p + 1;

            tmpitem->socket_no = socket_no++;

            while (*p != ';' && *p != '\0') p++;

            if (VIR_STRNDUP(tmp, q, p-q) < 0)
                goto cleanup;

            if (virStrToLong_i(tmp, NULL, 16, &ischemata) < 0)
                goto cleanup;

            VIR_FREE(tmp);
            tmp = NULL;
            tmpitem->schemata = ischemata;
            tmpitem ++;
            schemata->n_schemata_items += 1;
        }
        p++;
    }

    schemata->schemata_items = schemataitems;
    return schemata;

 cleanup:
    VIR_FREE(schemata);
    VIR_FREE(tmpitem);
    return NULL;
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

/* Remove the Domain from sysfs, this should only success no pids in tasks
 * of a partition.
 */
static
int virResCtrlRemoveDomain(const char *name)
{
    char *path = NULL;
    int rc = 0;

    if ((rc = virAsprintf(&path, "%s/%s", RESCTRL_DIR, name)) < 0)
        return rc;
    rc = rmdir(path);
    VIR_FREE(path);
    return rc;
}

static
int virResCtrlDestroyDomain(virResDomainPtr p)
{
    size_t i;
    int rc;
    if ((rc = virResCtrlRemoveDomain(p->name)) < 0)
        VIR_WARN("Failed to removed partition %s", p->name);

    VIR_FREE(p->name);
    p->name = NULL;
    for (i = 0; i < p->n_tasks; i ++)
        VIR_FREE(p->tasks[i]);
    VIR_FREE(p);
    p = NULL;
    return rc;
}


/* assemble schemata string*/
static
char* virResCtrlAssembleSchemata(virResSchemataPtr schemata, int type)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    virBufferAsprintf(&buf, "%s:%u=%x", resctrlall[type].name,
                      schemata->schemata_items[0].socket_no,
                      schemata->schemata_items[0].schemata);

    for (i = 1; i < schemata->n_schemata_items; i++) {
        virBufferAsprintf(&buf, ";%u=%x",
                       schemata->schemata_items[i].socket_no,
                       schemata->schemata_items[i].schemata);
    }

    return virBufferContentAndReset(&buf);
}

/* Refresh default domains' schemata
 */
static
int virResCtrlRefreshSchemata(void)
{
    size_t i, j, k;
    unsigned int tmp_schemata;
    unsigned int default_schemata;
    unsigned int min_schemata;
    int pair_type = 0;

    virResDomainPtr header, p;

    header = domainall.domains;

    if (!header)
        return 0;

    for (i = 0; i < VIR_RDT_RESOURCE_LAST; i++) {
        if (VIR_RESCTRL_ENABLED(i)) {
            min_schemata = VIR_RESCTRL_GET_SCHEMATA(resctrlall[i].min_cbm_bits);

            if (i == VIR_RDT_RESOURCE_L3DATA)
                pair_type = VIR_RDT_RESOURCE_L3CODE;
            if (i == VIR_RDT_RESOURCE_L3CODE)
                pair_type = VIR_RDT_RESOURCE_L3DATA;

            for (j = 0; j < header->schematas[i]->n_schemata_items; j ++) {
                p = header->next;
                // Reset to default schemata 0xfffff
                default_schemata = VIR_RESCTRL_GET_SCHEMATA(resctrlall[i].cbm_len);
                tmp_schemata = 0;
                /* NOTEs: if only header domain, the schemata will be set to default one*/
                for (k = 1; k < domainall.num_domains; k++) {
                    if (p->schematas[i]->schemata_items[j].schemata > min_schemata) {
                        tmp_schemata |= p->schematas[i]->schemata_items[j].schemata;
                        if (pair_type > 0)
                            tmp_schemata |= p->schematas[pair_type]->schemata_items[j].schemata;
                    }
                    p = p->next;
                }

                default_schemata ^= tmp_schemata;

                default_schemata &= VIR_RESCTRL_GET_SCHEMATA(virResCtrlBitsContinuesPos(default_schemata));
                header->schematas[i]->schemata_items[j].schemata = default_schemata;
                int bitsnum = virResCtrlBitsContinuesNum(default_schemata);
                resctrlall[i].cache_banks[j].cache_left =
                    (bitsnum - resctrlall[i].min_cbm_bits) * resctrlall[i].cache_banks[j].cache_min;
            }
        }
    }

    return 0;

}

/* Get a domain ptr by domain's name*/
static
virResDomainPtr virResCtrlGetDomain(const char* name) {
    size_t i;
    virResDomainPtr p = domainall.domains;
    for (i = 0; i < domainall.num_domains; i++) {
        if ((p->name) && STREQ(name, p->name))
            return p;
        p = p->next;
    }
    return NULL;
}

static int
virResCtrlAddTask(virResDomainPtr dom, pid_t pid)
{
    size_t maxtasks;

    if (VIR_RESIZE_N(dom->tasks, maxtasks, dom->n_tasks + 1, 1) < 0)
        return -1;

    if (virAsprintf(&(dom->tasks[dom->n_tasks]), "%llu", (long long)pid) < 0)
        return -1;

    dom->n_tasks += 1;
    return 0;
}

static int
virResCtrlWrite(const char *name, const char *item, const char *content)
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

/* if name == NULL we load default schemata */
static
virResDomainPtr virResCtrlLoadDomain(const char *name)
{
    char *schematas;
    virResDomainPtr p;
    size_t i;

    if (VIR_ALLOC(p) < 0)
        goto cleanup;

    for (i = 0; i < VIR_RDT_RESOURCE_LAST; i++) {
        if (VIR_RESCTRL_ENABLED(i)) {
            if (virResCtrlGetSchemata(i, name, &schematas) < 0)
                goto cleanup;
            p->schematas[i] = virParseSchemata(schematas, &(p->n_sockets));
            VIR_FREE(schematas);
        }
    }

    p->tasks = NULL;
    p->n_tasks = 0;

    if ((name != NULL) && (VIR_STRDUP(p->name, name)) < 0)
        goto cleanup;

    return p;

 cleanup:
    VIR_FREE(p);
    return NULL;
}

static
virResDomainPtr virResCtrlCreateDomain(const char *name)
{
    char *path;
    mode_t mode = 0755;
    virResDomainPtr p;
    size_t i, j;

    if (virAsprintf(&path, "%s/%s", RESCTRL_DIR, name) < 0)
        return NULL;

    if (virDirCreate(path, mode, 0, 0, 0) < 0)
        goto cleanup;

    if ((p = virResCtrlLoadDomain(name)) == NULL)
        return p;

    /* sys fs doens't let us use 0.
     * reset schemata to min_bits*/
    for (i = 0; i < VIR_RDT_RESOURCE_LAST; i++) {
        if (VIR_RESCTRL_ENABLED(i)) {
            int min_bits =  VIR_RESCTRL_GET_SCHEMATA(resctrlall[i].min_cbm_bits);
            for (j = 0; j < p->n_sockets; j++)
                p->schematas[i]->schemata_items[j].schemata = min_bits;
        }
    }

    VIR_FREE(path);
    return p;

 cleanup:
    VIR_FREE(path);
    return NULL;
}

/* flush domains's information to sysfs*/
static int
virResCtrlFlushDomainToSysfs(virResDomainPtr dom)
{
    size_t i;
    char* schemata;
    char* tmp;
    int rc = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    for (i = 0; i < VIR_RDT_RESOURCE_LAST; i++) {
        if (VIR_RESCTRL_ENABLED(i)) {
            tmp = virResCtrlAssembleSchemata(dom->schematas[i], i);
            virBufferAsprintf(&buf, "%s\n", tmp);
            VIR_FREE(tmp);
        }
    }

    schemata = virBufferContentAndReset(&buf);

    if (virResCtrlWrite(dom->name, "schemata", schemata) < 0)
        goto cleanup;

    if (dom->n_tasks > 0) {
        for (i = 0; i < dom->n_tasks; i++) {
            if (virResCtrlWrite(dom->name, "tasks", dom->tasks[i]) < 0)
                goto cleanup;
        }
    }

    rc = 0;

 cleanup:
    VIR_FREE(schemata);
    return rc;
}

static virResDomainPtr virResCtrlGetAllDomains(unsigned int *len)
{
    struct dirent *ent;
    DIR *dp = NULL;
    int direrr;

    *len = 0;
    virResDomainPtr header, tmp, tmp_pre;
    header = tmp = tmp_pre = NULL;
    if (virDirOpenQuiet(&dp, RESCTRL_DIR) < 0) {
        if (errno == ENOENT)
            return NULL;
        VIR_ERROR(_("Unable to open %s (%d)"), RESCTRL_DIR, errno);
        goto cleanup;
    }

    header = virResCtrlLoadDomain(NULL);
    if (header == NULL)
        goto cleanup;

    header->next = NULL;

    *len = 1;

    while ((direrr = virDirRead(dp, &ent, NULL)) > 0) {
        if ((ent->d_type != DT_DIR) || STREQ(ent->d_name, "info"))
            continue;

        tmp = virResCtrlLoadDomain(ent->d_name);
        if (tmp == NULL)
            goto cleanup;

        tmp->next = NULL;

        if (header->next == NULL)
            header->next = tmp;

        if (tmp_pre == NULL) {
            tmp->pre = header;
        } else {
            tmp->pre = tmp_pre;
            tmp_pre->next = tmp;
        }

        tmp_pre = tmp;
        (*len) ++;
    }
    return header;

 cleanup:
    VIR_DIR_CLOSE(dp);
    tmp_pre = tmp = header;
    while (tmp) {
        tmp_pre = tmp;
        tmp = tmp->next;
        VIR_FREE(tmp_pre);
    }
    return NULL;
}

static int
virResCtrlAppendDomain(virResDomainPtr dom)
{
    virResDomainPtr p = domainall.domains;

    virMutexLock(&domainall.lock);

    while (p->next != NULL) p = p->next;
    p->next = dom;
    dom->pre = p;
    domainall.num_domains += 1;

    virMutexUnlock(&domainall.lock);
    return 0;
}

/* scan /sys/fs/resctrl again and refresh default schemata */
static
int virResCtrlScan(void)
{
    struct dirent *ent;
    DIR *dp = NULL;
    int direrr;
    virResDomainPtr p;
    int rc = -1;

    if (virDirOpenQuiet(&dp, RESCTRL_DIR) < 0) {
        if (errno == ENOENT)
            return -1;
        VIR_ERROR(_("Unable to open %s (%d)"), RESCTRL_DIR, errno);
        goto cleanup;
    }

    while ((direrr = virDirRead(dp, &ent, NULL)) > 0) {
        if ((ent->d_type != DT_DIR) || STREQ(ent->d_name, "info"))
            continue;
        /* test if we'v tracked all domains */
        p = virResCtrlGetDomain(ent->d_name);
        if (p == NULL) {
            p = virResCtrlLoadDomain(ent->d_name);
            if (p == NULL)
                continue;
            virResCtrlAppendDomain(p);
        }
    }
    rc = 0;

 cleanup:
    VIR_DIR_CLOSE(dp);
    return rc;
}

static int
virResCtrlGetSocketIdByHostID(int type, unsigned int hostid)
{
    size_t i;
    for (i = 0; i < resctrlall[type].num_banks; i++) {
        if (resctrlall[type].cache_banks[i].host_id == hostid)
            return i;
    }
    return -1;
}

static int
virResCtrlCalculateSchemata(int type,
                            int sid,
                            unsigned hostid,
                            unsigned long long size)
{
    size_t i;
    int count;
    int rc = -1;
    virResDomainPtr p;
    unsigned int tmp_schemata;
    unsigned int schemata_sum = 0;
    int pair_type = 0;

    virMutexLock(&resctrlall[type].cache_banks[sid].lock);

    if (resctrlall[type].cache_banks[sid].cache_left < size) {
        VIR_ERROR(_("Not enough cache left on bank %u"), hostid);
        goto cleanup;
    }

    if ((count = size / resctrlall[type].cache_banks[sid].cache_min) <= 0) {
        VIR_ERROR(_("Error cache size %llu"), size);
        goto cleanup;
    }

    tmp_schemata = VIR_RESCTRL_GET_SCHEMATA(count);

    p = domainall.domains;
    p = p->next;

    /* for type is l3code and l3data, we need to deal them specially*/
    if (type == VIR_RDT_RESOURCE_L3DATA)
        pair_type = VIR_RDT_RESOURCE_L3CODE;

    if (type == VIR_RDT_RESOURCE_L3CODE)
        pair_type = VIR_RDT_RESOURCE_L3DATA;

    for (i = 1; i < domainall.num_domains; i++) {
        schemata_sum |= p->schematas[type]->schemata_items[sid].schemata;
        if (pair_type > 0)
            schemata_sum |= p->schematas[pair_type]->schemata_items[sid].schemata;
        p = p->next;
    }

    tmp_schemata = tmp_schemata << (resctrlall[type].cbm_len - count);

    while ((tmp_schemata & schemata_sum) != 0)
        tmp_schemata = tmp_schemata >> 1;

    resctrlall[type].cache_banks[sid].cache_left -= size;
    if (pair_type > 0)
        resctrlall[pair_type].cache_banks[sid].cache_left = resctrlall[type].cache_banks[sid].cache_left;

    rc = tmp_schemata;

 cleanup:
    virMutexUnlock(&resctrlall[type].cache_banks[sid].lock);
    return rc;
}

int virResCtrlSetCacheBanks(virDomainCachetunePtr cachetune,
                            unsigned char *uuid, pid_t *pids, int npid)
{
    size_t i;
    char name[VIR_UUID_STRING_BUFLEN];
    virResDomainPtr p;
    int type;
    int pair_type = -1;
    int sid;
    int schemata;
    int lockfd;
    int rc = -1;

    for (i = 0; i < cachetune->n_banks; i++) {
        VIR_DEBUG("cache_banks %u, %u, %llu, %s",
                 cachetune->cache_banks[i].id,
                 cachetune->cache_banks[i].host_id,
                 cachetune->cache_banks[i].size,
                 cachetune->cache_banks[i].type);
    }

    if (cachetune->n_banks < 1)
        return 0;

    virUUIDFormat(uuid, name);

    if ((lockfd = open(RESCTRL_DIR, O_RDONLY)) < 0)
        goto cleanup;

    if (VIR_RESCTRL_LOCK(lockfd, LOCK_EX) < 0) {
        virReportSystemError(errno, _("Unable to lock '%s'"), RESCTRL_DIR);
        goto cleanup;
    }

    if (virResCtrlScan() < 0) {
        VIR_ERROR(_("Failed to scan resctrl domain dir"));
        goto cleanup;
    }
    p = virResCtrlGetDomain(name);
    if (p == NULL) {
        VIR_DEBUG("no domain name %s found, create new one!", name);
        p = virResCtrlCreateDomain(name);
    }
    if (p != NULL) {

        virResCtrlAppendDomain(p);

        for (i = 0; i < cachetune->n_banks; i++) {
            if ((type = virResCtrlTypeFromString(
                            cachetune->cache_banks[i].type)) < 0) {
                VIR_WARN("Ignore unknown cache type %s.",
                         cachetune->cache_banks[i].type);
                continue;
            }
            /* use cdp compatible mode */
            if (!VIR_RESCTRL_ENABLED(type) &&
                    (type == VIR_RDT_RESOURCE_L3) &&
                    VIR_RESCTRL_ENABLED(VIR_RDT_RESOURCE_L3DATA)) {
                type = VIR_RDT_RESOURCE_L3DATA;
                pair_type = VIR_RDT_RESOURCE_L3CODE;
            }

            if ((sid = virResCtrlGetSocketIdByHostID(
                            type, cachetune->cache_banks[i].host_id)) < 0) {
                VIR_WARN("Can not find cache bank host id %u.",
                         cachetune->cache_banks[i].host_id);
                continue;
            }

            if ((schemata = virResCtrlCalculateSchemata(
                            type, sid, cachetune->cache_banks[i].host_id,
                            cachetune->cache_banks[i].size)) < 0) {
                VIR_WARN("Failed to set schemata for cache bank id %u",
                         cachetune->cache_banks[i].id);
                continue;
            }

            p->schematas[type]->schemata_items[sid].schemata = schemata;
            if (pair_type > 0)
                p->schematas[pair_type]->schemata_items[sid].schemata = schemata;
        }

        for (i = 0; i < npid; i++)
            virResCtrlAddTask(p, pids[i]);

        if (virResCtrlFlushDomainToSysfs(p) < 0) {
            VIR_ERROR(_("failed to flush domain %s to sysfs"), name);
            virResCtrlDestroyDomain(p);
            goto cleanup;
        }
    } else {
        VIR_ERROR(_("Failed to create a domain in sysfs"));
        goto cleanup;
    }

    virResCtrlRefreshSchemata();
    /* after refresh, flush header's schemata changes to sys fs */
    if (virResCtrlFlushDomainToSysfs(domainall.domains) < 0) {
        VIR_ERROR(_("failed to flush domain to sysfs"));
        goto cleanup;
    }

    rc = 0;

 cleanup:
    VIR_RESCTRL_UNLOCK(lockfd);
    return rc;
}

/* Should be called after pid disappeared, we recalculate
 * schemata of default and flush it to sys fs.
 */
int virResCtrlUpdate(unsigned char *uuid)
{
    char name[VIR_UUID_STRING_BUFLEN];
    virResDomainPtr del;

    virUUIDFormat(uuid, name);

    del = virResCtrlGetDomain(name);

    if (del != NULL) {

        virMutexLock(&domainall.lock);

        del->pre->next = del->next;
        if (del->next != NULL)
            del->next->pre = del->pre;
        virResCtrlDestroyDomain(del);
        domainall.num_domains -= 1;
        virResCtrlRefreshSchemata();
        if (virResCtrlFlushDomainToSysfs(domainall.domains) < 0)
            VIR_WARN("failed to flush domain to sysfs");
        virMutexUnlock(&domainall.lock);
    }
    return 0;
}

int
virResCtrlInit(void)
{
    size_t i, j;
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

    domainall.domains = virResCtrlGetAllDomains(&(domainall.num_domains));

    for (i = 0; i < VIR_RDT_RESOURCE_LAST; i++) {
        if (VIR_RESCTRL_ENABLED(i)) {
            for (j = 0; j < resctrlall[i].num_banks; j++) {
                if (virMutexInit(&resctrlall[i].cache_banks[j].lock) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Unable to initialize mutex"));
                    return -1;
                }
            }
        }
    }

    if (virMutexInit(&domainall.lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Unable to initialize mutex"));
        return -1;
    }

    if ((rc = virResCtrlRefreshSchemata()) < 0)
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                _("Failed to refresh resource control"));

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
