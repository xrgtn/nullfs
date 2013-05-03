#! /usr/bin/make -f

default: all

package?=nullfs
srcs?=$(wildcard *.c++)
objs?=${srcs:.c++=.o}
exes?=${package} nul1fs nulnfs

LDFLAGS=$(shell pkg-config fuse --libs)
CXXFLAGS=$(shell pkg-config fuse --cflags)


all: ${exes} 

${package}: ${objs}
	${CXX} -o ${@F} ${LDFLAGS} $^

clean:
	${RM} -fv *.o *~

%.o:%.c++
	${CXX} -c -o $@ ${CXXFLAGS} $<

install: ${package} ${exes}
	install -d ${DESTDIR}/usr/bin/
	install $< ${DESTDIR}/usr/bin/
	install nul1fs ${DESTDIR}/usr/bin/
	install nulnfs ${DESTDIR}/usr/bin/

distclean: clean
	${RM} -fv ${package} ${exes}

help:
	@echo "srcs=${srcs}"
	@echo "objs=${objs}"

