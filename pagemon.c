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
 * Copyright (C) Colin Ian King
 * colin.i.king@gmail.com
 */
#include <stdio.h>
#include <stdlib.h>
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

WINDOW *mainwin;
static bool resized;

#define APP_NAME		"pagemon"
#define MAX_MMAPS		(65536)
#define PAGE_SIZE		(4096ULL)

#define ADDR_OFFSET		(17)
#define HEX_WIDTH		(3)

#define VIEW_PAGE		(0)
#define VIEW_MEM		(1)

#define OK			(0)
#define ERR_NO_MAP_INFO		(-1)
#define ERR_NO_MEM_INFO		(-2)
#define ERR_SMALL_WIN		(-3)

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

static mem_info_t mem_info;
static bool tab_view = false;
static bool help_view = false;
static uint8_t view = VIEW_PAGE;
static uint8_t opt_flags;

/*
 *  read_maps()
 *	read memory maps for a specifc process
 */
static int read_maps(const char *path_maps)
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

		mem_info.maps[n].name[0] = '\0';
		ret = sscanf(buffer, "%" SCNx64 "-%" SCNx64 " %5s %*s %6s %*d %s",
			&mem_info.maps[n].begin,
			&mem_info.maps[n].end,
			mem_info.maps[n].attr,
			mem_info.maps[n].dev,
			mem_info.maps[n].name);
		if (ret != 5)
			continue;

		/* Simple sanity check */
		if (mem_info.maps[n].end < mem_info.maps[n].begin)
			continue;

		if (last_addr < mem_info.maps[n].end)
			last_addr = mem_info.maps[n].end;
		mem_info.npages += (mem_info.maps[n].end -
				    mem_info.maps[n].begin) / PAGE_SIZE;
		n++;
		if (n >= MAX_MMAPS)
			break;
	}
	fclose(fp);

	mem_info.nmaps = n;
	mem_info.pages = page = calloc(mem_info.npages, sizeof(page_t));
	mem_info.last_addr = last_addr;

	for (i = 0; i < mem_info.nmaps; i++) {
		uint64_t count = (mem_info.maps[i].end -
				  mem_info.maps[i].begin) / PAGE_SIZE;
		uint64_t addr = mem_info.maps[i].begin;

		for (j = 0; j < count; j++) {
			page->addr = addr;
			page->map = &mem_info.maps[i];
			page->index = i;
			addr += PAGE_SIZE;
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

static void show_page_bits(
	const int fd,
	map_t *map,
	const uint32_t page_size,
	const int64_t index)
{
	uint64_t info;
	off_t offset;

	wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
	mvwprintw(mainwin, 3, 4,
		" Page:   0x%16.16" PRIx64 "                  ",
		mem_info.pages[index].addr);
	mvwprintw(mainwin, 4, 4,
		" Map:    0x%16.16" PRIx64 "-%16.16" PRIx64 " ",
		map->begin, map->end - 1);
	mvwprintw(mainwin, 5, 4,
		" Device: %5.5s                               ",
		map->dev);
	mvwprintw(mainwin, 6, 4,
		" Prot:   %4.4s                                ",
		map->attr);
	mvwprintw(mainwin, 6, 4,
		" File:   %-20.20s ", basename(map->name));

	offset = sizeof(uint64_t) * (mem_info.pages[index].addr / page_size);
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
		return;
	if (read(fd, &info, sizeof(info)) != sizeof(info))
		return;

	mvwprintw(mainwin, 7, 4,
		" Flag:   0x%16.16" PRIx64 "                  ", info);
	if (info & PAGE_SWAPPED) {
		mvwprintw(mainwin, 8, 4,
			"   Swap Type:           0x%2.2" PRIx64 "                 ",
			info & 0x1f);
		mvwprintw(mainwin, 9, 4,
			"   Swap Offset:         0x%16.16" PRIx64 "   ",
			(info & 0x00ffffffffffffff) >> 5);
	} else {
		mvwprintw(mainwin, 8, 4,
			"                                             ");
		mvwprintw(mainwin, 9, 4,
			"   Page Frame Number:   0x%16.16" PRIx64 "   ",
			info & 0x00ffffffffffffff);
	}
	mvwprintw(mainwin, 10, 4,
		"   Soft-dirty PTE:      %3s                  ",
		(info & PAGE_PTE_SOFT_DIRTY) ? "Yes" : "No ");
	mvwprintw(mainwin, 11, 4,
		"   Exlusively Mapped:   %3s                  ",
		(info & PAGE_EXCLUSIVE_MAPPED) ? "Yes" : "No ");
	mvwprintw(mainwin, 12, 4,
		"   File or Shared Anon: %3s                  ",
		(info & PAGE_FILE_SHARED_ANON) ? "Yes" : "No ");
	mvwprintw(mainwin, 13, 4,
		"   Present in Swap:     %3s                  ",
		(info & PAGE_SWAPPED) ? "Yes" : "No ");
	mvwprintw(mainwin, 14, 4,
		"   Present in RAM:      %3s                  ",
		(info & PAGE_PRESENT) ? "Yes" : "No ");
}


/*
 *  show_pages()
 *	show page mapping
 */
static int show_pages(
	const char *path_pagemap,
	const int32_t cursor_index,
	const int32_t page_index,
	const uint32_t page_size,
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

			if (index >= mem_info.npages) {
				wattrset(mainwin, COLOR_PAIR(BLACK_BLACK));
				state = '~';
			} else {
				uint64_t addr = mem_info.pages[index].addr;
				off_t offset = sizeof(uint64_t) *
					       (addr / page_size);

				if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
					break;
				if (read(fd, &info, sizeof(info)) < 0)
					break;

				wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));

				if (info & PAGE_PRESENT) {
					wattrset(mainwin, COLOR_PAIR(WHITE_YELLOW));
					state = 'P';
				}
				if (info & PAGE_SWAPPED) {
					wattrset(mainwin, COLOR_PAIR(WHITE_GREEN));
					state = 'S';
				}
				if (info & PAGE_FILE_SHARED_ANON) {
					wattrset(mainwin, COLOR_PAIR(WHITE_RED));
					state = 'M';
				}
				if (info & PAGE_PTE_SOFT_DIRTY) {
					wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
					state = 'D';
				}
			
				index += zoom;
			}
			mvwprintw(mainwin, i, ADDR_OFFSET + j, "%c", state);
		}
	}

	wattrset(mainwin, A_NORMAL);

	if (map && tab_view)
		show_page_bits(fd, map, page_size, cursor_index);

	(void)close(fd);
	return 0;
}

