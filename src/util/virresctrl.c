/*
 * virresctrl.c: methods for managing resource contral
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

#include <sys/ioctl.h>
#if defined HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "virresctrl.h"
#include "viralloc.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "virstring.h"
#include "nodeinfo.h"

VIR_LOG_INIT("util.resctrl");

#define VIR_FROM_THIS VIR_FROM_RESCTRL

#define CONSTRUCT_RESCTRL_PATH(domain_name, item_name) \
do { \
    if (NULL == domain_name) { \
        if (asprintf(&path, "%s/%s", RESCTRL_DIR, item_name) < 0)\
            return -1; \
    } \
    else { \
        if (asprintf(&path, "%s/%s/%s", RESCTRL_DIR, \
                                        domain_name, \
                                        item_name) < 0) \
            return -1;  \
    } \
} while(0)


static virResCtrl ResCtrlAll[] = {
    {
        .name = "L3",
        .domains = NULL,
        .cache_level = 3,
    },
    {
        .name = "L3DATA",
        .domains = NULL,
        .cache_level = 3,
    },
    {
        .name = "L3CODE",
        .domains = NULL,
        .cache_level = 3,
    },
    {
        .name = "L2",
        .domains = NULL,
        .cache_level = 2,
    },
};

static int DebugResCtrlAll(void) {
    int i,j,k;
    virResDomainPtr p;
    for(i = 0; i < RDT_NUM_RESOURCES; i ++) {
        printf("-------------------------------\n");
        printf("%s, %u, %p\n", ResCtrlAll[i].name, ResCtrlAll[i].cache_level, ResCtrlAll[i].domains);
        printf("num_closid %d\n", ResCtrlAll[i].num_closid);
        printf("min_cbm_bits %d\n", ResCtrlAll[i].min_cbm_bits);
        printf("cbm_len %d\n", ResCtrlAll[i].cbm_len);
        printf("num_domains %d\n", ResCtrlAll[i].num_domains);
        printf("num_sockets %d\n", ResCtrlAll[i].num_sockets);
        for(j = 0; j < ResCtrlAll[i].num_sockets; j ++)
            printf("cache_left = %d \n", ResCtrlAll[i].cache_left[j]);

        p = ResCtrlAll[i].domains;
        for(j = 0; j < ResCtrlAll[i].num_domains; j++)
        {
            printf("dom's name is %s\n", p->name);
            printf("dom's tasks is %s\n", p->tasks);
            printf("no_sockets = %d\n", p->n_sockets);
            for(k = 0 ; k < p->n_sockets; k++)
            {
                printf("schematas is %x\n", p->schematas[k].schemata);
            }
            p = p->next;
        }
        printf("+++++++++++++++++++++++++++++++\n");
    }
   return 0;
}


static int virBit_Is_1(int bits)
{
    int ret = 0;
    for(int i = 0; i < MAX_CBM_BIT_LEN; i++) {
        if((bits & 0x1) == 0x1) ret ++;
        if(bits == 0) break;
        bits = bits >> 1;
    }
    return ret;
}

/*
static int virResCtrlAddTask(const char *domain, const char *pid)
{
    int rc = 0;
    char *path;
    int writefd;

    CONSTRUCT_RESCTRL_PATH(domain, "tasks");

    if (!virFileExists(path)) {
        return -1;
    }
    // Append pid to tasks file
    if ((writefd = open(path, O_WRONLY | O_APPEND, S_IRUSR | S_IWUSR)) < 0) {
        rc = -2;
        goto cleanup;
    }
    if (safewrite(writefd, pid, strlen(pid)) < 0) {
        rc = -3;
        goto cleanup;
    }

cleanup:
    VIR_FREE(path);
    VIR_FORCE_CLOSE(writefd);
    return rc;
}

*/

