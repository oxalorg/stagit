include config.mk

NAME = urmoms
VERSION = 0.1
SRC = \
	urmoms.c\
	urmoms-index.c
COMPATSRC = \
	strlcat.c\
	strlcpy.c
BIN = \
	urmoms\
	urmoms-index
MAN1 = \
	urmoms.1\
	urmoms-index.1
DOC = \
	LICENSE\
	README\
	TODO
HDR = compat.h

OBJ = ${SRC:.c=.o} ${COMPATOBJ}

all: $(BIN)

.c.o:
	${CC} -c ${CFLAGS} $<

dist: $(BIN)
	rm -rf release/${VERSION}
	mkdir -p release/${VERSION}
	cp -f ${MAN1} ${HDR} ${SCRIPTS} ${SRC} ${COMPATSRC} ${DOC} \
		Makefile config.mk \
		logo.png style.css \
		release/${VERSION}/
	# make tarball
	rm -f urmoms-${VERSION}.tar.gz
	(cd release/${VERSION}; \
	tar -czf ../../urmoms-${VERSION}.tar.gz .)

${OBJ}: config.h config.mk ${HDR}

config.h:
	@echo creating $@ from config.def.h
	@cp config.def.h $@

urmoms: urmoms.o ${COMPATOBJ}
	${CC} -o $@ urmoms.o ${COMPATOBJ} ${LDFLAGS}

urmoms-index: urmoms-index.o ${COMPATOBJ}
	${CC} -o $@ urmoms-index.o ${COMPATOBJ} ${LDFLAGS}

clean:
	rm -f ${BIN} ${OBJ}

install: all
	# installing executable files.
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f ${BIN} ${SCRIPTS} ${DESTDIR}${PREFIX}/bin
	for f in $(BIN) $(SCRIPTS); do chmod 755 ${DESTDIR}${PREFIX}/bin/$$f; done
	# installing example files.
	mkdir -p ${DESTDIR}${PREFIX}/share/${NAME}
	cp -f style.css\
		logo.png\
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
		${DESTDIR}${PREFIX}/share/${NAME}/logo.png\
		${DESTDIR}${PREFIX}/share/${NAME}/README
	-rmdir ${DESTDIR}${PREFIX}/share/${NAME}
	# removing manual pages.
	for m in $(MAN1); do rm -f ${DESTDIR}${MANPREFIX}/man1/$$m; done

.PHONY: all clean dist install uninstall