/*
 *  show_memory()
 *	show memory contents
 */
static int show_memory(
	const char *path_mem,
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
			uint8_t byte = bytes[j];

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
			byte &= 0x7f;
			wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
			mvwprintw(mainwin, i, ADDR_OFFSET + (HEX_WIDTH * j), "%2.2" PRIx8 " ", byte);
			wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
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
static int read_all_pages(const char *path_mem)
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
		mvwprintw(mainwin, LINES - 1, 0, "KEY: ");
		wattrset(mainwin, COLOR_PAIR(WHITE_RED));
		wprintw(mainwin, "A");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		wprintw(mainwin, " Mapped anon/file, ");
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
		mvwprintw(mainwin, LINES-1, 0, "%-*s", COLS, "");
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
		" HELP (press ? or h to toggle on/off)      ");
	mvwprintw(mainwin, y + 1,  x,
		"                                           ");
	mvwprintw(mainwin, y + 2,  x,
		" Esc or q   quit                           ");
	mvwprintw(mainwin, y + 3,  x,
		" Tab        Toggle page information        ");
	mvwprintw(mainwin, y + 4,  x,
		" Enter      Toggle map/memory views        ");
	mvwprintw(mainwin, y + 5,  x,
		" + or z     Zoom in memory map             ");
	mvwprintw(mainwin, y + 6,  x,
		" - or Z     Zoom out memory map            ");
	mvwprintw(mainwin, y + 7,  x,
		" R or r     Read pages (swap in all pages) ");
	mvwprintw(mainwin, y + 8,  x,
		" PgUp       Scroll up 1/2 page             ");
	mvwprintw(mainwin, y + 9,  x,
		" PgDown     Scroll Down1/2 page            ");
	mvwprintw(mainwin, y + 10, x,
		" Home       Move cursor back to top        ");
	mvwprintw(mainwin, y + 11, x,
		" Cursor keys move Up/Down/Left/Right       ");
}

static inline void update_xwidth(position_t *position, int v)
{
	static int32_t xwidth_scale[] = {
		1,	/* VIEW_PAGE */
		4	/* VIEW_MEM */
	};

	position[v].xwidth = (COLS - ADDR_OFFSET) / xwidth_scale[view];
}

