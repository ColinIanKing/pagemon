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
#define _GNU_SOURCE

#include "perf.h"

#if defined(PERF_ENABLED)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <linux/perf_event.h>

#define UNRESOLVED	(~0UL)

static perf_tp_info_t perf_tp_info[] = {
	{ PERF_TP_PAGE_FAULT_USER,	"exceptions/page_fault_user" },
	{ PERF_TP_PAGE_FAULT_KERNEL,	"exceptions/page_fault_kernel" },
	{ PERF_TP_MM_PAGE_ALLOC,	"kmem/mm_page_alloc" },
	{ PERF_TP_MM_PAGE_FREE,		"kmem/mm_page_free" },

};

static inline unsigned long
perf_type_tracepoint_resolve_config(const char *path)
{
	char perf_path[PATH_MAX];
	unsigned long config;
	FILE *fp;

	(void)snprintf(perf_path, sizeof(perf_path),
		"/sys/kernel/debug/tracing/events/%s/id", path);
	if ((fp = fopen(perf_path, "r")) == NULL)
		return UNRESOLVED;
	if (fscanf(fp, "%lu", &config) != 1) {
		fclose(fp);
		return UNRESOLVED;
	}
	(void)fclose(fp);

	return config;
}

int perf_start(perf_t *p, const pid_t pid)
{
	int i;

	if (pid <= 0)
		return 0;
	p->perf_opened = 0;
	for (i = 0; i < PERF_MAX; i++) {
		struct perf_event_attr attr;

		(void)memset(&attr, 0, sizeof(attr));
		attr.type = PERF_TYPE_TRACEPOINT;
		attr.config = perf_type_tracepoint_resolve_config(perf_tp_info[i].path);
		attr.disabled = 1;
		attr.inherit = 1;
		attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
				   PERF_FORMAT_TOTAL_TIME_RUNNING;
		attr.size = sizeof(attr);
		p->perf_stat[i].fd = syscall(__NR_perf_event_open, &attr, pid, -1, -1, 0);
		if (p->perf_stat[i].fd > -1)
			p->perf_opened++;
		else
			return -1;
	}
	if (!p->perf_opened)
		return -1;

	for (i = 0; i < PERF_MAX; i++) {
		int fd = p->perf_stat[i].fd;

		if (fd > -1) {
			if (ioctl(fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) < 0) {
				(void)close(fd);
				p->perf_stat[i].fd = -1;
				continue;
			}
			if (ioctl(fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
				(void)close(fd);
				p->perf_stat[i].fd = -1;
			}
		}
	}
	return 0;
}

/*
 *  perf_stop()
 *	stop and read counters
 */
int perf_stop(perf_t *p)
{
	size_t i = 0;

	if (!p)
		return -1;
	if (!p->perf_opened)
		return -1;
	for (i = 0; i < PERF_MAX; i++) {
		const int fd = p->perf_stat[i].fd;

		/* assume invalid unless we get good data */
		p->perf_stat[i].valid = false;
		if (fd < 0)
			continue;
		if (ioctl(fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) == 0) {
			perf_data_t data;

			if (read(fd, &data, sizeof(data)) == sizeof(data)) {
				const double scale = data.time_running ?
					data.time_enabled / data.time_running :
					((data.time_enabled == 0) ? 1.0 : 0.0);

				p->perf_stat[i].valid = true;
				p->perf_stat[i].counter += (uint64_t)
					((double)data.counter * scale);
			}
		}
		(void)close(fd);
		p->perf_stat[i].fd = -1;
	}
	return 0;
}

/*
 *  perf_counter
 *	fetch counter and index via perf index
 */
uint64_t perf_counter(
	const perf_t *p,
	const int i)
{
	if ((i < 0) || (i >= PERF_MAX))
		return 0ULL;
	if (p->perf_stat[i].valid)
		return p->perf_stat[i].counter;
	return 0ULL;
}
#endif