/* Return pointer of  and ncount of schemata*/
static virResSchemataPtr virParseSchemata(const char* schemata, int *ncount)
{
    char type[MAX_SCHEMATA_LEN];
    const char *p, *q;
    int pos;
    int ischemata;
    virResSchemataPtr tmpschemata, pschemata;

    unsigned int socket_no = 0;
    p = q = schemata;
    pos = strchr(schemata, ':') - p;

    if(virStrncpy(type, schemata, pos, strlen(schemata)) == NULL) {
        return NULL;
    }

    *ncount = 1;
    while((q = strchr(p, ';')) != 0) {
        p = q + 1;
        (*ncount) ++;
    }

    // allocat an arrry to keep schemata
    if(VIR_ALLOC_N_QUIET(tmpschemata, *ncount) < 0) {
        return NULL;
    }

    pschemata = tmpschemata;

    p = q = schemata + pos + 1;

    char *tmp;

    while(*p != '\0'){
        if (*p == '='){
            q = p + 1;

            if (VIR_STRDUP(tmpschemata->name, type) < 0)
                goto cleanup;

            tmpschemata->socket_no = socket_no++;

            while(*p != ';' && *p != '\0') p++;

            if (VIR_STRNDUP(tmp, q, p-q) < 0)
                goto cleanup;

            if (virStrToLong_i(tmp, NULL, 16, &ischemata) < 0)
                goto cleanup;

            VIR_FREE(tmp);
            tmpschemata->schemata = ischemata;
            tmpschemata ++;
        }
        p++;
    }

    return pschemata;

cleanup:
    // fix me
    VIR_FREE(pschemata);
    return NULL;

}

static int virResCtrlGetStr(const char *domain_name, const char *item_name, char **ret)
{
    char *path;
    int rc = 0;

    CONSTRUCT_RESCTRL_PATH(domain_name, item_name);

    if (virFileReadAll(path, MAX_FILE_LEN, ret) < 0) {
        rc = -1;
        goto cleanup;
    }

cleanup:
    VIR_FREE(path);
    return rc;
}

static int virResCtrlGetTasks(const char *domain_name, char **pids)
{
    return virResCtrlGetStr(domain_name, "tasks", pids);
}

static int virResCtrlGetSchemata(const int type, const char *name, char **schemata)
{
    int rc;
    char *tmp, *end;
    char *buf;

    if ((rc = virResCtrlGetStr(name, "schemata", &buf)) < 0)
        return rc;

    tmp = strstr(buf, ResCtrlAll[type].name);
    end = strchr(tmp, '\n');
    *end = '\0';
    if(VIR_STRDUP(*schemata, tmp) < 0)
        rc = -1;

    VIR_FREE(buf);
    return rc;
}

static int virResCtrlGetInfoStr(const int type, const char *item, char **str)
{
    int ret = 0;
    char *tmp;
    char *path;

    if (asprintf(&path, "%s/%s/%s", RESCTRL_INFO_DIR, ResCtrlAll[type].name, item) < 0)
        return -1;
    if (virFileReadAll(path, 10, str) < 0) {
        ret = -1;
        goto cleanup;
    }

    if ((tmp = strchr(*str, '\n'))) {
        *tmp = '\0';
    }

cleanup:
    VIR_FREE(path);
    return ret;
}

static virResDomainPtr virResCtrlGetAllDomains(int type, int *len)
{
    struct dirent *ent;
    DIR *dp = NULL;
    int direrr;
    char *schematas, *tasks;

    *len = 0;
    virResDomainPtr header, tmp, tmp_pre;
    header = tmp = tmp_pre = NULL;
    if (virDirOpenQuiet(&dp, RESCTRL_DIR) < 0) {
        if (errno == ENOENT)
            return NULL;
        VIR_ERROR(_("Unable to open %s (%d)"), RESCTRL_DIR, errno);
        goto cleanup;
    }

    //read default partitions
    if(VIR_ALLOC(header) < 0)
        return NULL;
    header->pre = NULL;
    header->next = NULL;
    if(VIR_STRDUP(header->name, "default") < 0)
        goto cleanup;

    if (virResCtrlGetSchemata(type, NULL, &schematas) < 0) {
        goto cleanup;
    }
    header->schematas = virParseSchemata(schematas, &(header->n_sockets));
    VIR_FREE(schematas);
    *len = 1;

    while ((direrr = virDirRead(dp, &ent, NULL)) > 0) {
        if ((ent->d_type != DT_DIR) || STREQ(ent->d_name, "info"))
            continue;

        if(virResCtrlGetTasks(ent->d_name, &tasks) < 0)
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
        if (virResCtrlGetSchemata(type, tmp->name, &schematas) < 0) {
            goto cleanup;
        }

        (*len) ++;
        tmp->schematas = virParseSchemata(schematas, &(tmp->n_sockets));
        tmp->tasks = tasks;
        VIR_FREE(schematas);
    }
    return header;

cleanup:

    VIR_DIR_CLOSE(dp);
    return NULL;
}

