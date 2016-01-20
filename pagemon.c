/*
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
 * Copyright (C) Colin Ian King 2015, 2016
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
#include <fcntl.h>
#include <signal.h>
#include <ncurses.h>

#define APP_NAME		"pagemon"
#define MAX_MMAPS		(65536)

#define ADDR_OFFSET		(17)
#define HEX_WIDTH		(3)

#define VIEW_PAGE		(0)
#define VIEW_MEM		(1)

#define MIN_TICKS		(1)
#define MAX_TICKS		(1000)

#define MIN_ZOOM		(1)
#define MAX_ZOOM		(999)

#define BLINK_MASK		(0x20)

#define OK			(0)
#define ERR_NO_MAP_INFO		(-1)
#define ERR_NO_MEM_INFO		(-2)
#define ERR_SMALL_WIN		(-3)
#define ERR_ALLOC_NOMEM		(-4)
#define ERR_TOO_MANY_PAGES	(-5)

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

typedef struct {
	uint64_t begin;			/* Start of mapping */
	uint64_t end;			/* End of mapping */
	char attr[5];			/* Map attributes */
	char dev[6];			/* Map device, if any */
	char name[NAME_MAX + 1];	/* Name of mapping */
} map_t;

typedef struct {
	uint64_t addr;			/* Address */
	map_t   *map;			/* Mapping it is in */
	int64_t  index;			/* Index into map */
} page_t;

typedef struct {
	map_t maps[MAX_MMAPS];		/* Mappings */
	uint32_t nmaps;			/* Number of mappings */

	page_t *pages;			/* Pages */
	uint64_t npages;		/* Number of pages */

	uint64_t last_addr;		/* Last address */
} mem_info_t;

typedef struct {
	int32_t xpos;			/* Cursor x position */
	int32_t ypos;			/* Cursor y position */
	int32_t xpos_prev;		/* Previous x position */
	int32_t ypos_prev;		/* Previous y position */
	int32_t ypos_max;		/* Max y position */
	int32_t xwidth;			/* Width of x */
} position_t;

/* I dislike globals, but it saves passing these around a lot */
static mem_info_t mem_info;		/* Mapping and page info */
static uint64_t max_pages;		/* Max pages in system */
static uint32_t page_size;		/* Page size in bytes */
static bool tab_view = false;		/* Page pop-up info */
static bool vm_view = false;		/* Process VM stats */
static bool help_view = false;		/* Help pop-up info */
static bool resized = false;		/* SIGWINCH occurred */
static bool auto_zoom = false;		/* Automatic zoom */
static uint8_t view = VIEW_PAGE;	/* Default page or memory view */
static uint8_t opt_flags;		/* User option flags */
WINDOW *mainwin = NULL;			/* curses main window */
static char path_refs[PATH_MAX];
static char path_pagemap[PATH_MAX];
static char path_maps[PATH_MAX];
static char path_mem[PATH_MAX];
static char path_status[PATH_MAX];

/*
 *  read_maps()
 *	read memory maps for a specifc process
 */
