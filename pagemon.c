/*
 * Copyright (C) Colin Ian King 2015-2019
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
 * colin.i.king@gmail.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <libgen.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <ncurses.h>
#include <dirent.h>
#include <libgen.h>
#include <ctype.h>
#include <setjmp.h>

#include "perf.h"

#define APP_NAME		"pagemon"
#define MAX_MAPS		(65536)

#define ADDR_OFFSET		(17)	/* Display x offset from address */
#define HEX_WIDTH		(3)	/* Width of each 2 hex digit value */

#define VIEW_PAGE		(0)	/* View pages in memory map */
#define VIEW_MEM		(1)	/* View memory in hex */

#define MIN_TICKS		(1)
#define MAX_TICKS		(1000)

#define MIN_ZOOM		(1)
#define MAX_ZOOM		(999)

#define MAXIMUM(a, b)		((a) > (b) ? (a) : (b))
#define MINIMUM(a, b)		((a) < (b) ? (a) : (b))

#define DEFAULT_UDELAY		(15000)	/* Delay between each refresh */
#define DEFAULT_TICKS		(60)	/* Ticks between dirty page checks */
#define PROCPATH_MAX		(32)	/* Size of proc pathnames */
#define BLINK_MASK		(0x20)	/* Cursor blink counter mask */

/*
 *  Memory size scaling
 */
#define KB			(1024ULL)
#define MB			(KB * KB)
#define GB			(KB * KB * KB)
#define TB			(KB * KB * KB * KB)

/*
 *  Error returns
 */
#define OK			(0)
#define ERR_NO_MAP_INFO		(-1)
#define ERR_NO_MEM_INFO		(-2)
#define ERR_SMALL_WIN		(-3)
#define ERR_ALLOC_NOMEM		(-4)
#define ERR_TOO_MANY_PAGES	(-5)
#define ERR_TOO_FEW_PAGES	(-6)
#define ERR_RESIZE_FAIL		(-7)
#define ERR_NO_PROCESS		(-8)
#define ERR_FAULT		(-9)

/*
 *  PTE bits from uint64_t in /proc/PID/pagemap
 *  for each mapped page
 */
#define	PAGE_PTE_SOFT_DIRTY	(1ULL << 55)
#define	PAGE_EXCLUSIVE_MAPPED	(1ULL << 56)
#define PAGE_FILE_SHARED_ANON	(1ULL << 61)
#define PAGE_SWAPPED		(1ULL << 62)
#define PAGE_PRESENT		(1ULL << 63)

#define OPT_FLAG_READ_ALL_PAGES	(0x00000001)
#define OPT_FLAG_PID		(0x00000002)

enum {
	WHITE_RED = 1,
	WHITE_BLUE,
	WHITE_YELLOW,
	WHITE_CYAN,
	WHITE_GREEN,
	WHITE_BLACK,
	RED_BLUE,
	BLACK_WHITE,
	BLACK_BLACK,
	BLUE_WHITE,
};

/*
 *  Note that we use 64 bit addresses even for 32 bit systems since
 *  this allows pagemon to run in a 32 bit chroot and still access
 *  the 64 bit mapping info.  Otherwise I'd use uintptr_t instead.
 */
typedef uint64_t addr_t;		/* Addresses */
typedef int64_t index_t;		/* Index into page tables */
typedef uint64_t pagemap_t;		/* PTE page map bits */
typedef uint64_t checksum_t;		/* Pagemap checksum */

/*
 *  Memory map info, represents 1 or more pages
 */
typedef struct {
	addr_t begin;			/* Start of mapping */
	addr_t end;			/* End of mapping */
	char attr[5];			/* Map attributes */
	char dev[6];			/* Map device, if any */
	char name[NAME_MAX + 1];	/* Name of mapping */
} map_t;

/*
 *  Page info, 1 per page with map reference
 *  to the memory map it belongs to
 */
typedef struct {
	addr_t addr;			/* Address */
	map_t   *map;			/* Mapping it is in */
	index_t index;			/* Index into map */
} page_t;

/*
 *  General memory mapping info, containing
 *  a fix set of memory maps, and pointer
 *  to an array of per page info.
 */
typedef struct {
	map_t maps[MAX_MAPS];		/* Mappings */
	uint32_t nmaps;			/* Number of mappings */
	page_t *pages;			/* Pages */
	addr_t npages;			/* Number of pages */
	addr_t last_addr;		/* Last address */
} mem_info_t;

/*
 *  Cursor context, we have one each for the
 *  memory map and page contents views
 */
typedef struct {
	int32_t xpos;			/* Cursor x position */
	int32_t ypos;			/* Cursor y position */
	int32_t xpos_prev;		/* Previous x position */
	int32_t ypos_prev;		/* Previous y position */
	int32_t ypos_max;		/* Max y position */
	int32_t xmax;			/* Width */
	int32_t ymax;			/* Height */
} position_t;

/*
 *  Globals, stashed in a global struct
 */
typedef struct {
	WINDOW *mainwin;		/* curses main window */
	sigjmp_buf env;			/* terminate abort jmp */
	addr_t max_pages;		/* Max pages in system */
	checksum_t checksum;		/* Pagemap check sum */
	checksum_t prev_checksum;	/* Previous checksum */
	uint32_t page_size;		/* Page size in bytes */
	pid_t pid;			/* Process ID */
	mem_info_t mem_info;		/* Mapping and page info */
#if defined(PERF_ENABLED)
	perf_t perf;			/* Perf context */
#endif
	bool curses_started;		/* Are we in curses mode? */
	bool tab_view;			/* Page pop-up info */
	bool vm_view;			/* Process VM stats */
	bool help_view;			/* Help pop-up info */
	bool resized;			/* SIGWINCH occurred */
	bool terminate;			/* SIGSEGV termination */
	bool auto_zoom;			/* Automatic zoom */
#if defined(PERF_ENABLED)
	bool perf_view;			/* Perf statistics */
#endif
	uint8_t view;			/* Default page or memory view */
	uint8_t opt_flags;		/* User option flags */
	char path_refs[PROCPATH_MAX];	/* /proc/$PID/clear_refs */
	char path_pagemap[PROCPATH_MAX];/* /proc/$PID/pagemap */
	char path_maps[PROCPATH_MAX];	/* /proc/$PID/maps */
	char path_mem[PROCPATH_MAX];	/* /proc/$PID/mem */
	char path_status[PROCPATH_MAX];	/* /proc/$PID/status */
	char path_stat[PROCPATH_MAX];	/* /proc/$PID/stat */
	char path_oom[PROCPATH_MAX];	/* /proc/$PID/oom_score */
} global_t;

static global_t g;

/*
 *  mem_to_str()
 *	report memory in different units
 */
