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
#define MAX_MMAPS	(8192)

enum {
	WHITE_RED = 1,
	WHITE_BLUE,
	WHITE_YELLOW,
	WHITE_CYAN,
	BLACK_WHITE,
	CYAN_BLUE,
	RED_BLUE,
	YELLOW_BLUE,
	BLACK_GREEN,
	BLACK_YELLOW,
	YELLOW_RED,
	YELLOW_BLACK,
};

typedef struct {
	unsigned long begin;
	unsigned long end;
	char attr[5];
	char dev[6];
	char name[256];
} map_t;

int read_mmaps(const char *filename, map_t *maps, const int max)
{
	FILE *fp;
	int i = 0;
	char buffer[4096];

	fp = fopen(filename, "r");
	if (fp == NULL)
		return 0;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		sscanf(buffer, "%lx-%lx %5s %*s %6s %*d %s",
			&maps[i].begin,
			&maps[i].end,
			maps[i].attr,
			maps[i].dev,
			maps[i].name);
			
		i++;
		if (i >= max)
			break;
	}
	fclose(fp);
	return i;
}

void handle_winch(int sig)
{
	(void)sig;

	resized = true;
}

void show_usage(void)
{
	printf(APP_NAME ", version " VERSION "\n\n"
		"Usage: " APP_NAME " [options] pid\n"
		" -h help\n");
}

