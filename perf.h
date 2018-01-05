/*
 * Copyright (C) Colin Ian King 2015-2018
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#ifndef __PERF_H__
#define __PERF_H__

#define _GNU_SOURCE

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#if defined(__linux__) && defined(__NR_perf_event_open)
#define PERF_ENABLED
#endif

enum {
	PERF_TP_PAGE_FAULT_USER = 0,
	PERF_TP_PAGE_FAULT_KERNEL,
	PERF_TP_MM_PAGE_ALLOC,
	PERF_TP_MM_PAGE_FREE,
	PERF_MAX
};

/* per perf counter info */
typedef struct {
	uint64_t counter;               /* perf counter */
	bool	 valid;			/* is it valid */
	int      fd;                    /* perf per counter fd */
} perf_stat_t;

typedef struct {
	perf_stat_t perf_stat[PERF_MAX];/* perf counters */
	int perf_opened;		/* count of opened counters */
} perf_t;

/* used for table of perf events to gather */
typedef struct {
	int id;				/* stress-ng perf ID */
	unsigned long type;		/* perf types */
	unsigned long config;		/* perf type specific config */
} perf_info_t;

/* perf trace point id -> path resolution */
typedef struct {
	int id;				/* stress-ng perf ID */
	char *path;			/* path to config value */
} perf_tp_info_t;

/* perf data */
typedef struct {
	uint64_t counter;		/* perf counter */
	uint64_t time_enabled;		/* perf time enabled */
	uint64_t time_running;		/* perf time running */
} perf_data_t;

static inline void perf_init(perf_t *p)
{
        memset(p, 0, sizeof(perf_t));
}

extern int perf_start(perf_t *p, const pid_t pid);
extern int perf_stop(perf_t *p);
extern uint64_t perf_counter(const perf_t *p, const int id);

#endif