static void mem_to_str(const addr_t addr, char *const buf, const size_t buflen)
{
	uint64_t scaled, val = (uint64_t)addr;
	char unit;

	if (val < 99 * MB) {
		scaled = val / KB;
		unit = 'K';
	} else if (val < 99 * GB) {
		scaled = val / MB;
		unit = 'M';
	} else if (val < 99 * TB) {
		scaled = val / GB;
		unit = 'G';
	} else {
		scaled = val / TB;
		unit = 'T';
	}
	(void)snprintf(buf, buflen, "%7" PRIu64 " %c", scaled, unit);
}

/*
 *  read_buf()
 *	read data into a buffer, just
 *	for one liner /proc file reading
 */
static int read_buf(
	const char *path,
	char *const buffer,
	const size_t sz)
{
	int fd;
	ssize_t ret;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	ret = read(fd, buffer, sz);
	(void)close(fd);

	if ((ret < 1) || (ret > (ssize_t)sz))
		return -1;
	buffer[ret - 1] = '\0';
	return 0;
}

/*
 *  proc_name_to_pid()
 *	find a process by name, return PID of
 *	first match found. Zero indicates error
 */
static pid_t proc_name_to_pid(char *const name)
{
	char *ptr;
	bool isnum = true;
	DIR *dir;
	struct dirent *d;
	pid_t pid = 0;

	/* Could be just a pid in numeric form */
	for (ptr = name; *ptr; ptr++) {
		if (!isdigit(*ptr)) {
			isnum = false;
			break;
		}
	}
	if (isnum) {
		pid = (pid_t)strtol(name, NULL, 10);
		if (errno || (pid < 1)) {
			(void)fprintf(stderr, "Invalid pid value '%s'\n", name);
			return 0;
		}
		return pid;
	}

	/* No, search for process name */
	dir = opendir("/proc");
	if (!dir)
		return pid;

	while ((d = readdir(dir)) != NULL) {
		char path[PATH_MAX], buf[4096], *bn;

		if (!isdigit(d->d_name[0]))
			continue;

		(void)snprintf(path, sizeof(path), "/proc/%s/cmdline",
			       d->d_name);

		if (read_buf(path, buf, sizeof(buf)) < 0)
			continue;

		bn = basename(buf);
		if (!bn)
			continue;

		if (!strcmp(name, bn)) {
			pid = (pid_t)strtol(d->d_name, NULL, 10);
			if ((errno == 0) && (pid > 0))
				break;
		}
	}
	(void)closedir(dir);

	if (!pid)
		(void)fprintf(stderr, "Cannot find process '%s'\n", name);
	return pid;
}


/*
 *  read_faults()
 *	read minor and major page faults
 */
static int read_faults(
	uint64_t *const minor_flt,
	uint64_t *const major_flt)
{
	int count = 0;
	char buf[4096], *ptr = buf;

	*minor_flt = 0;
	*major_flt = 0;

	if (read_buf(g.path_stat, buf, sizeof(buf)) < 0)
		return -1;

	/*
	 * Skipping over fields is less expensive
	 * than lots of sscanf fields being parsed
	 */
	while (*ptr) {
		if (*ptr == ' ') {
			count++;
			if (count == 9)
				break;
		}
		ptr++;
	}
	if (!*ptr)
		return -1;

	if (sscanf(ptr, "%" SCNu64 " %*u %" SCNu64, minor_flt, major_flt) != 2)
		return -1;
	return 0;
}

/*
 *  read_oom_score()
 *	read the process oom score
 */
static int read_oom_score(uint64_t *const score)
{
	char buf[4096];

	*score = ~0ULL;

	if (read_buf(g.path_oom, buf, sizeof(buf)) < 0)
		return -1;

	if (sscanf(buf, "%" SCNu64, score) != 1)
		return -1;
	return 0;
}

/*
 *  read_maps()
 *	read memory maps for a specific process
 */
static int read_maps(const bool force)
{
	FILE *fp;
	uint32_t i, j, n = 0;
	char buffer[4096];
	page_t *page;
	checksum_t checksum = 0ULL;
	map_t *map;

	g.mem_info.npages = 0;
	g.mem_info.last_addr = 0;

	if (kill(g.pid, 0) < 0)
		return ERR_NO_PROCESS;
	if (force)
		g.prev_checksum = 0;

	fp = fopen(g.path_maps, "r");
	if (fp == NULL)
		return ERR_NO_MAP_INFO;

	map = g.mem_info.maps;
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		int ret;
		intptr_t length;

		map->name[0] = '\0';
		ret = sscanf(buffer, "%" SCNx64 "-%" SCNx64
			" %5s %*s %6s %*d %s",
			&map->begin,
			&map->end,
			map->attr,
			map->dev,
			map->name);
		if ((ret != 5) && (ret != 4))
			continue;
		if (ret == 4)
			map->name[0]= '\0';

		/* Simple sanity check */
		if (map->end < map->begin)
			continue;

		length = map->end - map->begin;
		/* Check for overflow */
		if (g.mem_info.npages + length < g.mem_info.npages)
			continue;

		if (g.mem_info.last_addr < map->end)
			g.mem_info.last_addr = map->end;

		checksum ^= map->begin;
		checksum <<= 1;
		checksum ^= map->end;
		checksum <<= 1;
		checksum ^= map->attr[0];
		checksum <<= 1;
		checksum ^= map->attr[1];
		checksum <<= 1;
		checksum ^= map->attr[2];
		checksum <<= 1;
		checksum ^= map->attr[3];
		checksum <<= 1;
		checksum ^= length;

		g.mem_info.npages += length / g.page_size;
		n++;
		map++;
		if (n >= MAX_MAPS)
			break;
	}
	(void)fclose(fp);

	checksum += g.mem_info.npages;
	checksum += n;
	g.checksum = checksum;

	/* No change in maps, so nothing to do */
	if (g.checksum == g.prev_checksum)
		return OK;
	g.prev_checksum = checksum;

	/* Unlikely, but need to keep Coverity Scan happy */
	if (g.mem_info.npages > g.max_pages)
		return ERR_TOO_MANY_PAGES;
	if (g.mem_info.npages == 0)
		return ERR_TOO_FEW_PAGES;

	free(g.mem_info.pages);
	g.mem_info.nmaps = n;
	g.mem_info.pages = calloc(g.mem_info.npages, sizeof(page_t));
	if (!g.mem_info.pages) {
		g.mem_info.nmaps = 0;
		return ERR_ALLOC_NOMEM;
	}

	map = g.mem_info.maps;
	page = g.mem_info.pages;
	for (i = 0; i < g.mem_info.nmaps; i++, map++) {
		addr_t addr = map->begin;
		addr_t count = (map->end - map->begin) / g.page_size;

		for (j = 0; j < count; j++, page++) {
			page->addr = addr;
			page->map = map;
			page->index = i;
			addr += g.page_size;
		}
	}
	return (n == 0) ? ERR_NO_MAP_INFO : OK;
}

/*
 *  handle_winch()
 *	handle SIGWINCH, flag a window resize
 */
static void handle_winch(int sig)
{
	(void)sig;

	g.resized = true;
}