static int read_maps(void)
{
	FILE *fp;
	uint32_t i, j, n = 0;
	char buffer[4096];
	page_t *page;
	uint64_t last_addr = 0;

	memset(&mem_info, 0, sizeof(mem_info));
	fp = fopen(path_maps, "r");
	if (fp == NULL)
		return ERR_NO_MAP_INFO;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		int ret;
		int64_t length;

		mem_info.maps[n].name[0] = '\0';
		ret = sscanf(buffer, "%" SCNx64 "-%" SCNx64 " %5s %*s %6s %*d %s",
			&mem_info.maps[n].begin,
			&mem_info.maps[n].end,
			mem_info.maps[n].attr,
			mem_info.maps[n].dev,
			mem_info.maps[n].name);
		if (ret!=5 && ret!=4)
			continue;
		if (ret==4) {
			mem_info.maps[n].name[0]='\0';
		}

		/* Simple sanity check */
		if (mem_info.maps[n].end < mem_info.maps[n].begin)
			continue;

		length = mem_info.maps[n].end - mem_info.maps[n].begin;
		/* Check for overflow */
		if (mem_info.npages + length < mem_info.npages)
			continue;

		if (last_addr < mem_info.maps[n].end)
			last_addr = mem_info.maps[n].end;

		mem_info.npages += length / page_size;
		n++;
		if (n >= MAX_MMAPS)
			break;
	}
	fclose(fp);

	/* Unlikely, but need to keep Coverity Scan happy */
	if (mem_info.npages > max_pages)
		return ERR_TOO_MANY_PAGES;

	mem_info.nmaps = n;
	mem_info.pages = page = calloc(mem_info.npages, sizeof(page_t));
	if (!mem_info.pages)
		return ERR_ALLOC_NOMEM;

	mem_info.last_addr = last_addr;

	for (i = 0; i < mem_info.nmaps; i++) {
		uint64_t count = (mem_info.maps[i].end -
				  mem_info.maps[i].begin) / page_size;
		uint64_t addr = mem_info.maps[i].begin;

		for (j = 0; j < count; j++) {
			page->addr = addr;
			page->map = &mem_info.maps[i];
			page->index = i;
			addr += page_size;
			page++;
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

	resized = true;
}

/*
 *  show_usage()
 *	mini help info
 */
static void show_usage(void)
{
	printf(APP_NAME ", version " VERSION "\n\n"
		"Usage: " APP_NAME " [options]\n"
		" -d        delay in microseconds between refreshes, default 10000\n"
		" -h        help\n"
		" -p pid    process ID to monitor\n"
		" -r        read (page back in) pages at start\n"
		" -t ticks  ticks between dirty page checks\n"
		" -z zoom   set page zoom scale\n");
}

/*
 *  show_vm()
 *	show Virtual Memory stats
 */
static void show_vm(void)
{
	FILE *fp;
	char buffer[4096];
	int y = 3;

	fp = fopen(path_status, "r");
	if (fp == NULL)
		return;

	wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char vmname[9], size[8];
		uint64_t sz;

		if (sscanf(buffer, "Vm%8s %" SCNu64 "%7s",
		    vmname, &sz, size) != 3)
			continue;
		mvwprintw(mainwin, y, 54, "Vm%-6.6s %10" PRIu64 " %s",
			vmname, sz, size);
		y++;
	}
	fclose(fp);
}

/*
 *  show_page_bits()
 *	show info based on the page bit pattern
 */
static void show_page_bits(
	const int fd,
	map_t *map,
	const int64_t index)
{
	uint64_t info;
	off_t offset;

	wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
	mvwprintw(mainwin, 3, 4,
		" Page:      0x%16.16" PRIx64 "%18s",
		mem_info.pages[index].addr, "");
	mvwprintw(mainwin, 4, 4,
		" Page Size: 0x%8.8" PRIx32 " bytes%20s",
		page_size, "");
	mvwprintw(mainwin, 5, 4,
		" Map:       0x%16.16" PRIx64 "-%16.16" PRIx64 " ",
		map->begin, map->end - 1);
	mvwprintw(mainwin, 6, 4,
		" Device:    %5.5s%31s",
		map->dev, "");
	mvwprintw(mainwin, 7, 4,
		" Prot:      %4.4s%32s",
		map->attr, "");
	mvwprintw(mainwin, 8, 4,
		" Map Name:  %-35.35s ", map->name[0] == '\0' ?
			"[Anonymous]" : basename(map->name));

	offset = sizeof(uint64_t) * (mem_info.pages[index].addr / page_size);
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
		return;
	if (read(fd, &info, sizeof(info)) != sizeof(info))
		return;

	mvwprintw(mainwin, 9, 4,
		" Flag:      0x%16.16" PRIx64 "%18s", info, "");
	if (info & PAGE_SWAPPED) {
		mvwprintw(mainwin, 10, 4,
			"   Swap Type:           0x%2.2" PRIx64 "%20s",
			info & 0x1f, "");
		mvwprintw(mainwin, 11, 4,
			"   Swap Offset:         0x%16.16" PRIx64 "%6s",
			(info & 0x00ffffffffffffffULL) >> 5, "");
	} else {
		mvwprintw(mainwin, 10, 4, "%48s", "");
		mvwprintw(mainwin, 11, 4,
			"   Page Frame Number:   0x%16.16" PRIx64 "%6s",
			info & 0x00ffffffffffffffULL, "");
	}
	mvwprintw(mainwin, 12, 4,
		"   Soft-dirty PTE:      %3s%21s",
		(info & PAGE_PTE_SOFT_DIRTY) ? "Yes" : "No ", "");
	mvwprintw(mainwin, 13, 4,
		"   Exclusively Mapped:  %3s%21s",
		(info & PAGE_EXCLUSIVE_MAPPED) ? "Yes" : "No ", "");
	mvwprintw(mainwin, 14, 4,
		"   File or Shared Anon: %3s%21s",
		(info & PAGE_FILE_SHARED_ANON) ? "Yes" : "No ", "");
	mvwprintw(mainwin, 15, 4,
		"   Present in Swap:     %3s%21s",
		(info & PAGE_SWAPPED) ? "Yes" : "No ", "");
	mvwprintw(mainwin, 16, 4,
		"   Present in RAM:      %3s%21s",
		(info & PAGE_PRESENT) ? "Yes" : "No ", "");
}


