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

#define APP_NAME	"pagemon"
#define MAX_MMAPS	(65536)
#define PAGE_SIZE	(4096)

#define VIEW_PAGE	(0)
#define VIEW_MEM	(1)

#define OK		(0)
#define ERR_NO_MAP_INFO	(-1)
#define ERR_NO_MEM_INFO	(-2)
#define ERR_SMALL_WIN	(-3)

enum {
	WHITE_RED = 1,
	WHITE_BLUE,
	WHITE_YELLOW,
	WHITE_CYAN,
	WHITE_GREEN,
	WHITE_BLACK,
	BLACK_WHITE,
	CYAN_BLUE,
	RED_BLUE,
	YELLOW_BLUE,
	BLACK_GREEN,
	BLACK_YELLOW,
	YELLOW_RED,
	YELLOW_BLACK,
	BLACK_BLACK,
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
	int64_t npages;			/* Number of pages */
} mem_info_t;

typedef struct {
	int32_t xpos;			/* Cursor x position */
	int32_t ypos;			/* Cursor y position */
	int32_t xpos_prev;		/* Previous x position */
	int32_t ypos_prev;		/* Previous y position */
	int32_t ypos_max;		/* Max y position */
	int32_t xwidth;			/* Width of x */
} position_t;

static uint32_t view = VIEW_PAGE;
static bool tab_view = false;
static bool help_view = false;
static mem_info_t mem_info;

static int read_maps(
	const char *filename)
{
	FILE *fp;
	uint32_t i, j, n = 0;
	char buffer[4096];
	page_t *page;

	memset(&mem_info, 0, sizeof(mem_info));

	fp = fopen(filename, "r");
	if (fp == NULL)
		return ERR_NO_MAP_INFO;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		mem_info.maps[n].name[0] = '\0';
		sscanf(buffer, "%" SCNx64 "-%" SCNx64 " %5s %*s %6s %*d %s",
			&mem_info.maps[n].begin,
			&mem_info.maps[n].end,
			mem_info.maps[n].attr,
			mem_info.maps[n].dev,
			mem_info.maps[n].name);

		mem_info.npages += (mem_info.maps[n].end - mem_info.maps[n].begin) / PAGE_SIZE;
		n++;
		if (n >= MAX_MMAPS)
			break;
	}
	fclose(fp);

	mem_info.nmaps = n;
	mem_info.pages = page = calloc(mem_info.npages, sizeof(page_t));

	for (i = 0; i < mem_info.nmaps; i++) {
		uint64_t count = (mem_info.maps[i].end - mem_info.maps[i].begin) / PAGE_SIZE;
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

static void handle_winch(int sig)
{
	(void)sig;

	resized = true;
}

static void show_usage(void)
{
	printf(APP_NAME ", version " VERSION "\n\n"
		"Usage: " APP_NAME " [options] pid\n"
		" -h help\n");
}

static int show_pages(
	const char *path_pagemap,
	const int64_t page_index,
	const uint32_t page_size,
	const int32_t xwidth,
	const int32_t zoom)
{
	int32_t i;
	int64_t index = page_index;
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
			mvwprintw(mainwin, i, 0, "%16.16lx ", addr);
		}

		for (j = 0; j < xwidth; j++) {
			char state = '.';

			if (index >= mem_info.npages) {
				wattrset(mainwin, COLOR_PAIR(BLACK_BLACK));
				state = '~';
			} else {
				uint64_t addr = mem_info.pages[index].addr;
				lseek(fd, sizeof(uint64_t) * (addr / page_size), SEEK_SET);
				if (read(fd, &info, sizeof(info)) < 0)
					break;

				wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));

				if (info & ((uint64_t)1 << 63)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_YELLOW));
					state = 'R';
				}
				if (info & ((uint64_t)1 << 62)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_GREEN));
					state = 'S';
				}
				if (info & ((uint64_t)1 << 61)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_RED));
					state = 'M';
				}
				if (info & ((uint64_t)1 << 55)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
					state = 'D';
				}
			
				index += zoom;
			}
			mvwprintw(mainwin, i, 17 + j, "%c", state);
		}
	}

	wattrset(mainwin, A_NORMAL);
	if (map && tab_view) {
		uint64_t info;

		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		mvwprintw(mainwin, 3, 4, " Page:   0x%16.16" PRIx64 "                  ",
			mem_info.pages[index].addr);
		mvwprintw(mainwin, 4, 4, " Map:    0x%16.16" PRIx64 "-%16.16" PRIx64 " ",
			map->begin, map->end - 1);
		mvwprintw(mainwin, 5, 4, " Device: %5.5s                               ", map->dev);
		mvwprintw(mainwin, 6, 4, " Prot:   %4.4s                                ", map->attr);
		mvwprintw(mainwin, 6, 4, " File:   %-20.20s ", basename(map->name));
		lseek(fd, sizeof(uint64_t) * (mem_info.pages[index].addr / page_size), SEEK_SET);
		if (read(fd, &info, sizeof(info)) > 0) {
		mvwprintw(mainwin, 7, 4, " Flag:   0x%16.16lx                  ", info);
		mvwprintw(mainwin, 8, 4, "   Present in RAM:      %3s                  ", 
			(info & ((uint64_t)1 << 63)) ? "Yes" : "No ");
		mvwprintw(mainwin, 9, 4, "   Present in Swap:     %3s                  ", 
			(info & ((uint64_t)1 << 62)) ? "Yes" : "No ");
		mvwprintw(mainwin, 10, 4, "   File or Shared Anon: %3s                  ", 
			(info & ((uint64_t)1 << 61)) ? "Yes" : "No ");
		mvwprintw(mainwin, 11, 4, "   Soft-dirty PTE:      %3s                  ", 
			(info & ((uint64_t)1 << 55)) ? "Yes" : "No ");
		}
	}

	(void)close(fd);

	return 0;
}