/*
 *  handle_terminate()
 *	handle termination signals
 */
static void handle_terminate(int sig)
{
	static bool already_handled = false;

	(void)sig;

	if (already_handled)
		exit(EXIT_FAILURE);

	g.terminate = true;
	siglongjmp(g.env, 1);
}

/*
 *  show_usage()
 *	mini help info
 */
static void show_usage(void)
{
	(void)printf(APP_NAME ", version " VERSION "\n\n"
		"Usage: " APP_NAME " [options]\n"
		" -a        enable automatic zoom mode\n"
		" -d        delay in microseconds between refreshes, "
			"default %u\n"
		" -h        help\n"
		" -p pid    process ID to monitor\n"
		" -r        read (page back in) pages at start\n"
		" -t ticks  ticks between dirty page checks\n"
		" -v        enable VM view\n"
		" -z zoom   set page zoom scale\n",
		DEFAULT_UDELAY);
}

#if defined(PERF_ENABLED)
/*
 *  show_perf()
 *	show perf stats
 */
static void show_perf(void)
{
	int y = LINES - 6;
	const int x = 2;
	static uint8_t perf_ticker = 0;

	/* Don't hammer perf to death */
	if (++perf_ticker > 10) {
		perf_stop(&g.perf);
		perf_start(&g.perf, g.pid);
		perf_ticker = 0;
	}
	(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_CYAN) | A_BOLD);
	(void)mvwprintw(g.mainwin, y + 0, x,
		" Page Faults (User Space):   %15" PRIu64 " ",
		perf_counter(&g.perf, PERF_TP_PAGE_FAULT_USER));
	(void)mvwprintw(g.mainwin, y + 1, x,
		" Page Faults (Kernel Space): %15" PRIu64 " ",
		perf_counter(&g.perf, PERF_TP_PAGE_FAULT_KERNEL));
	(void)mvwprintw(g.mainwin, y + 2, x,
		" Kernel Page Allocate:       %15" PRIu64 " ",
		perf_counter(&g.perf, PERF_TP_MM_PAGE_ALLOC));
	(void)mvwprintw(g.mainwin, y + 3, x,
		" Kernel Page Free:           %15" PRIu64 " ",
		perf_counter(&g.perf, PERF_TP_MM_PAGE_FREE));

}
#endif

/*
 *  show_vm()
 *	show Virtual Memory stats
 */
static void show_vm(void)
{
	FILE *fp;
	char buffer[4096];
	int y = 2;
	const int x = COLS - 26;
	uint64_t major, minor, score;

	fp = fopen(g.path_status, "r");
	if (fp == NULL)
		return;

	(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char vmname[9], size[8];
		char state[6], longstate[13];
		uint64_t sz;

		if (sscanf(buffer, "State: %5s %12s", state, longstate) == 2) {
			(void)mvwprintw(g.mainwin, y++, x,
				" State:    %-12.12s ", longstate);
			continue;
		}
		if (sscanf(buffer, "Vm%8s %" SCNu64 "%7s",
		    vmname, &sz, size) == 3) {
			(void)mvwprintw(g.mainwin, y++, x,
				" Vm%-6.6s %10" PRIu64 " %s ",
				vmname, sz, size);
			continue;
		}
	}
	(void)fclose(fp);

	if (!read_faults(&minor, &major)) {
		(void)mvwprintw(g.mainwin, y++, x, " %-23s", "Page Faults:");
		(void)mvwprintw(g.mainwin, y++, x,
			" Minor: %12" PRIu64 "    ", minor);
		(void)mvwprintw(g.mainwin, y++, x,
			" Major: %12" PRIu64 "    ", major);
	}

	if (!read_oom_score(&score)) {
		(void)mvwprintw(g.mainwin, y++, x,
			" OOM Score: %8" PRIu64 "    ", score);
	}
}

/*
 *  show_page_bits()
 *	show info based on the page bit pattern
 */
