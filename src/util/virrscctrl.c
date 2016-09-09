/*
 * virrscctrl.c: methods for managing control cgroups
 *
 * Copyright (C) 2010-2015 Red Hat, Inc.
 * Copyright IBM Corp. 2008
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
 *  Dan Smith <danms@us.ibm.com>
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

#include "virrscctrl.h"

VIR_LOG_INIT("util.rscctrl");


#define RSC_DIR "/sys/fs/rscctrl"
#define MAX_TASKS_FILE (10*1024*1024)

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
        return rc;
     }

    if ((rc = virDirCreate(path, mode, 0, 0, 0)) < 0) {
        return rc;
    }

    if ((rc = asprintf(&schema_path, "%s/%s", path, "schemas")) < 0) {
        rmdir(path);
        return rc;
     }

    if ((rc = virFileWriteStr(schema_path, schema, 0644)) < 0) {
        rmdir(path);
        return rc;
    }
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
    char *buf;
    char *tmp;

    if( name == NULL) {
        if ((rc = asprintf(&path, "%s/schemas", RSC_DIR)) < 0) {
            return rc;
        }
    }
    else {
        if ((rc = asprintf(&path, "%s/%s/schemas", RSC_DIR, name)) < 0) {
            return rc;
        }
    }
    if (virFileReadAll(path, 100, &buf) < 0) {
        rc = -1;
        goto cleanup;
    }

    VIR_DEBUG("Value is %s", buf);

    if ((tmp = strchr(buf, '\n')))
        *tmp = '\0';

    if (*schemas != NULL) {
        *schemas = buf;
    }

cleanup:
    VIR_FREE(path);
    return rc;
}


/*
Get all Partitions from /sys/fs/rscctrl
*/
int VirRscctrlGetAllPartitions(char **partitions)
{
    int rc = 0;
    struct dirent *ent;
    DIR *dp = NULL;
    int direrr;

    if (virDirOpenQuiet(&dp, RSC_DIR) < 0) {
         if (errno == ENOENT)
             return 0;
         rc = -errno;
         VIR_ERROR(_("Unable to open %s (%d)"), RSC_DIR, errno);
         return rc;
     }

    while ((direrr = virDirRead(dp, &ent, NULL)) > 0) {
        if ((ent->d_type != DT_DIR) || STREQ(ent->d_name, "info"))
            continue;
        printf("%s \n", ent->d_name);
    }

    VIR_DIR_CLOSE(dp);
    if (partitions == NULL){
        ;
    }
    return rc;
}

/* Append the pid to a tasks file of a Partition. This pid will be automaticly
Moved from the old partition */
int VirRscctrlAddtask(const char *p, const char *pid)
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
    printf("%s : %s\n", p, pid);
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
    VIR_FORCE_CLOSE(writefd);
    VIR_FREE(tasks_path);
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