/*
 *  show_pages()
 *	show page mapping
 */
static int show_pages(
	const int32_t cursor_index,
	const int32_t page_index,
	const int32_t xwidth,
	const int32_t zoom)
{
	int32_t i;
	uint64_t index = page_index;
	int fd;
	map_t *map = mem_info.pages[index].map;

	if ((fd = open(path_pagemap, O_RDONLY)) < 0)
		return ERR_NO_MAP_INFO;

	for (i = 1; i < LINES - 1; i++) {
		uint64_t info;
		int32_t j;

		if (index >= mem_info.npages) {
			wattrset(mainwin, COLOR_PAIR(BLACK_BLACK));
			mvwprintw(mainwin, i, 0, "---------------- ");
		} else {
			uint64_t addr = mem_info.pages[index].addr;
			wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
			mvwprintw(mainwin, i, 0, "%16.16" PRIx64 " ", addr);
		}

		for (j = 0; j < xwidth; j++) {
			char state = '.';
			int attr = COLOR_PAIR(BLACK_WHITE);

			if (index >= mem_info.npages) {
				attr = COLOR_PAIR(BLACK_BLACK);
				state = '~';
			} else {
				uint64_t addr = mem_info.pages[index].addr;
				off_t offset = sizeof(uint64_t) *
					       (addr / page_size);

				if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
					break;
				if (read(fd, &info, sizeof(info)) < 0)
					break;

				attr = COLOR_PAIR(BLACK_WHITE);
				if (info & PAGE_PRESENT) {
					attr = COLOR_PAIR(WHITE_YELLOW);
					state = 'P';
				}
				if (info & PAGE_SWAPPED) {
					attr = COLOR_PAIR(WHITE_GREEN);
					state = 'S';
				}
				if (info & PAGE_FILE_SHARED_ANON) {
					attr = COLOR_PAIR(WHITE_RED);
					state = 'M';
				}
				if (info & PAGE_PTE_SOFT_DIRTY) {
					attr = COLOR_PAIR(WHITE_CYAN);
					state = 'D';
				}
				index += zoom;
			}
			wattrset(mainwin, attr);
			mvwprintw(mainwin, i, ADDR_OFFSET + j, "%c", state);
		}
	}
	wattrset(mainwin, A_NORMAL);

	if (map && tab_view)
		show_page_bits(fd, map, cursor_index);
	if (vm_view)
		show_vm();

	(void)close(fd);
	return 0;
}

/*
 *  show_memory()
 *	show memory contents
 */