static void show_page_bits(
	const int fd,
	map_t *const map,
	const index_t idx)
{
	pagemap_t pagemap_info;
	off_t offset;
	char buf[16];
	const int x = 2;
	int kfd;

	mem_to_str(map->end - map->begin, buf, sizeof(buf) - 1);
	(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
	(void)mvwprintw(g.mainwin, 2, x,
		" Page:      0x%16.16" PRIx64 "%18s",
		g.mem_info.pages[idx].addr, "");
	(void)mvwprintw(g.mainwin, 3, x,
		" Page Size: 0x%8.8" PRIx32 " bytes%20s",
		g.page_size, "");
	(void)mvwprintw(g.mainwin, 4, x,
		" Map:       0x%16.16" PRIx64 "-%16.16" PRIx64 " ",
		map->begin, map->end - 1);
	(void)mvwprintw(g.mainwin, 5, x,
		" Map Size:  %s%27s", buf, "");
	(void)mvwprintw(g.mainwin, 6, x,
		" Device:    %5.5s%31s",
		map->dev, "");
	(void)mvwprintw(g.mainwin, 7, x,
		" Prot:      %4.4s%32s",
		map->attr, "");
	(void)mvwprintw(g.mainwin, 8, x,
		" Map Name:  %-35.35s ", map->name[0] == '\0' ?
			"[Anonymous]" : basename(map->name));

	offset = sizeof(pagemap_t) *
		(g.mem_info.pages[idx].addr / g.page_size);
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
		return;
	if (read(fd, &pagemap_info, sizeof(pagemap_info)) != sizeof(pagemap_info))
		return;

	(void)mvwprintw(g.mainwin, 9, x,
		" Flag:      0x%16.16" PRIx64 "%18s", pagemap_info, "");
	if (pagemap_info & PAGE_SWAPPED) {
		(void)mvwprintw(g.mainwin, 10, x,
			" Swap Type:           0x%2.2" PRIx64 "%22s",
			pagemap_info & 0x1f, "");
		(void)mvwprintw(g.mainwin, 11, x,
			" Swap Offset:         0x%16.16" PRIx64 "%8s",
			(uint64_t)(pagemap_info & 0x00ffffffffffffffULL) >> 5, "");
	} else {
		(void)mvwprintw(g.mainwin, 10, x, "%48s", "");
		if (pagemap_info & PAGE_PRESENT) {
			(void)mvwprintw(g.mainwin, 11, x,
				" Physical Address:    0x%16.16" PRIx64 "%8s",
				(uint64_t)(pagemap_info & 0x00ffffffffffffffULL) * g.page_size, "");
		} else {
			(void)mvwprintw(g.mainwin, 11, x,
				" Physical Address:    0x----------------%8s", "");
		}
	}
	(void)mvwprintw(g.mainwin, 12, x,
		" Soft-dirty PTE:      %3s%23s",
		(pagemap_info & PAGE_PTE_SOFT_DIRTY) ? "Yes" : "No ", "");
	(void)mvwprintw(g.mainwin, 13, x,
		" Exclusively Mapped:  %3s%23s",
		(pagemap_info & PAGE_EXCLUSIVE_MAPPED) ? "Yes" : "No ", "");
	(void)mvwprintw(g.mainwin, 14, x,
		" File or Shared Anon: %3s%23s",
		(pagemap_info & PAGE_FILE_SHARED_ANON) ? "Yes" : "No ", "");
	(void)mvwprintw(g.mainwin, 15, x,
		" Present in Swap:     %3s%23s",
		(pagemap_info & PAGE_SWAPPED) ? "Yes" : "No ", "");
	(void)mvwprintw(g.mainwin, 16, x,
		" Present in RAM:      %3s%23s",
		(pagemap_info & PAGE_PRESENT) ? "Yes" : "No ", "");

	kfd = open("/proc/kpagecount", O_RDONLY);
	if (kfd > -1) {
		uint64_t count;

		offset = sizeof(count) *
			(pagemap_info & 0x00ffffffffffffffULL);

		if (lseek(kfd, offset, SEEK_SET) == (off_t)-1)
			goto close_kfd;
		if (read(kfd, &count, sizeof(count)) != sizeof(count)) 
			goto close_kfd;
		(void)mvwprintw(g.mainwin, 16, x,
			" KPageCount:          %-10" PRIu64 "%13s", count, "");
		
close_kfd:
		(void)close(kfd);
	}
}

/*
 *  banner()
 *	clear a banner across the window
 */
static inline void banner(const int y)
{
	(void)mvwprintw(g.mainwin, y, 0, "%*s", COLS, "");
}

/*
 *  show_pages()
 *	show page mapping
 */
static int show_pages(
	const index_t cursor_index,
	const index_t page_index,
	const position_t *const p,
	const int32_t zoom)
{
	int32_t i;
	index_t idx;
	const uint32_t shift = ((uint32_t)(8 * sizeof(pagemap_t) - 4 -
		__builtin_clzll((g.page_size))));
	int fd;
	map_t *map;
	const int32_t xmax = p->xmax, ymax = p->ymax;
	pagemap_t pagemap_info_buf[xmax];

	if ((fd = open(g.path_pagemap, O_RDONLY)) < 0)
		return ERR_NO_MAP_INFO;

	idx = page_index;
	for (i = 1; i <= ymax; i++) {
		int32_t j;
		addr_t addr, offset;
		const size_t sz = sizeof(pagemap_info_buf);

		if (idx >= (index_t)g.mem_info.npages) {
			(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_BLACK));
			(void)mvwprintw(g.mainwin, i, 0, "---------------- ");
		} else {
			addr = g.mem_info.pages[idx].addr;
			(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE));
			(void)mvwprintw(g.mainwin, i, 0, "%16.16" PRIx64 " ", addr);
		}

		/*
		 *  Slurp up an entire row
		 */
		(void)memset(pagemap_info_buf, 0, sz);

		if (idx >= (index_t)g.mem_info.npages) {
			addr = 0;
			map = NULL;
		} else {
			addr = g.mem_info.pages[idx].addr;
			map = g.mem_info.pages[idx].map;
			offset = (addr >> shift) & ~7ULL;

			if (lseek(fd, offset, SEEK_SET) != (off_t)-1) {
				ssize_t ret = read(fd, pagemap_info_buf, sz);
				(void)ret;
			}
		}

		for (j = 0; j < xmax; j++) {
			char state = '.';
			int attr = COLOR_PAIR(BLACK_WHITE);

			if (idx >= (index_t)g.mem_info.npages) {
				attr = COLOR_PAIR(BLACK_BLACK);
				state = '~';
			} else {
				map_t *new_map;
				register pagemap_t pagemap_info;

				new_map = g.mem_info.pages[idx].map;
				/*
				 *  On a different mapping? If so, slurp up
				 *  the new mappings from here to end
				 */
				if (new_map != map) {
					map = new_map;
					addr = g.mem_info.pages[idx].addr;
					offset = (addr >> shift) & ~7;
					if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
						break;
					if (read(fd, &pagemap_info_buf[j],
					    (xmax - j) * sizeof(pagemap_t)) < 0)
						break;
				}

				__builtin_prefetch(&g.mem_info.pages[idx + zoom].addr, 1, 1);

				pagemap_info = pagemap_info_buf[j];
				attr = COLOR_PAIR(BLACK_WHITE);
				if (pagemap_info & PAGE_PRESENT) {
					attr = COLOR_PAIR(WHITE_YELLOW);
					state = 'P';
				}
				if (pagemap_info & PAGE_SWAPPED) {
					attr = COLOR_PAIR(WHITE_GREEN);
					state = 'S';
				}
				if (pagemap_info & PAGE_FILE_SHARED_ANON) {
					attr = COLOR_PAIR(WHITE_RED);
					state = 'M';
				}
				if (pagemap_info & PAGE_PTE_SOFT_DIRTY) {
					attr = COLOR_PAIR(WHITE_CYAN);
					state = 'D';
				}
				idx += zoom;
			}
			(void)wattrset(g.mainwin, attr);
			(void)mvwprintw(g.mainwin, i, ADDR_OFFSET + j, "%c", state);
		}
	}
	(void)wattrset(g.mainwin, A_NORMAL);

	map = g.mem_info.pages[cursor_index].map;
	if (map && g.tab_view)
		show_page_bits(fd, map, cursor_index);
	if (g.vm_view)
		show_vm();
#if defined(PERF_ENABLED)
	if (g.perf_view)
		show_perf();
#endif

	(void)close(fd);
	return 0;
}

/*
 *  show_memory()
 *	show memory contents
 */
static int show_memory(
	const index_t page_index,
	index_t data_index,
	const position_t *const p)
{
	addr_t addr;
	index_t idx = page_index;
	int32_t i;
	const int32_t xmax = p->xmax, ymax = p->ymax;
	int fd;

	if ((fd = open(g.path_mem, O_RDONLY)) < 0)
		return ERR_NO_MEM_INFO;

	for (i = 1; i <= ymax; i++) {
		int32_t j;
		uint8_t bytes[xmax];
		ssize_t nread = 0;

		addr = g.mem_info.pages[idx].addr + data_index;
		if (lseek(fd, (off_t)addr, SEEK_SET) == (off_t)-1) {
			nread = -1;
		} else {
			nread = read(fd, bytes, (size_t)xmax);
			if (nread < 0)
				nread = -1;
		}

		(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE));
		if (idx >= (index_t)g.mem_info.npages)
			(void)mvwprintw(g.mainwin, i, 0, "---------------- ");
		else
			(void)mvwprintw(g.mainwin, i, 0, "%16.16" PRIx64 " ", addr);
		(void)mvwprintw(g.mainwin, i, COLS - 3, "   ");

		for (j = 0; j < xmax; j++) {
			uint8_t byte;

			addr = g.mem_info.pages[idx].addr + data_index;
			if ((idx >= (index_t)g.mem_info.npages) ||
			    (addr > g.mem_info.last_addr)) {
				/* End of memory */
				(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_BLACK));
				(void)mvwprintw(g.mainwin, i, ADDR_OFFSET +
					(HEX_WIDTH * j), "   ");
				(void)mvwprintw(g.mainwin, i, ADDR_OFFSET +
					(HEX_WIDTH * xmax) + j, " ");
				goto do_border;
			}
			if (j > nread) {
				/* Failed to read data */
				(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE));
				(void)mvwprintw(g.mainwin, i, ADDR_OFFSET +
					(HEX_WIDTH * j), "?? ");
				(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE));
				(void)mvwprintw(g.mainwin, i, ADDR_OFFSET +
					(HEX_WIDTH * xmax) + j, "?");
				goto do_border;
			}

			/* We have some legimate data to display */
 			byte = bytes[j];
			(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE));
			(void)mvwprintw(g.mainwin, i, ADDR_OFFSET +
				(HEX_WIDTH * j), "%2.2" PRIx8 " ", byte);
			(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE));
			byte &= 0x7f;
			(void)mvwprintw(g.mainwin, i, ADDR_OFFSET +
				(HEX_WIDTH * xmax) + j, "%c",
				(byte < 32 || byte > 126) ? '.' : byte);
