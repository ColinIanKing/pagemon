#
# Copyright (C) 2011-2015 Canonical
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# Author: Colin Ian King <colin.king@canonical.com>
#

VERSION=0.01.01

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"' -O2 -g
LDFLAGS += -lncurses

BINDIR=/usr/sbin
MANDIR=/usr/share/man/man8


pagemon: pagemon.o Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS)  $< -lm -o $@ $(LDFLAGS)

pagemon.o: pagemon.c Makefile

pagemon.8.gz: pagemon.8
	gzip -c $< > $@

dist:
	rm -rf pagemon-$(VERSION)
	mkdir pagemon-$(VERSION)
	cp -rp README Makefile pagemon.c pagemon.8 COPYING pagemon-$(VERSION)
	tar -zcf pagemon-$(VERSION).tar.gz pagemon-$(VERSION)
	rm -rf pagemon-$(VERSION)

clean:
	rm -f pagemon pagemon.o pagemon.8.gz

install: pagemon pagemon.8.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp pagemon ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp pagemon.8.gz ${DESTDIR}${MANDIR}
