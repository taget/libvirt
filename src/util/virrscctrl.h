/*
 * virrscctrl.h: methods for managing rscctrl
 *
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
 * Authors:
 *  Eli Qiao <liyong.qiao@intel.com>
 */

#ifndef __VIR_RSCCTRL_H__
# define __VIR_RSCCTRL_H__

# include "virutil.h"
# include "virbitmap.h"

bool virRscctrlAvailable(void);

int virRscctrlGetUnsignd(const char *, unsigned int *);
int virRscctrlGetMaxL3Cbmlen(unsigned int *);
int virRscctrlGetMaxclosId(unsigned int *);

int VirRscctrlAddNewPartition(const char *, const char *);
int VirRscctrlRemovePartition(const char *);

int VirRscctrlGetSchemas(const char *, char **);
int VirRscctrlAddtask(const char *, const char *);
int VirRscctrlGetTasks(const char *, char **);
int VirRscctrlGetAllPartitions(char **);
#endif