int main(int argc, char **argv)
{
	struct sigaction action;

	char path_refs[PATH_MAX];
	char path_pagemap[PATH_MAX];
	char path_maps[PATH_MAX];
	char path_mem[PATH_MAX];

	map_t *map;

	useconds_t udelay = 10000;
	position_t position[2];

	int64_t page_index = 0, prev_page_index;
	int64_t data_index = 0, prev_data_index;

	int32_t tick, ticks = 60, blink = 0, zoom = 1;
	uint32_t page_size = PAGE_SIZE;

	pid_t pid = -1;
	int rc = OK;
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
			if ((ticks < 1) || (ticks > 1000)) {
				fprintf(stderr, "Invalid ticks value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'z':
			zoom = strtoul(optarg, NULL, 10);
			if (errno || (zoom < 1) || (zoom > 999)) {
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

		/*
		 *  SIGWINCH window resize triggered so
		 *  handle window resizing in ugly way
		 */
		if (resized) {
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
		    ((rc = read_maps(path_maps)) < 0))
			break;

		if (opt_flags & OPT_FLAG_READ_ALL_PAGES) {
			read_all_pages(path_mem);
			opt_flags &= ~OPT_FLAG_READ_ALL_PAGES;
		}

		tick++;
		if (tick > ticks) {
			int fd;
			tick = 0;

			fd = open(path_refs, O_RDWR);
			if (fd > -1) {
				int ret = write(fd, "4", 1);
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

			map = mem_info.pages[cursor_index].map;
			show_addr = mem_info.pages[cursor_index].addr +
				data_index + (p->xpos + (p->ypos * p->xwidth));
			if (show_memory(path_mem, cursor_index, data_index, page_size, p->xwidth) < 0)
				break;

			blink_attrs = A_BOLD | ((blink & 0x20) ?
				COLOR_PAIR(WHITE_BLUE) :
				COLOR_PAIR(BLUE_WHITE));
			wattrset(mainwin, blink_attrs);
			curch = mvwinch(mainwin, p->ypos + 1, curxpos) & A_CHARTEXT;
			mvwprintw(mainwin, p->ypos + 1, curxpos, "%c", curch);

			blink_attrs = A_BOLD | ((blink & 0x20) ?
				COLOR_PAIR(BLACK_WHITE) : COLOR_PAIR(WHITE_BLACK));
			curxpos = ADDR_OFFSET + (p->xwidth * 3) + p->xpos;
			wattrset(mainwin, blink_attrs);
			curch = mvwinch(mainwin, p->ypos + 1, curxpos) & A_CHARTEXT;
			mvwprintw(mainwin, p->ypos + 1, curxpos, "%c", curch);
		} else {
			int32_t curxpos = p->xpos + ADDR_OFFSET;
			uint32_t cursor_index = page_index +
				(p->xpos + (p->ypos * p->xwidth));

			map = mem_info.pages[cursor_index].map;
			show_addr = mem_info.pages[cursor_index].addr;
			show_pages(path_pagemap, cursor_index, page_index, page_size, p->xwidth, zoom);
		
			blink_attrs = A_BOLD | ((blink & 0x20) ?
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
				"Pagemon 0x---------------- Zoom x %-3d ",
				zoom);
			wprintw(mainwin, "---- --:-- %-20.20s", "[Not Mapped]");
		} else {
			mvwprintw(mainwin, 0, 0, "Pagemon 0x%16.16" PRIx64
				" Zoom x %-3d ", show_addr, zoom);
			wprintw(mainwin, "%s %s %-20.20s",
				map->attr, map->dev,
				map->name[0] == '\0' ?
					"[Anonymous]" : basename(map->name));
		}

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
		case '?':
		case 'h':
			/* Toggle Help */
			help_view = !help_view;
			break;
		case 'r':
		case 'R':
			read_all_pages(path_mem);
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
				if (zoom > 999)
					zoom = 999;
			}
			p->xpos = 0;
			p->ypos = 0;
			data_index = 0;
			page_index = 0;
			break;
		case '-':
		case 'Z':
			/* Zoom out */
			if (view == VIEW_PAGE) {
				zoom--;
				if (zoom < 1)
					zoom = 1;
			}
			p->xpos = 0;
			p->ypos = 0;
			data_index = 0;
			page_index = 0;
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
			p->xpos = 0;
			p->ypos = 0;
			data_index = 0;
			page_index = 0;
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
				page_index = prev_page_index;
				p->xpos = p->xpos_prev;
				p->ypos = p->ypos_prev;
			}
		}

		if (view == VIEW_PAGE) {
			free(mem_info.pages);
			mem_info.npages = 0;
		}

		usleep(udelay);
	} while (do_run);

	endwin();

	switch (rc) {
	case OK:
		rc = EXIT_SUCCESS;
		break;
	case ERR_NO_MAP_INFO:
		rc = EXIT_FAILURE;
		fprintf(stderr, "Cannot access memory maps for PID %d\n", pid);
		break;
	case ERR_NO_MEM_INFO:
		rc = EXIT_FAILURE;
		fprintf(stderr, "Cannot access memory for PID %d\n", pid);
		break;
	case ERR_SMALL_WIN:
		rc = EXIT_FAILURE;
		fprintf(stderr, "Window too small\n");
		break;
	default:
		rc = EXIT_FAILURE;
		fprintf(stderr, "Unknown failure (%d)\n", rc);
		break;
	}

	exit(rc);
}