static int show_memory(
	const int64_t page_index,
	int64_t data_index,
	const uint32_t page_size,
	const int32_t xwidth)
{
	int32_t i;
	uint64_t index = page_index;
	int fd;
	uint64_t addr;

	if ((fd = open(path_mem, O_RDONLY)) < 0)
		return ERR_NO_MEM_INFO;

	for (i = 1; i < LINES - 1; i++) {
		int32_t j;
		uint8_t bytes[xwidth];
		ssize_t nread = 0;

		addr = mem_info.pages[index].addr + data_index;
		if (lseek(fd, (off_t)addr, SEEK_SET) == (off_t)-1) {
			nread = -1;
		} else {
			nread = read(fd, bytes, (size_t)xwidth);
			if (nread < 0)
				nread = -1;
		}

		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
		if (index >= mem_info.npages)
			mvwprintw(mainwin, i, 0, "---------------- ");
		else
			mvwprintw(mainwin, i, 0, "%16.16" PRIx64 " ", addr);
		mvwprintw(mainwin, i, COLS - 3, "   ", addr);

		for (j = 0; j < xwidth; j++) {
			uint8_t byte;
			addr = mem_info.pages[index].addr + data_index;
			if ((index >= mem_info.npages) ||
			    (addr > mem_info.last_addr)) {
				/* End of memory */
				wattrset(mainwin, COLOR_PAIR(BLACK_BLACK));
				mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * j), "   ");
				mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * xwidth) + j, " ");
				goto do_border;
			}
			if (j > nread) {
				/* Failed to read data */
				wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
				mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * j), "?? ");
				wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
				mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * xwidth) + j, "?");
				goto do_border;
			}

			/* We have some legimate data to display */
 			byte = bytes[j];
			wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
			mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * j), "%2.2" PRIx8 " ", byte);
			wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
			byte &= 0x7f;
			mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * xwidth) + j, "%c",
				(byte < 32 || byte > 126) ? '.' : byte);
do_border:
			wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
			mvwprintw(mainwin, i, 16 + (HEX_WIDTH * xwidth), " ");
			data_index++;
			if (data_index >= page_size) {
				data_index -= page_size;
				index++;
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
	uint64_t index;

	if ((fd = open(path_mem, O_RDONLY)) < 0)
		return ERR_NO_MEM_INFO;

	for (index = 0; index < mem_info.npages; index++) {
		off_t addr = mem_info.pages[index].addr;
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
	if (view == VIEW_PAGE) {
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		mvwprintw(mainwin, LINES - 1, 0, "Page View, KEY: ");
		wattrset(mainwin, COLOR_PAIR(WHITE_RED));
		wprintw(mainwin, "A");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		wprintw(mainwin, " Anon/File, ");
		wattrset(mainwin, COLOR_PAIR(WHITE_YELLOW));
		wprintw(mainwin, "P");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		wprintw(mainwin, " Present in RAM, ");
		wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
		wprintw(mainwin, "D");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		wprintw(mainwin, " Dirty, ");
		wattrset(mainwin, COLOR_PAIR(WHITE_GREEN));
		wprintw(mainwin, "S");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		wprintw(mainwin, " Swap, ");
		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
		wprintw(mainwin, ".");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		wprintw(mainwin, " not in RAM");
		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE) | A_BOLD);
	} else {
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		mvwprintw(mainwin, LINES-1, 0, "%-*s", COLS, "Memory View");
	}
}

/*
 *  show_help()
 *	show pop-up help info
 */