static int show_memory(
	const char *path_mem,
	const int64_t page_index,
	int64_t data_index,
	const uint32_t page_size,
	const int32_t xwidth)
{
	int32_t i;
	int64_t index = page_index;
	int fd;
	off_t addr;

	if ((fd = open(path_mem, O_RDONLY)) < 0)
		return ERR_NO_MEM_INFO;

	for (i = 1; i < LINES - 1; i++) {
		int32_t j;
		addr = mem_info.pages[index].addr + data_index;

		if (index >= mem_info.npages) {
			wattrset(mainwin, COLOR_PAIR(BLACK_BLACK));
			mvwprintw(mainwin, i, 0, "---------------- ");
		} else {
			wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
			mvwprintw(mainwin, i, 0, "%16.16lx ", addr);
		}
		mvwprintw(mainwin, i, COLS - 3, "   ", addr);

		for (j = 0; j < xwidth; j++) {
			if (index >= mem_info.npages) {
				wattrset(mainwin, COLOR_PAIR(BLACK_BLACK));
			} else {
				uint8_t byte;
				addr = mem_info.pages[index].addr + data_index;

				lseek(fd, addr, SEEK_SET);
				if (read(fd, &byte, sizeof(byte)) < 0) {
					wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
					mvwprintw(mainwin, i, 17 + j * 3, "?? ");
					mvwprintw(mainwin, i, 17 + (3 * xwidth) + j, "?");
				} else {
					wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
					mvwprintw(mainwin, i, 17 + j * 3, "%2.2x ", byte);
					byte &= 0x7f;
	
					mvwprintw(mainwin, i, 17 + (3 * xwidth) + j, "%c",
						(byte < 32 || byte > 126) ? '.' : byte);
				}
			}
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

int main(int argc, char **argv)
{
	uint32_t page_size = PAGE_SIZE;
	int64_t page_index = 0, prev_page_index;
	int64_t data_index = 0;
	bool do_run = true;
	pid_t pid = -1;
	char path_refs[PATH_MAX];
	char path_pagemap[PATH_MAX];
	char path_maps[PATH_MAX];
	char path_mem[PATH_MAX];
	map_t *map;
	int tick = 0;
	int blink = 0;
	int rc = OK;
	int32_t zoom = 1;
	position_t position[2];

	memset(position, 0, sizeof(position));

	for (;;) {
		int c = getopt(argc, argv, "h");

		if (c == -1)
			break;
		switch (c) {
		case 'h':
			break;
		default:
			show_usage();
			exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		pid = strtol(argv[optind], NULL, 10);
		if (errno) {
			fprintf(stderr, "Invalid pid value\n");
			exit(EXIT_FAILURE);
		}
	} else {
		show_usage();
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
	init_pair(CYAN_BLUE, COLOR_CYAN, COLOR_BLUE);
	init_pair(RED_BLUE, COLOR_RED, COLOR_BLUE);
	init_pair(YELLOW_BLUE, COLOR_YELLOW, COLOR_BLUE);
	init_pair(BLACK_GREEN, COLOR_BLACK, COLOR_GREEN);
	init_pair(BLACK_YELLOW, COLOR_BLACK, COLOR_YELLOW);
	init_pair(YELLOW_RED, COLOR_YELLOW, COLOR_RED);
	init_pair(YELLOW_BLACK, COLOR_YELLOW, COLOR_BLACK);
	init_pair(BLACK_BLACK, COLOR_BLACK, COLOR_BLACK);

	signal(SIGWINCH, handle_winch);

	do {
		int ch;
		char curch;
		position_t *p = &position[view];
		uint64_t show_addr;
		int32_t curxpos;

		if (resized) {
			delwin(mainwin);
			endwin();
			refresh();
			clear();
			if ((COLS < 30) || (LINES < 5)) {
				rc = ERR_SMALL_WIN;
				break;
			}
			mainwin = newwin(LINES, COLS, 0, 0);
			resized = false;
		}

		if ((COLS < 80) || (LINES < 20)) {
			wbkgd(mainwin, COLOR_PAIR(RED_BLUE));
			wattrset(mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
			mvwprintw(mainwin, LINES / 2, (COLS / 2) - 10, "[ WINDOW TOO SMALL ]");
			wrefresh(mainwin);
			refresh();
			usleep(10000);
			continue;	
		}

		if ((view == VIEW_PAGE) &&
		    ((rc = read_maps(path_maps)) < 0))
			break;

		position[VIEW_PAGE].xwidth = COLS - 17;
		position[VIEW_MEM].xwidth = (COLS - 17) / 4;

		wbkgd(mainwin, COLOR_PAIR(RED_BLUE));

		tick++;
		if (tick > 10) {
			int fd, ret;
			tick = 0;

			fd = open(path_refs, O_RDWR);
			if (fd > -1) {
				ret = write(fd, "4", 1);
				(void)ret;
				(void)close(fd);
			}
		}

		ch = getch();
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		mvwprintw(mainwin, LINES - 1, 0, "KEY: ");
		wattrset(mainwin, COLOR_PAIR(WHITE_RED));
		wprintw(mainwin, "A");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " Mapped anon/file, ");
		wattrset(mainwin, COLOR_PAIR(WHITE_YELLOW));
		wprintw(mainwin, "R");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " in RAM, ");
		wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
		wprintw(mainwin, "D");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " Dirty, ");
		wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
		wprintw(mainwin, "S");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " Swap, ");
		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
		wprintw(mainwin, ".");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " not in RAM");

		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE) | A_BOLD);

		blink++;
		if (view == VIEW_MEM) {
			position_t *pc = &position[VIEW_PAGE];
			uint32_t index = page_index + (pc->xpos + (pc->ypos * pc->xwidth));
			map = mem_info.pages[index].map;
			show_addr = mem_info.pages[index].addr + data_index + (p->xpos + (p->ypos * p->xwidth));
			if (show_memory(path_mem, index, data_index, page_size, p->xwidth) < 0)
				break;
			curxpos = (p->xpos * 3) + 17;
		} else {
			uint32_t index = page_index + (p->xpos + (p->ypos * p->xwidth));
			map = mem_info.pages[index].map;
			show_addr = mem_info.pages[index].addr;
			show_pages(path_pagemap, page_index, page_size, p->xwidth, zoom);
			curxpos = p->xpos + 17;
		}
		wattrset(mainwin, A_BOLD | ((blink & 0x20) ? COLOR_PAIR(BLACK_WHITE) : COLOR_PAIR(WHITE_BLACK)));
		curch = mvwinch(mainwin, p->ypos + 1, curxpos) & A_CHARTEXT;
		mvwprintw(mainwin, p->ypos + 1, curxpos, "%c", curch);

		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		if (!map) {
			mvwprintw(mainwin, 0, 0, "Pagemon 0x---------------- Zoom x %-3d ", zoom);
			wprintw(mainwin, "---- --:-- %-20.20s", "[Not Mapped]");
		} else {
			mvwprintw(mainwin, 0, 0, "Pagemon 0x%16.16" PRIx64 " Zoom x %-3d ", show_addr, zoom);
			wprintw(mainwin, "%s %s %-20.20s",
				map->attr, map->dev, map->name[0] == '\0' ?  "[Anonymous]" : basename(map->name));
		}
		if (help_view) {
			wattrset(mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
			int x = (COLS - 40) / 2;
			int y = (LINES - 10) / 2;
			mvwprintw(mainwin, y + 0, x, " HELP (press ? or h to toggle on/off)  ");
			mvwprintw(mainwin, y + 1, x, "                                       ");
			mvwprintw(mainwin, y + 2, x, " Esc or q   quit                       ");
			mvwprintw(mainwin, y + 3, x, " Tab        Toggle page information    ");
			mvwprintw(mainwin, y + 4, x, " Enter      Toggle map/memory views    ");
			mvwprintw(mainwin, y + 5, x, " + or z     Zoom in memory map         ");
			mvwprintw(mainwin, y + 6, x, " - or Z     Zoom out memory map        ");
			mvwprintw(mainwin, y + 7, x, " PgUp       Scroll up 1/2 page         ");
			mvwprintw(mainwin, y + 8, x, " PgDown     Scroll Down1/2 page        ");
			mvwprintw(mainwin, y + 9, x, " Cursor keys move Up/Down/Left/Right   ");
		}

		wrefresh(mainwin);
		refresh();

		prev_page_index = page_index;
		p->xpos_prev = p->xpos;
		p->ypos_prev = p->ypos;

		switch (ch) {
		case 27:	/* ESC */
		case 'q':
		case 'Q':
			do_run = false;
			break;
		case '\t':
			tab_view = !tab_view;
			break;
		case '?':
		case 'h':
			help_view = !help_view;
			break;
		case '\n':
			view ^= 1;
			p = &position[view];
			blink = 0;
			break;
		case '+':
		case 'z':
			if (view == VIEW_PAGE) {
				zoom++ ;
				if (zoom > 999)
					zoom = 999;
			}
			break;
		case '-':
		case 'Z':
			if (view == VIEW_PAGE) {
				zoom--;
				if (zoom < 1)
					zoom = 1;
			}
			break;
		case KEY_DOWN:
			blink = 0;
			p->ypos++;
			break;
		case KEY_UP:
			blink = 0;
			p->ypos--;
			break;
		case KEY_LEFT:
			blink = 0;
			p->xpos--;
			break;
		case KEY_RIGHT:
			blink = 0;
			p->xpos++;
			break;
		case KEY_NPAGE:
			blink = 0;
			p->ypos += (LINES - 2) / 2;
			break;
		case KEY_PPAGE:
			blink = 0;
			p->ypos -= (LINES - 2) / 2;
			break;
		}

		position[VIEW_PAGE].ypos_max =
			(((mem_info.npages - page_index) / zoom) - p->xpos) / position[0].xwidth;
		position[VIEW_MEM].ypos_max = LINES - 2;

		if (p->xpos >= p->xwidth) {
			p->xpos = 0;
			p->ypos++;
		}
		if (p->xpos < 0) {
			p->xpos = p->xwidth - 1;
			p->ypos--;
		}
		if (p->ypos > p->ypos_max)
			p->ypos = p->ypos_max;

		if (view == VIEW_MEM) {
			if (p->ypos > LINES - 3) {
				data_index += p->xwidth * (p->ypos - (LINES - 3));
				p->ypos = LINES - 3;
				if (data_index > page_size) {
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
			data_index = 0;
			if (p->ypos > LINES - 3) {
				page_index += zoom * p->xwidth * (p->ypos - (LINES - 3));
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
		} else {
			if (page_index + zoom * (p->xpos + (p->ypos * p->xwidth)) >= mem_info.npages) {
				page_index = prev_page_index;
				p->xpos = p->xpos_prev;
				p->ypos = p->ypos_prev;
			}
		}

		if (view == VIEW_PAGE) {
			free(mem_info.pages);
			mem_info.npages = 0;
		}

		usleep(10000);
	} while (do_run);

	endwin();

	switch (rc) {
	case OK:
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
	default:
		break;
	}

	exit(EXIT_SUCCESS);
}