int main(int argc, char **argv)
{
	unsigned long addr = 0;
	unsigned long page_size = 4096;
	bool do_run = true;
	pid_t pid = -1;
	char path_refs[PATH_MAX];
	char path_map[PATH_MAX];
	char path_mmap[PATH_MAX];
	map_t mmaps[MAX_MMAPS];
	int nmaps;
	int map_index = 0;
	int tick = 0;
	int xpos = 0, ypos = 0;
	bool page_view = false;

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
		printf("%s\n", argv[optind]);
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
	snprintf(path_map, sizeof(path_map),
		"/proc/%i/pagemap", pid);
	snprintf(path_mmap, sizeof(path_mmap),
		"/proc/%i/maps", pid);
	nmaps = read_mmaps(path_mmap, mmaps, MAX_MMAPS);

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

	init_pair(BLACK_WHITE, COLOR_BLACK, COLOR_WHITE);
	init_pair(CYAN_BLUE, COLOR_CYAN, COLOR_BLUE);
	init_pair(RED_BLUE, COLOR_RED, COLOR_BLUE);
	init_pair(YELLOW_BLUE, COLOR_YELLOW, COLOR_BLUE);
	init_pair(BLACK_GREEN, COLOR_BLACK, COLOR_GREEN);
	init_pair(BLACK_YELLOW, COLOR_BLACK, COLOR_YELLOW);
	init_pair(YELLOW_RED, COLOR_YELLOW, COLOR_RED);
	init_pair(YELLOW_BLACK, COLOR_YELLOW, COLOR_BLACK);

	addr = mmaps[map_index].begin;

	signal(SIGWINCH, handle_winch);

	do {
		int ch;
		int i;
		int width_step = COLS - 17;
		unsigned long width_page_step = page_size * width_step;
		int fd;
		unsigned long tmp_addr;
		int tmp_index;
		unsigned long addrs[LINES-2];
		bool mmapped;

		if (resized) {
			delwin(mainwin);
			endwin();
			refresh();
			clear();
			if (COLS < 18)
				break;
			if (LINES < 5)
				break;
			width_step = COLS - 17;
			width_page_step = page_size * width_step;
			mainwin = newwin(LINES, COLS, 0, 0);
			resized = false;
		}

		wbkgd(mainwin, COLOR_PAIR(RED_BLUE));

		tick++;
		if (tick > 10) {
			tick = 0;
			fd = open(path_refs, O_RDWR);
			if (fd < 0)
				break;
			if (write(fd, "4", 1) < 0)
				break;
			(void)close(fd);
		}

		fd = open(path_map, O_RDONLY);
		if (fd < 0)
			break;

		ch = getch();

		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		mvwprintw(mainwin, LINES - 1, 0, "KEY: ");

		wattrset(mainwin, COLOR_PAIR(WHITE_RED));
		wprintw(mainwin, "A");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " ANON or FILE mapped, ");

		wattrset(mainwin, COLOR_PAIR(WHITE_YELLOW));
		wprintw(mainwin, "R");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " in RAM, ");

		wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
		wprintw(mainwin, "D");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " Dirty, ");

		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
		wprintw(mainwin, ".");
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE));
		wprintw(mainwin, " not in RAM");

		wattrset(mainwin, COLOR_PAIR(BLACK_WHITE) | A_BOLD);
		tmp_index = map_index;
		tmp_addr = addr;
		for (i = 1; i < LINES - 1; i++) {
			uint64_t buffer[width_step];
			int j;
			unsigned long addr_check;

			if (tmp_addr > mmaps[tmp_index].end) {
				tmp_index++;
				if (tmp_index >= nmaps)
					tmp_index = 0;
				tmp_addr = mmaps[tmp_index].begin;
			}
			addrs[i - 1] = tmp_addr;
			lseek(fd, sizeof(uint64_t) * (tmp_addr / page_size), SEEK_SET);

			if (read(fd, buffer, sizeof(buffer)) < 0)
				break;
			wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));
			mvwprintw(mainwin, i, 0, "%16.16lx %s", tmp_addr);

			addr_check = tmp_addr;
			for (j = 0; j < width_step; j++) {
				char state = '.';

				wattrset(mainwin, COLOR_PAIR(BLACK_WHITE));

				if (buffer[j] & ((uint64_t)1 << 63)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_YELLOW));
					state = 'R';
				}
				if (buffer[j] & ((uint64_t)1 << 62)) {
					state = 'S';
				}
				if (buffer[j] & ((uint64_t)1 << 61)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_RED));
					state = 'A';
				}
				if (buffer[j] & ((uint64_t)1 << 55)) {
					wattrset(mainwin, COLOR_PAIR(WHITE_CYAN));
					state = 'D';
				}
				mvwprintw(mainwin, i, 17 + j, "%c", state);
				addr_check += 4096;
			}
			tmp_addr += width_page_step;
		}
		close(fd);
		tmp_addr = addrs[ypos] + (4096 * xpos);
		wattrset(mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		mvwprintw(mainwin, 0, 0, "Pagemon 0x%16.16lx ",
			tmp_addr);

		mmapped = false;
		for (i = 0; i < nmaps; i++) {
			if (mmaps[i].begin <= tmp_addr &&
			    mmaps[i].end >= tmp_addr) {
				mmapped = true;
				wprintw(mainwin, "%s %s %-20.20s",
					mmaps[i].attr,
					mmaps[i].dev,
					mmaps[i].name[0] == '\0' ?
						"[Anonymous]" :
						basename(mmaps[i].name));
				break;
			}
		}
		if (!mmapped)
			wprintw(mainwin, "Unmapped Page                   ");
		wattrset(mainwin, A_NORMAL);

		wattrset(mainwin, COLOR_PAIR(CYAN_BLUE) | A_BOLD);
		mvwprintw(mainwin, ypos + 1, xpos + 17, "#");
		wattrset(mainwin, A_NORMAL);

		if (page_view) {
			mvwprintw(mainwin, 3, 4, "PAGE DATA:");
		}

		wrefresh(mainwin);
		refresh();

		switch (ch) {
		case 27:	/* ESC */
		case 'q':
		case 'Q':
			do_run = false;
			break;
		case '\t':
			page_view = !page_view;
			break;
		case KEY_DOWN:
			ypos++;
			break;
		case KEY_UP:
			ypos--;
			break;
		case KEY_LEFT:
			xpos--;
			break;
		case KEY_RIGHT:
			xpos++;
			break;
		case KEY_NPAGE:
			addr += width_page_step * (LINES - 2);
			break;
		case KEY_PPAGE:
			addr -= width_page_step * (LINES - 2);
			break;
		}
		if (xpos >= width_step) {
			xpos = 0;
			ypos++;
		}
		if (xpos < 0) {
			xpos = width_step - 1;
			ypos--;
		}
		if (ypos >= LINES-2) {
			ypos = LINES-3;
			addr += width_page_step;
		}
		if (ypos < 0) {
			ypos = 0;
			addr -= width_page_step;
		}
		if (addr < mmaps[map_index].begin) {
			map_index--;
			if (map_index < 0)
				map_index = nmaps - 1;
			addr = mmaps[map_index].begin;
		}
		if (addr > mmaps[map_index].end) {
			map_index++;
			if (map_index >= nmaps)
				map_index = 0;
			addr = mmaps[map_index].begin;
		}
		usleep(5000);
	} while (do_run);


	endwin();

	exit(EXIT_SUCCESS);
}

