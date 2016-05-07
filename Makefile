include config.mk

NAME = stagit
VERSION = 0.4
SRC = \
	stagit.c\
	stagit-index.c
COMPATSRC = \
	reallocarray.c\
	strlcat.c\
	strlcpy.c
BIN = \
	stagit\
	stagit-index
MAN1 = \
	stagit.1\
	stagit-index.1
DOC = \
	LICENSE\
	README\
	TODO
HDR = compat.h

COMPATOBJ = \
	reallocarray.o\
	strlcat.o\
	strlcpy.o

OBJ = ${SRC:.c=.o} ${COMPATOBJ}

all: $(BIN)

.o:
	${CC} ${LDFLAGS} -o $@ ${LIBS}

.c.o:
	${CC} -c ${CFLAGS} ${CPPFLAGS} -o $@ -c $<

dist:
	rm -rf ${NAME}-${VERSION}
	mkdir -p ${NAME}-${VERSION}
	cp -f ${MAN1} ${HDR} ${SCRIPTS} ${SRC} ${COMPATSRC} ${DOC} \
		Makefile config.def.h config.mk \
		favicon.png logo.png style.css \
		example.sh \
		${NAME}-${VERSION}
	# make tarball
	tar -cf - ${NAME}-${VERSION} | \
		gzip -c > ${NAME}-${VERSION}.tar.gz
	rm -rf ${NAME}-${VERSION}

${OBJ}: config.mk ${HDR}

stagit: stagit.o ${COMPATOBJ}
	${CC} -o $@ stagit.o ${COMPATOBJ} ${LDFLAGS}

stagit-index: stagit-index.o ${COMPATOBJ}
	${CC} -o $@ stagit-index.o ${COMPATOBJ} ${LDFLAGS}

clean:
	rm -f ${BIN} ${OBJ} ${NAME}-${VERSION}.tar.gz

install: all
	# installing executable files.
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f ${BIN} ${SCRIPTS} ${DESTDIR}${PREFIX}/bin
	for f in $(BIN) $(SCRIPTS); do chmod 755 ${DESTDIR}${PREFIX}/bin/$$f; done
	# installing example files.
	mkdir -p ${DESTDIR}${PREFIX}/share/${NAME}
	cp -f style.css\
		favicon.png\
		logo.png\
		example.sh\
		README\
		${DESTDIR}${PREFIX}/share/${NAME}
	# installing manual pages.
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	cp -f ${MAN1} ${DESTDIR}${MANPREFIX}/man1
	for m in $(MAN1); do chmod 644 ${DESTDIR}${MANPREFIX}/man1/$$m; done

uninstall:
	# removing executable files and scripts.
	for f in $(BIN) $(SCRIPTS); do rm -f ${DESTDIR}${PREFIX}/bin/$$f; done
	# removing example files.
	rm -f \
		${DESTDIR}${PREFIX}/share/${NAME}/style.css\
		${DESTDIR}${PREFIX}/share/${NAME}/favicon.png\
		${DESTDIR}${PREFIX}/share/${NAME}/logo.png\
		${DESTDIR}${PREFIX}/share/${NAME}/example.sh\
		${DESTDIR}${PREFIX}/share/${NAME}/README
	-rmdir ${DESTDIR}${PREFIX}/share/${NAME}
	# removing manual pages.
	for m in $(MAN1); do rm -f ${DESTDIR}${MANPREFIX}/man1/$$m; done

.PHONY: all clean dist install uninstall