do_border:
			(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE));
			(void)mvwprintw(g.mainwin, i, 16 + (HEX_WIDTH * xmax), " ");
			data_index++;
			if (data_index >= g.page_size) {
				data_index -= g.page_size;
				idx++;
			}
		}
	}
	(void)close(fd);

	return 0;
}

/*
 *  read_all_pages()
 *	read in all pages into memory, this
 *	will force swapped out pages back into
 *	memory
 */
static int read_all_pages(void)
{
	int fd;
	index_t idx;

	if ((fd = open(g.path_mem, O_RDONLY)) < 0)
		return ERR_NO_MEM_INFO;

	for (idx = 0; idx < (index_t)g.mem_info.npages; idx++) {
		const off_t addr = g.mem_info.pages[idx].addr;
		uint8_t byte;

		if (lseek(fd, addr, SEEK_SET) == (off_t)-1)
			continue;
		if (read(fd, &byte, sizeof(byte)) < 0)
			continue;
	}
	(void)close(fd);

	return 0;
}

/*
 *  show_key()
 *	show key for mapping info
 */
static inline void show_key(void)
{
	banner(LINES - 1);
	if (g.view == VIEW_PAGE) {
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)mvwprintw(g.mainwin, LINES - 1, 0, "Page View, KEY: ");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_RED));
		(void)wprintw(g.mainwin, "A");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)wprintw(g.mainwin, " Anon/File, ");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_YELLOW));
		(void)wprintw(g.mainwin, "P");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)wprintw(g.mainwin, " Present in RAM, ");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_CYAN));
		(void)wprintw(g.mainwin, "D");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)wprintw(g.mainwin, " Dirty, ");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_GREEN));
		(void)wprintw(g.mainwin, "S");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)wprintw(g.mainwin, " Swap, ");
		(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE));
		(void)wprintw(g.mainwin, ".");
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)wprintw(g.mainwin, " not in RAM");
		(void)wattrset(g.mainwin, COLOR_PAIR(BLACK_WHITE) | A_BOLD);
	} else {
		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		(void)mvwprintw(g.mainwin, LINES - 1, 0, "%-*s", COLS, "Memory View");
	}
}

/*
 *  show_help()
 *	show pop-up help info
 */