static inline void show_help(void)
{
	const int x = (COLS - 45) / 2;
	const int y = (LINES - 10) / 2;

	wattrset(mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
	mvwprintw(mainwin, y + 0,  x,
		" HELP (press ? or h to toggle on/off)%6s", "");
	mvwprintw(mainwin, y + 1,  x,
		"%43s", "");
	mvwprintw(mainwin, y + 2,  x,
		" Esc or q   quit%27s", "");
	mvwprintw(mainwin, y + 3,  x,
		" Tab        Toggle page information%8s", "");
	mvwprintw(mainwin, y + 4,  x,
		" Enter      Toggle map/memory views%8s", "");
	mvwprintw(mainwin, y + 5,  x,
		" + or z     Zoom in memory map%13s", "");
	mvwprintw(mainwin, y + 6,  x,
		" - or Z     Zoom out memory map%12s", "");
	mvwprintw(mainwin, y + 7,  x,
		" R or r     Read pages (swap in all pages) ");
	mvwprintw(mainwin, y + 8,  x,
		" V or v     Toggle Virtual Memory Stats    ");
	mvwprintw(mainwin, y + 9,  x,
		" PgUp       Scroll up 1/2 page%13s", "");
	mvwprintw(mainwin, y + 10,  x,
		" PgDown     Scroll Down1/2 page%12s", "");
	mvwprintw(mainwin, y + 11, x,
		" Home       Move cursor back to top%8s", "");
	mvwprintw(mainwin, y + 12, x,
		" Cursor keys move Up/Down/Left/Right%7s", "");
}

/*
 *  update_xwidth()
 *	set the xwidth scale for a specific view v
 *	based on column width and scaling factor for
 *	page or mem (hex) views
 */
static inline void update_xwidth(position_t *position, int v)
{
	static int32_t xwidth_scale[] = {
		1,	/* VIEW_PAGE */
		4	/* VIEW_MEM */
	};

	position[v].xwidth = (COLS - ADDR_OFFSET) / xwidth_scale[v];
}

/*
 *  reset_cursor()
 *	reset to home position
 */
static inline void reset_cursor(
	position_t *p,
	int64_t *data_index,
	int64_t *page_index)
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

	useconds_t udelay = 10000;
	position_t position[2];

	int64_t page_index = 0, prev_page_index;
	int64_t data_index = 0, prev_data_index;

	int32_t tick, ticks = 60, blink = 0, zoom = 1;
	pid_t pid = -1;
	int rc = OK, ret;
	bool do_run = true;

	memset(position, 0, sizeof(position));

	for (;;) {
		int c = getopt(argc, argv, "d:hp:rt:z:");

		if (c == -1)
			break;
		switch (c) {
		case 'd':
			udelay = strtoul(optarg, NULL, 10);
			if (errno) {
				fprintf(stderr, "Invalid delay value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			show_usage();
			exit(EXIT_SUCCESS);
			break;
		case 'p':
			pid = strtol(optarg, NULL, 10);
			if (errno || (pid < 0)) {
				fprintf(stderr, "Invalid pid value\n");
				exit(EXIT_FAILURE);
			}
			opt_flags |= OPT_FLAG_PID;
			break;
		case 'r':
			opt_flags |= OPT_FLAG_READ_ALL_PAGES;
			break;
		case 't':
			ticks = strtol(optarg, NULL, 10);
			if ((ticks < MIN_TICKS) || (ticks > MAX_TICKS)) {
				fprintf(stderr, "Invalid ticks value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'z':
			zoom = strtoul(optarg, NULL, 10);
			if (errno || (zoom < MIN_ZOOM) || (zoom > MAX_ZOOM)) {
				fprintf(stderr, "Invalid zoom value\n");
				exit(EXIT_FAILURE);
			}
			break;
		default:
			show_usage();
			exit(EXIT_FAILURE);
		}
	}
	if (!(opt_flags & OPT_FLAG_PID)) {
		fprintf(stderr, "Must provide process ID with -p option\n");
		exit(EXIT_FAILURE);
	}
	if (geteuid() != 0) {
		fprintf(stderr, "%s requires root privileges to "
			"access memory of pid %d\n", APP_NAME, pid);
		exit(EXIT_FAILURE);
	}
	if (kill(pid, 0) < 0) {
		fprintf(stderr, "No such process %d\n", pid);
		exit(EXIT_FAILURE);
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size == (uint32_t)-1) {
		/* Guess */
		page_size = 4096UL;
	}
	max_pages = ((uint64_t)((size_t)~0)) / page_size;
	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_winch;
	if (sigaction(SIGWINCH, &action, NULL) < 0) {
		fprintf(stderr, "Could not set up window resizing handler\n");
		exit(EXIT_FAILURE);
	}

	snprintf(path_refs, sizeof(path_refs),
		"/proc/%i/clear_refs", pid);
	snprintf(path_pagemap, sizeof(path_pagemap),
		"/proc/%i/pagemap", pid);
	snprintf(path_maps, sizeof(path_maps),
		"/proc/%i/maps", pid);
	snprintf(path_mem, sizeof(path_mem),
		"/proc/%i/mem", pid);
	snprintf(path_status, sizeof(path_status),
		"/proc/%i/status", pid);

	tick = ticks;	/* force immediate page load */
	resized = false;
	initscr();
	start_color();
	cbreak();
	noecho();
	nodelay(stdscr, 1);
	keypad(stdscr, 1);
	curs_set(0);
	mainwin = newwin(LINES, COLS, 0, 0);

	init_pair(WHITE_RED, COLOR_WHITE, COLOR_RED);
	init_pair(WHITE_BLUE, COLOR_WHITE, COLOR_BLUE);
	init_pair(WHITE_YELLOW, COLOR_WHITE, COLOR_YELLOW);
	init_pair(WHITE_CYAN, COLOR_WHITE, COLOR_CYAN);
	init_pair(WHITE_GREEN, COLOR_WHITE, COLOR_GREEN);
	init_pair(WHITE_BLACK, COLOR_WHITE, COLOR_BLACK);
	init_pair(BLACK_WHITE, COLOR_BLACK, COLOR_WHITE);
	init_pair(RED_BLUE, COLOR_RED, COLOR_BLUE);
	init_pair(BLACK_BLACK, COLOR_BLACK, COLOR_BLACK);
	init_pair(BLUE_WHITE, COLOR_BLUE, COLOR_WHITE);

	update_xwidth(position, 0);
	update_xwidth(position, 1);

	do {
		int ch, blink_attrs;
		char curch;
		position_t *p = &position[view];
		uint64_t show_addr;
		float percent;

		/*
		 *  SIGWINCH window resize triggered so
		 *  handle window resizing in ugly way
		 */
		if (resized) {
			uint32_t cursor_index = page_index +
				(p->xpos + (p->ypos * p->xwidth));

			delwin(mainwin);
			endwin();
			refresh();
			clear();

			/* Way too small, give up */
			if ((COLS < 30) || (LINES < 5)) {
				rc = ERR_SMALL_WIN;
				break;
			}
			mainwin = newwin(LINES, COLS, 0, 0);
			wbkgd(mainwin, COLOR_PAIR(RED_BLUE));
			resized = false;

			p->xpos = 0;
			p->ypos = 0;
			page_index = cursor_index;
		}

		/*
		 *  Window getting too small, tell user
		 */
		if ((COLS < 80) || (LINES < 20)) {
			wbkgd(mainwin, COLOR_PAIR(RED_BLUE));
			wattrset(mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
			mvwprintw(mainwin, LINES / 2, (COLS / 2) - 10,
				"[ WINDOW TOO SMALL ]");
			wrefresh(mainwin);
			refresh();
			usleep(udelay);
			continue;	
		}

		update_xwidth(position, view);
		wbkgd(mainwin, COLOR_PAIR(RED_BLUE));

		if ((view == VIEW_PAGE) &&
		    ((rc = read_maps()) < 0))
			break;

		if (auto_zoom) {
			int64_t window_pages = p->xwidth * (LINES - 3);
			zoom = mem_info.npages / window_pages;
			if (zoom > MAX_ZOOM)
				zoom = MAX_ZOOM;
			if (zoom < MIN_ZOOM)
				zoom = MIN_ZOOM;
		}

		if (opt_flags & OPT_FLAG_READ_ALL_PAGES) {
			read_all_pages();
			opt_flags &= ~OPT_FLAG_READ_ALL_PAGES;
		}

		tick++;
		if (tick > ticks) {
			int fd;
			tick = 0;

			fd = open(path_refs, O_RDWR);
			if (fd > -1) {
				ret = write(fd, "4", 1);
				(void)ret;
				(void)close(fd);
			}
		}

		ch = getch();
		show_key();

		blink++;
		if (view == VIEW_MEM) {
			int32_t curxpos = (p->xpos * 3) + ADDR_OFFSET;
			position_t *pc = &position[VIEW_PAGE];
			uint32_t cursor_index = page_index +
				(pc->xpos + (pc->ypos * pc->xwidth));
			percent = 100.0 * cursor_index / mem_info.npages;

			map = mem_info.pages[cursor_index].map;
			show_addr = mem_info.pages[cursor_index].addr +
				data_index + (p->xpos + (p->ypos * p->xwidth));
			if (show_memory(cursor_index, data_index, page_size, p->xwidth) < 0)
				break;

			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(WHITE_BLUE) :
				COLOR_PAIR(BLUE_WHITE));
			wattrset(mainwin, blink_attrs);
			curch = mvwinch(mainwin, p->ypos + 1, curxpos) & A_CHARTEXT;
			mvwprintw(mainwin, p->ypos + 1, curxpos, "%c", curch);

			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(BLACK_WHITE) : COLOR_PAIR(WHITE_BLACK));
			curxpos = ADDR_OFFSET + (p->xwidth * 3) + p->xpos;
			wattrset(mainwin, blink_attrs);
			curch = mvwinch(mainwin, p->ypos + 1, curxpos) & A_CHARTEXT;
			mvwprintw(mainwin, p->ypos + 1, curxpos, "%c", curch);
		} else {
			int32_t curxpos = p->xpos + ADDR_OFFSET;
			uint32_t cursor_index = page_index +
				(p->xpos + (p->ypos * p->xwidth));
			percent = 100.0 * cursor_index / mem_info.npages;

			map = mem_info.pages[cursor_index].map;
			show_addr = mem_info.pages[cursor_index].addr;
			show_pages(cursor_index, page_index, p->xwidth, zoom);
		
			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(BLACK_WHITE) : COLOR_PAIR(WHITE_BLACK));
			wattrset(mainwin, blink_attrs);
			curch = mvwinch(mainwin, p->ypos + 1, curxpos) & A_CHARTEXT;
			mvwprintw(mainwin, p->ypos + 1, curxpos, "%c", curch);
		}
		if (help_view)
			show_help();

		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		if (!map) {
			mvwprintw(mainwin, 0, 0,
				"Pagemon 0x---------------- %4.4s x %-3d ",
				auto_zoom && ((blink & BLINK_MASK))  ? "Auto" : "Zoom", zoom);
			wprintw(mainwin, "---- --:-- %-20.20s", "[Not Mapped]");
		} else {
			mvwprintw(mainwin, 0, 0, "Pagemon 0x%16.16" PRIx64
				" %4.4s x %-3d ", show_addr,
				auto_zoom && ((blink & BLINK_MASK)) ? "Auto" : "Zoom", zoom);
			wprintw(mainwin, "%s %s %-20.20s",
				map->attr, map->dev,
				map->name[0] == '\0' ?
					"[Anonymous]" : basename(map->name));
		}
		mvwprintw(mainwin, 0, COLS - 8, " %6.1f%%", percent * zoom);

		wrefresh(mainwin);
		refresh();

		prev_page_index = page_index;
		prev_data_index = data_index;
		p->xpos_prev = p->xpos;
		p->ypos_prev = p->ypos;

		switch (ch) {
		case 27:	/* ESC */
		case 'q':
		case 'Q':
			/* Quit */
			do_run = false;
			break;
		case '\t':
			/* Toggle Tab view */
			tab_view = !tab_view;
			break;
		case 'v':
		case 'V':
			/* Toggle VM stats view */
			vm_view = !vm_view;
			break;
		case '?':
		case 'h':
			/* Toggle Help */
			help_view = !help_view;
			break;
		case 'r':
		case 'R':
			read_all_pages();
			break;
		case 'a':
		case 'A':
			/* Toggle auto zoom */
			auto_zoom = !auto_zoom;
			break;
		case '\n':
			/* Toggle MAP / MEMORY views */
			view ^= 1;
			p = &position[view];
			blink = 0;
			break;
		case '+':
		case 'z':
			/* Zoom in */
			if (view == VIEW_PAGE) {
				zoom++ ;
				if (zoom > MAX_ZOOM)
					zoom = MAX_ZOOM;
				reset_cursor(p, &data_index, &page_index);
			}
			break;
		case '-':
		case 'Z':
			/* Zoom out */
			if (view == VIEW_PAGE) {
				zoom--;
				if (zoom < MIN_ZOOM)
					zoom = MIN_ZOOM;
				reset_cursor(p, &data_index, &page_index);
			}
			break;
		case 't':
			/* Tick increase */
			ticks++;
			if (ticks > MAX_TICKS)
				ticks = MAX_TICKS;
			break;
		case 'T':
			/* Tick decrease */
			ticks--;
			if (ticks < MIN_TICKS)
				ticks = MIN_TICKS;
			break;
		case KEY_DOWN:
			blink = 0;
			if (view == VIEW_PAGE)
				data_index = 0;
			p->ypos++;
			break;
		case KEY_UP:
			blink = 0;
			if (view == VIEW_PAGE)
				data_index = 0;
			p->ypos--;
			break;
		case KEY_LEFT:
			blink = 0;
			if (view == VIEW_PAGE)
				data_index = 0;
			p->xpos--;
			break;
		case KEY_RIGHT:
			blink = 0;
			if (view == VIEW_PAGE)
				data_index = 0;
			p->xpos++;
			break;
		case KEY_NPAGE:
			if (view == VIEW_PAGE)
				data_index = 0;
			blink = 0;
			p->ypos += (LINES - 2) / 2;
			break;
		case KEY_PPAGE:
			if (view == VIEW_PAGE)
				data_index = 0;
			blink = 0;
			p->ypos -= (LINES - 2) / 2;
			break;
		case KEY_HOME:
			reset_cursor(p, &data_index, &page_index);
			break;
		}

		position[VIEW_PAGE].ypos_max =
			(((mem_info.npages - page_index) / zoom) - p->xpos) /
			position[0].xwidth;
		position[VIEW_MEM].ypos_max = LINES - 2;

		if (p->xpos >= p->xwidth) {
			p->xpos = 0;
			p->ypos++;
		}
		if (p->xpos < 0) {
			p->xpos = p->xwidth - 1;
			p->ypos--;
		}

		/*
		 *  Handling yposition overflow / underflow
		 *  is non-trivial as we need to consider
		 *  different views and how to handle the
		 *  scroll data and page index offsets
		 */
		if (view == VIEW_MEM) {
			if (p->ypos > LINES - 3) {
				data_index += p->xwidth *
					(p->ypos - (LINES - 3));
				p->ypos = LINES - 3;
				if (data_index >= page_size) {
					data_index -= page_size;
					page_index++;
				}
			}
			if (p->ypos < 0) {
				data_index -= p->xwidth * (-p->ypos);
				p->ypos = 0;
				if (data_index < 0) {
					data_index += page_size;
					page_index--;
				}
			}
		} else {
			if (p->ypos > LINES - 3) {
				page_index += zoom * p->xwidth *
					(p->ypos - (LINES - 3));
				p->ypos = LINES - 3;
			}
			if (p->ypos < 0) {
				page_index -= zoom * p->xwidth * (-p->ypos);
				p->ypos = 0;
			}
		}
		if (page_index < 0) {
			page_index = 0;	
			data_index = 0;
			p->ypos = 0;
		}
		if (view == VIEW_MEM) {
			position_t *pc = &position[VIEW_PAGE];
			uint32_t cursor_index = page_index +
				(pc->xpos + (pc->ypos * pc->xwidth));
			uint64_t addr = mem_info.pages[cursor_index].addr +
				data_index + (p->xpos + (p->ypos * p->xwidth));
			if (addr >= mem_info.last_addr) {
				page_index = prev_page_index;
				data_index = prev_data_index;
				p->xpos = p->xpos_prev;
				p->ypos = p->ypos_prev;
			}
		} else {
			if ((uint64_t)page_index + (zoom * (p->xpos +
			    (p->ypos * p->xwidth))) >= mem_info.npages) {
				int64_t zoom_xwidth = zoom * p->xwidth;
				int64_t lines =
					((zoom_xwidth - 1) + mem_info.npages) / zoom_xwidth;
				uint64_t npages = 
					(zoom_xwidth * lines);
				int64_t diff = (npages - mem_info.npages) / zoom;
				int64_t last = p->xwidth - diff;

				if (lines < LINES) {
					p->ypos = lines - 1;
					page_index = 0;
				} else {
					p->ypos = LINES - 3;
					page_index = (lines - (LINES - 2)) * zoom_xwidth;
				}
				if (p->xpos > last - 1)
					p->xpos = last - 1;
			}
		}
		if (view == VIEW_PAGE) {
			free(mem_info.pages);
			mem_info.npages = 0;
		}
		usleep(udelay);
	} while (do_run);

	endwin();

	ret = EXIT_FAILURE;
	switch (rc) {
	case OK:
		ret = EXIT_SUCCESS;
		break;
	case ERR_NO_MAP_INFO:
		fprintf(stderr, "Cannot access memory maps for PID %d\n", pid);
		break;
	case ERR_NO_MEM_INFO:
		fprintf(stderr, "Cannot access memory for PID %d\n", pid);
		break;
	case ERR_SMALL_WIN:
		fprintf(stderr, "Window too small\n");
		break;
	case ERR_ALLOC_NOMEM:
		fprintf(stderr, "Memory allocation failed\n");
		break;
	case ERR_TOO_MANY_PAGES:
		fprintf(stderr, "Too many pages in process for %s\n", APP_NAME);
		printf("%" PRIu64 " vs %" PRIu64 "\n",
			mem_info.npages , max_pages);
		break;
	default:
		fprintf(stderr, "Unknown failure (%d)\n", rc);
		break;
	}
	exit(ret);
}
