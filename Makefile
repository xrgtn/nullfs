default: all
package?=nullfs
srcs?=$(wildcard *.c++)
objs?=${srcs:.c++=.o}
LDFLAGS=$(shell pkg-config fuse --libs)
CXXFLAGS=$(shell pkg-config fuse --cflags)
all: ${package}

${package}: ${objs}
	${CXX} -o ${@F} ${LDFLAGS} $^

clean:
	${RM} -fv *.o


%.o:%.c++
	${CXX} -c -o $@ ${CXXFLAGS} $<


install: ${package}
	install -d ${DESTDIR}/usr/bin/
	install $< ${DESTDIR}/usr/bin/


distclean: clean
	${RM} -fv ${package}


help:
	@echo "srcs=${srcs}"
	@echo "objs=${objs}"