static inline void show_help(void)
{
	const int x = (COLS - 45) / 2;
	int y = (LINES - 15) / 2;

	(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
	(void)mvwprintw(g.mainwin, y++,  x,
		" Pagemon Process Memory Monitor Quick Help ");
	(void)mvwprintw(g.mainwin, y++,  x,
		"%43s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" ? or h     This help information%10s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" Esc or q   Quit%27s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" Tab        Toggle page information%8s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" Enter      Toggle map/memory views%8s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" + or z     Zoom in memory map%13s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" - or Z     Zoom out memory map%12s", "");
	(void)mvwprintw(g.mainwin, y++,  x,
		" R or r     Read pages (swap in all pages) ");
	(void)mvwprintw(g.mainwin, y++,  x,
		" A or a     Toggle Auto Zoom on/off        ");
	(void)mvwprintw(g.mainwin, y++,  x,
		" V or v     Toggle Virtual Memory Stats    ");
#if defined(PERF_ENABLED)
	(void)mvwprintw(g.mainwin, y++,  x,
		" P or p     Toggle Perf Page Stats         ");
#endif
	(void)mvwprintw(g.mainwin, y++,  x,
		" PgUp/Down  Scroll up/down 1/2 page%8s", "");
	(void)mvwprintw(g.mainwin, y++, x,
		" Home/End   Move cursor back to top/bottom ");
	(void)mvwprintw(g.mainwin, y, x,
		" [ / ]      Zoom 1 / Zoom 999              ");
	(void)mvwprintw(g.mainwin, y, x,
		" Cursor keys move Up/Down/Left/Right%7s", "");
}

/*
 *  update_xymax()
 *	set the xymax scale for a specific view v
 *	based on column width and scaling factor for
 *	page or mem (hex) views
 */
static inline void update_xymax(position_t *const position, const int v)
{
	static const int32_t xmax_scale[] = {
		1,	/* VIEW_PAGE */
		4	/* VIEW_MEM */
	};

	position[v].xmax = (COLS - ADDR_OFFSET) / xmax_scale[v];
	position[v].ymax = LINES - 2;
}

/*
 *  reset_cursor()
 *	reset to home position
 */
static inline void reset_cursor(
	position_t *const p,
	index_t *const data_index,
	index_t *const page_index)
{
	p->xpos = 0;
	p->ypos = 0;
	*data_index = 0;
	*page_index = 0;
}

int main(int argc, char **argv)
{
	struct sigaction action;
	map_t *map;
	useconds_t udelay;
	position_t position[2];
	index_t page_index, prev_page_index;
	index_t data_index, prev_data_index;
	int32_t tick, ticks, blink, zoom;
	int rc, ret;

	if (sigsetjmp(g.env, 0)) {
		rc = ERR_FAULT;
		goto terminate;
	}

	g.pid = -1;
	rc = OK;
	blink = 0;
	zoom = MIN_ZOOM;
	ticks = DEFAULT_TICKS;
	tick = 0;
	udelay = DEFAULT_UDELAY;
	page_index = 0;
	data_index = 0;

	for (;;) {
		int c = getopt(argc, argv, "ad:hp:rt:vz:");

		if (c == -1)
			break;
		switch (c) {
		case 'a':
			g.auto_zoom = true;
			break;
		case 'd':
			udelay = strtoul(optarg, NULL, 10);
			if (errno) {
				(void)fprintf(stderr, "Invalid delay value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			show_usage();
			exit(EXIT_SUCCESS);
			break;
		case 'p':
			g.pid = proc_name_to_pid(optarg);
			if (g.pid < 1)
				exit(EXIT_FAILURE);
			g.opt_flags |= OPT_FLAG_PID;
			break;
		case 'r':
			g.opt_flags |= OPT_FLAG_READ_ALL_PAGES;
			break;
		case 't':
			ticks = strtol(optarg, NULL, 10);
			if ((ticks < MIN_TICKS) || (ticks > MAX_TICKS)) {
				(void)fprintf(stderr, "Invalid ticks value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			g.vm_view = true;
			break;
		case 'z':
			zoom = strtoul(optarg, NULL, 10);
			if (errno || (zoom < MIN_ZOOM) || (zoom > MAX_ZOOM)) {
				(void)fprintf(stderr, "Invalid zoom value\n");
				exit(EXIT_FAILURE);
			}
			break;
		default:
			show_usage();
			exit(EXIT_FAILURE);
		}
	}
	if (!(g.opt_flags & OPT_FLAG_PID)) {
		(void)fprintf(stderr, "Must provide process ID with -p option\n");
		exit(EXIT_FAILURE);
	}
	if (geteuid() != 0) {
		(void)fprintf(stderr, "%s requires root privileges to "
			"access memory of pid %d\n", APP_NAME, g.pid);
		exit(EXIT_FAILURE);
	}
	if (kill(g.pid, 0) < 0) {
		(void)fprintf(stderr, "No such process %d\n", g.pid);
		exit(EXIT_FAILURE);
	}
	g.page_size = sysconf(_SC_PAGESIZE);
	if (g.page_size == (uint32_t)-1) {
		/* Guess */
		g.page_size = 4096UL;
	}
	g.max_pages = ((addr_t)((size_t)~0)) / g.page_size;
	(void)memset(&action, 0, sizeof(action));
	action.sa_handler = handle_winch;
	if (sigaction(SIGWINCH, &action, NULL) < 0) {
		(void)fprintf(stderr, "Could not set up window resizing handler\n");
		exit(EXIT_FAILURE);
	}
	(void)memset(&action, 0, sizeof(action));
	action.sa_handler = handle_terminate;
	if (sigaction(SIGSEGV, &action, NULL) < 0) {
		(void)fprintf(stderr, "Could not set up error handler\n");
		exit(EXIT_FAILURE);
	}
	if (sigaction(SIGBUS, &action, NULL) < 0) {
		(void)fprintf(stderr, "Could not set up error handler\n");
		exit(EXIT_FAILURE);
	}

	(void)snprintf(g.path_refs, sizeof(g.path_refs),
		"/proc/%i/clear_refs", g.pid);
	(void)snprintf(g.path_pagemap, sizeof(g.path_pagemap),
		"/proc/%i/pagemap", g.pid);
	(void)snprintf(g.path_maps, sizeof(g.path_maps),
		"/proc/%i/maps", g.pid);
	(void)snprintf(g.path_mem, sizeof(g.path_mem),
		"/proc/%i/mem", g.pid);
	(void)snprintf(g.path_status, sizeof(g.path_status),
		"/proc/%i/status", g.pid);
	(void)snprintf(g.path_stat, sizeof(g.path_stat),
		"/proc/%i/stat", g.pid);
	(void)snprintf(g.path_oom, sizeof(g.path_stat),
		"/proc/%i/oom_score", g.pid);

	(void)initscr();
	(void)start_color();
	(void)cbreak();
	(void)noecho();
	(void)nodelay(stdscr, 1);
	(void)keypad(stdscr, 1);
	(void)curs_set(0);
	g.mainwin = newwin(LINES, COLS, 0, 0);
	g.curses_started = true;

	(void)init_pair(WHITE_RED, COLOR_WHITE, COLOR_RED);
	(void)init_pair(WHITE_BLUE, COLOR_WHITE, COLOR_BLUE);
	(void)init_pair(WHITE_YELLOW, COLOR_WHITE, COLOR_YELLOW);
	(void)init_pair(WHITE_CYAN, COLOR_WHITE, COLOR_CYAN);
	(void)init_pair(WHITE_GREEN, COLOR_WHITE, COLOR_GREEN);
	(void)init_pair(WHITE_BLACK, COLOR_WHITE, COLOR_BLACK);
	(void)init_pair(BLACK_WHITE, COLOR_BLACK, COLOR_WHITE);
	(void)init_pair(RED_BLUE, COLOR_RED, COLOR_BLUE);
	(void)init_pair(BLACK_BLACK, COLOR_BLACK, COLOR_BLACK);
	(void)init_pair(BLUE_WHITE, COLOR_BLUE, COLOR_WHITE);

	(void)memset(position, 0, sizeof(position));
	update_xymax(position, 0);
	update_xymax(position, 1);

#if defined(PERF_ENABLED)
	perf_start(&g.perf, g.pid);
#endif

	for (;;) {
		int ch, blink_attrs;
		char cursor_ch;
		position_t *p = &position[g.view];
		addr_t show_addr;
		float percent;

		if ((!tick) && (g.view == VIEW_PAGE)) {
			if ((rc = read_maps(false)) < 0)
				break;
		}
		if ((g.view == VIEW_PAGE) && g.auto_zoom) {
			const int32_t window_pages = p->xmax * p->ymax;

			zoom = (g.mem_info.npages + window_pages - 1) /
				window_pages;
			zoom = MINIMUM(MAX_ZOOM, zoom);
			zoom = MAXIMUM(MIN_ZOOM, zoom);
		}
		if (g.opt_flags & OPT_FLAG_READ_ALL_PAGES) {
			read_all_pages();
			g.opt_flags &= ~OPT_FLAG_READ_ALL_PAGES;
		}
		if (!tick) {
			int fd;
			tick = 0;

			fd = open(g.path_refs, O_RDWR);
			if (fd > -1) {
				ret = write(fd, "4", 1);
				(void)ret;
				(void)close(fd);
			}
		}
		tick++;
		if (tick > ticks)
			tick = 0;

		/*
		 *  SIGWINCH window resize triggered so
		 *  handle window resizing in ugly way
		 */
		if (g.resized) {
			int newx, newy;
			const index_t cursor_index = page_index +
				zoom * (p->xpos + (p->ypos * p->xmax));
			struct winsize ws;

			if (ioctl(fileno(stdin), TIOCGWINSZ, &ws) < 0) {
				rc = ERR_RESIZE_FAIL;
				break;
			}
			newy = ws.ws_row;
			newx = ws.ws_col;

			/* Way too small, give up */
			if ((COLS < 23) || (LINES < 1)) {
				rc = ERR_SMALL_WIN;
				break;
			}

			(void)resizeterm(newy, newx);
			(void)wresize(g.mainwin, newy, newx);
			(void)wrefresh(g.mainwin);
			(void)refresh();

			(void)wbkgd(g.mainwin, COLOR_PAIR(RED_BLUE));
			g.resized = false;
			p->xpos = 0;
			p->ypos = 0;
			page_index = cursor_index;
		}

		/*
		 *  Window getting too small, tell user
		 */
		if ((COLS < 80) || (LINES < 23)) {
			(void)wclear(g.mainwin);
			(void)wbkgd(g.mainwin, COLOR_PAIR(RED_BLUE));
			(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
			(void)mvwprintw(g.mainwin, LINES / 2, (COLS / 2) - 8,
				" WINDOW TOO SMALL ");
			(void)wrefresh(g.mainwin);
			(void)refresh();
			(void)usleep(udelay);
			continue;
		}

		update_xymax(position, g.view);
		(void)wbkgd(g.mainwin, COLOR_PAIR(RED_BLUE));
		show_key();

		blink++;
		if (g.view == VIEW_MEM) {
			int32_t curxpos = (p->xpos * 3) + ADDR_OFFSET;
			const position_t *pc = &position[VIEW_PAGE];
			const index_t cursor_index = page_index +
				zoom * (pc->xpos + ((index_t)pc->ypos * pc->xmax));
			percent = (g.mem_info.npages > 0) ?
				100.0 * cursor_index / g.mem_info.npages : 100;

			/* Memory may have shrunk, so check this */
			if (cursor_index >= (index_t)g.mem_info.npages) {
				/* Force end of memory key action */
				ch = KEY_END;
				goto force_ch;
			}

			map = g.mem_info.pages[cursor_index].map;
			show_addr = g.mem_info.pages[cursor_index].addr +
				data_index + (p->xpos + (p->ypos * p->xmax));
			if (show_memory(cursor_index, data_index, p) < 0)
				break;

			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(WHITE_BLUE) :
				COLOR_PAIR(BLUE_WHITE));
			(void)wattrset(g.mainwin, blink_attrs);
			cursor_ch = mvwinch(g.mainwin, p->ypos + 1, curxpos)
				& A_CHARTEXT;
			(void)mvwprintw(g.mainwin, p->ypos + 1, curxpos,
				"%c", cursor_ch);
			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(BLACK_WHITE) :
				COLOR_PAIR(WHITE_BLACK));
			curxpos = ADDR_OFFSET + (p->xmax * 3) + p->xpos;
			(void)wattrset(g.mainwin, blink_attrs);
			cursor_ch = mvwinch(g.mainwin, p->ypos + 1, curxpos)
				& A_CHARTEXT;
			(void)mvwprintw(g.mainwin, p->ypos + 1, curxpos,
				"%c", cursor_ch);
		} else {
			int32_t curxpos = p->xpos + ADDR_OFFSET;
			const index_t cursor_index = page_index +
				zoom * (p->xpos + ((index_t)p->ypos * p->xmax));
			percent = (g.mem_info.npages > 0) ?
				100.0 * cursor_index / g.mem_info.npages : 100;

			/* Memory may have shrunk, so check this */
			if (cursor_index >= (index_t)g.mem_info.npages) {
				/* Force end of memory key action */
				ch = KEY_END;
				goto force_ch;
			}

			map = g.mem_info.pages[cursor_index].map;
			show_addr = g.mem_info.pages[cursor_index].addr;
			show_pages(cursor_index, page_index, p, zoom);

			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(BLACK_WHITE) :
				COLOR_PAIR(WHITE_BLACK));
			(void)wattrset(g.mainwin, blink_attrs);
			cursor_ch = mvwinch(g.mainwin, p->ypos + 1, curxpos)
				& A_CHARTEXT;
			(void)mvwprintw(g.mainwin, p->ypos + 1, curxpos,
				"%c", cursor_ch);
		}
		ch = getch();

		if (g.help_view)
			show_help();

		(void)wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		banner(0);
		if (!map) {
			(void)mvwprintw(g.mainwin, 0, 0,
				"Pagemon 0x---------------- %4.4s x %-3d ",
				g.auto_zoom && ((blink & BLINK_MASK)) ?
				"Auto" : "Zoom", zoom);
			(void)wprintw(g.mainwin, "---- --:-- %-20.20s",
				"[Not Mapped]");
		} else {
			(void)mvwprintw(g.mainwin, 0, 0, "Pagemon 0x%16.16" PRIx64
				" %4.4s x %-3d ", show_addr,
				g.auto_zoom && ((blink & BLINK_MASK)) ?
					"Auto" : "Zoom", zoom);
			(void)wprintw(g.mainwin, "%s %s %-20.20s",
				map->attr, map->dev,
				map->name[0] == '\0' ?
					"[Anonymous]" : basename(map->name));
		}
		(void)mvwprintw(g.mainwin, 0, COLS - 8, " %6.1f%%", percent);

		(void)wrefresh(g.mainwin);
		(void)refresh();
force_ch:
		prev_page_index = page_index;
		prev_data_index = data_index;
		p->xpos_prev = p->xpos;
		p->ypos_prev = p->ypos;

		switch (ch) {
		case 27:	/* ESC */
		case 'q':
		case 'Q':
			/* Quit */
			g.terminate = true;
			break;
#if defined(PERF_ENABLED)
		case 'p':
		case 'P':
			/* Toggle perf stats */
			g.perf_view = !g.perf_view;
			break;
#endif
		case '\t':
			/* Toggle Tab view */
			g.tab_view = !g.tab_view;
			break;
		case 'v':
		case 'V':
			/* Toggle VM stats view */
			g.vm_view = !g.vm_view;
			break;
		case '?':
		case 'h':
			/* Toggle Help */
			g.help_view = !g.help_view;
			break;
		case 'r':
		case 'R':
			read_all_pages();
			break;
		case 'a':
		case 'A':
			/* Toggle auto zoom */
			g.auto_zoom = !g.auto_zoom;
			break;
		case '\n':
			/* Toggle MAP / MEMORY views */
			g.view ^= 1;
			p = &position[g.view];
			blink = 0;
			break;
		case '+':
		case 'z':
			/* Zoom in */
			if (g.view == VIEW_PAGE) {
				zoom++ ;
				zoom = MINIMUM(MAX_ZOOM, zoom);
				reset_cursor(p, &data_index, &page_index);
			}
			break;
		case '-':
		case 'Z':
			/* Zoom out */
			if (g.view == VIEW_PAGE) {
				zoom--;
				zoom = MAXIMUM(MIN_ZOOM, zoom);
				reset_cursor(p, &data_index, &page_index);
			}
			break;
		case '[':
			/* Reset zoom to MIN_ZOOM */
			if (g.view == VIEW_PAGE) {
				g.auto_zoom = false;
				zoom = MIN_ZOOM;
				reset_cursor(p, &data_index, &page_index);
			}
			break;
		case ']':
			/* Reset zoom to MAX_ZOOM */
			if (g.view == VIEW_PAGE) {
				g.auto_zoom = false;
				zoom = MAX_ZOOM;
				reset_cursor(p, &data_index, &page_index);
			}
			break;
		case 't':
			/* Tick increase */
			ticks++;
			ticks = MINIMUM(MAX_TICKS, ticks);
			break;
		case 'T':
			/* Tick decrease */
			ticks--;
			ticks = MAXIMUM(MIN_TICKS, ticks);
			break;
		case 'c':
		case 'C':
			/* Clear pop ups */
#if defined(PERF_ENABLED)
			g.perf_view = false;
#endif
			g.vm_view = false;
			g.tab_view = false;
			g.help_view = false;
			break;
		case KEY_DOWN:
			blink = 0;
			if (g.view == VIEW_PAGE)
				data_index = 0;
			p->ypos++;
			break;
		case KEY_UP:
			blink = 0;
			if (g.view == VIEW_PAGE)
				data_index = 0;
			p->ypos--;
			break;
		case KEY_LEFT:
			blink = 0;
			if (g.view == VIEW_PAGE)
				data_index = 0;
			p->xpos--;
			break;
		case KEY_RIGHT:
			blink = 0;
			if (g.view == VIEW_PAGE)
				data_index = 0;
			p->xpos++;
			break;
		case KEY_NPAGE:
			if (g.view == VIEW_PAGE)
				data_index = 0;
			blink = 0;
			p->ypos += p->ymax / 2;
			break;
		case KEY_PPAGE:
			if (g.view == VIEW_PAGE)
				data_index = 0;
			blink = 0;
			p->ypos -= p->ymax / 2;
			break;
		case KEY_HOME:
			reset_cursor(p, &data_index, &page_index);
			break;
		case KEY_END:
			if (g.view == VIEW_PAGE) {
				page_index = g.mem_info.npages - 1;
				p->xpos = 0;
			} else {
				data_index = g.page_size -
					((index_t)p->xmax * p->ymax);
			}
			p->ypos = p->ymax - 1;
			p->xpos = p->xmax - 1;
			break;
		}

		position[VIEW_PAGE].ypos_max =
			(((g.mem_info.npages - page_index) / zoom) - p->xpos) /
			position[0].xmax;
		position[VIEW_MEM].ypos_max = position[VIEW_MEM].ymax;

		if (p->xpos >= p->xmax) {
			p->xpos = 0;
			p->ypos++;
		}
		if (p->xpos < 0) {
			p->xpos = p->xmax - 1;
			p->ypos--;
		}

		/*
		 *  Handling yposition overflow / underflow
		 *  is non-trivial as we need to consider
		 *  different views and how to handle the
		 *  scroll data and page index offsets
		 */
		if (g.view == VIEW_MEM) {
			if (p->ypos > p->ymax - 1) {
				data_index += p->xmax *
					(p->ypos - (p->ymax - 1));
				p->ypos = p->ymax - 1;
				if (data_index >= g.page_size) {
					data_index -= g.page_size;
					page_index++;
				}
			}
			if (p->ypos < 0) {
				data_index -= p->xmax * (-p->ypos);
				p->ypos = 0;
				if (data_index < 0) {
					data_index += g.page_size;
					page_index--;
				}
			}
		} else {
			if (p->ypos > p->ymax - 1) {
				page_index += zoom * p->xmax *
					(p->ypos - (p->ymax - 1));
				p->ypos = p->ymax - 1;
			}
			if (p->ypos < 0) {
				page_index -= zoom * p->xmax * (-p->ypos);
				p->ypos = 0;
			}
		}
		if (page_index < 0) {
			page_index = 0;
			data_index = 0;
			p->ypos = 0;
		}
		if (g.view == VIEW_MEM) {
			const position_t *pc = &position[VIEW_PAGE];
			const index_t cursor_index = page_index +
				zoom * (pc->xpos + ((index_t)pc->ypos * pc->xmax));
			const addr_t addr =
				(cursor_index >= (index_t)g.mem_info.npages) ?
					g.mem_info.last_addr :
					g.mem_info.pages[cursor_index].addr +
					data_index + (p->xpos + (p->ypos * p->xmax));

			if (addr >= g.mem_info.last_addr) {
				page_index = prev_page_index;
				data_index = prev_data_index;
				p->xpos = p->xpos_prev;
				p->ypos = p->ypos_prev;
			}
		} else {
			if ((index_t)page_index + ((index_t)zoom * (p->xpos +
			    (p->ypos * p->xmax))) >= (index_t)g.mem_info.npages) {
				const int64_t zoom_xmax = (int64_t)zoom * p->xmax;
				const int64_t lines =
					((zoom_xmax - 1) + g.mem_info.npages) /
					zoom_xmax;
				const addr_t npages =
					(zoom_xmax * lines);
				const addr_t diff = (npages - g.mem_info.npages) /
					zoom;
				const int64_t last = p->xmax - diff;

				if (lines <= p->ymax + 1) {
					p->ypos = lines - 1;
					page_index = 0;
				} else {
					p->ypos = p->ymax - 1;
					page_index = (lines - p->ymax) *
						zoom_xmax;
				}
				if (p->xpos > last - 1)
					p->xpos = last - 1;
			}
		}
		if (g.terminate)
			break;

		if (kill(g.pid, 0) < 0)
			break;
		(void)usleep(udelay);
	}

	(void)werase(g.mainwin);
	(void)wrefresh(g.mainwin);
	(void)refresh();
	(void)delwin(g.mainwin);

terminate:
	if (g.curses_started) {
		(void)clear();
		(void)endwin();
	}

#if defined(PERF_ENABLED)
	perf_stop(&g.perf);
#endif
	free(g.mem_info.pages);

	ret = EXIT_FAILURE;
	switch (rc) {
	case OK:
		ret = EXIT_SUCCESS;
		break;
	case ERR_NO_MAP_INFO:
		(void)fprintf(stderr, "Cannot access memory maps for PID %d\n", g.pid);
		break;
	case ERR_NO_MEM_INFO:
		(void)fprintf(stderr, "Cannot access memory for PID %d\n", g.pid);
		break;
	case ERR_SMALL_WIN:
		(void)fprintf(stderr, "Window too small\n");
		break;
	case ERR_ALLOC_NOMEM:
		(void)fprintf(stderr, "Memory allocation failed\n");
		break;
	case ERR_TOO_MANY_PAGES:
		(void)fprintf(stderr, "Too many pages in process for %s\n", APP_NAME);
		(void)printf("%" PRIu64 " vs %" PRIu64 "\n",
			g.mem_info.npages , g.max_pages);
		break;
	case ERR_TOO_FEW_PAGES:
		(void)fprintf(stderr, "Too few pages in process for %s\n", APP_NAME);
		break;
	case ERR_RESIZE_FAIL:
		(void)fprintf(stderr, "Cannot get window size after a resize event\n");
		break;
	case ERR_NO_PROCESS:
		(void)fprintf(stderr, "Process %d exited\n", g.pid);
		break;
	case ERR_FAULT:
		(void)fprintf(stderr, "Internal error, segmentation fault or bus error\n");
		break;
	default:
		(void)fprintf(stderr, "Unknown failure (%d)\n", rc);
		break;
	}
	exit(ret);
}