static int virResCtrlGetCache(int type)
{
    virNodeInfo nodeinfo;
    if (nodeGetInfo(&nodeinfo) < 0)
        return 0;

    if (type == RDT_RESOURCE_L3)
        return nodeinfo.l3_cache;
    if (type == RDT_RESOURCE_L3DATA || type == RDT_RESOURCE_L3CODE)
        return nodeinfo.l3_cache / 2;

    return 0;
}

static int virResCtrlGetConfig(int type)
{
    int ret = 0;
    int i;
    char *str;

    if ((ret = virResCtrlGetInfoStr(type, "num_closids", &str)) < 0) {
        return ret;
    }
    if (virStrToLong_i(str, NULL, 10, &ResCtrlAll[type].num_closid) < 0) {
        return -1;
    }
    VIR_FREE(str);
    if ((ret = virResCtrlGetInfoStr(type, "min_cbm_bits", &str)) < 0) {
        return ret;
    }
    if (virStrToLong_i(str, NULL, 10, &ResCtrlAll[type].min_cbm_bits) < 0) {
        return -1;
    }
    VIR_FREE(str);

    if ((ret = virResCtrlGetInfoStr(type, "cbm_mask", &str)) < 0) {
        return ret;
    }
    ResCtrlAll[type].cbm_len = strlen(str) * 4;
    VIR_FREE(str);

    ResCtrlAll[type].domains = virResCtrlGetAllDomains(type, &ResCtrlAll[type].num_domains);

    ResCtrlAll[type].num_sockets = ResCtrlAll[type].domains->n_sockets;

    for(i = 0; i < ResCtrlAll[type].num_sockets; i++) {
        ResCtrlAll[type].cache_left[i] = virResCtrlGetCache(type);
    }

    ResCtrlAll[type].enabled = true;

    //printf("add tasks = %d\n", virResCtrlAddTask("p", "88879"));

    printf("%d\n", virBit_Is_1(1));
    return ret;
}

int virResCtrlInit(void) {
    int i = 0;
    char *tmp;
    int rc = 0;

    for(i = 0; i <RDT_NUM_RESOURCES; i ++) {
        if ((rc = asprintf(&tmp, "%s/%s", RESCTRL_INFO_DIR, ResCtrlAll[i].name)) < 0) {
            VIR_DEBUG("bad");
            continue;
        }
        if (virFileExists(tmp)) {
            if ((rc = virResCtrlGetConfig(i)) < 0 )
                VIR_DEBUG("error while get config for %d", i);
        }

        VIR_FREE(tmp);
    }
    return rc;
}

bool virResCtrlAvailable(void) {

    virResCtrlInit();

    DebugResCtrlAll();

    if (!virFileExists(RESCTRL_INFO_DIR))
        return false;
    return true;
}

virResCtrlPtr virGetResCtrl(int type) {
    return &ResCtrlAll[type];
}

int virResctrlCacheGetStats(virNodeCacheStatsPtr params,
                            int *nparams,
                            unsigned int flags)
{
    virCheckFlags(0, -1);
    int i, j, k;
    char resource_header [10];

    if(*nparams == 0) {
        for ( i = 0; i < RDT_NUM_RESOURCES; i ++) {
            if( ResCtrlAll[i].enabled ) {
                *nparams += ResCtrlAll[i].num_sockets;
            }
        }
    }
    if ( params == NULL )
        return 0;

    k = 0;

    for ( i = 0; i < RDT_NUM_RESOURCES; i ++) {
        if( ResCtrlAll[i].enabled ) {
            for ( j = 0; j < ResCtrlAll[i].num_sockets; j ++)
            {
                snprintf(resource_header, sizeof(resource_header), "%s.%d ", ResCtrlAll[i].name, j);
                if (virStrcpyStatic((&params[k])->field, resource_header) == NULL) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Field '%s' too long for destination"),
                           resource_header);
                    return -1;
                }
                (&params[k++])->value = ResCtrlAll[i].cache_left[j];
            }
        }
    }
    return 0;
}
