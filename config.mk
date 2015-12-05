# customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

GITINC = /usr/local/include
GITLIB = /usr/local/lib

# includes and libs
INCS = -I${GITINC}
LIBS = -L${GITLIB} -lgit2 -lc

# debug
CFLAGS = -fstack-protector-all -O0 -g -std=c99 -Wall -Wextra -pedantic \
	-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE ${INCS}
LDFLAGS = ${LIBS}

# optimized
#CFLAGS = -O2 -std=c99 \
#	-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE ${INCS}
#LDFLAGS = -s ${LIBS}

# optimized static
#CFLAGS = -static -O2 -std=c99 \
#	-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE ${INCS}
#LDFLAGS = -static -s ${LIBS}

# compiler and linker
#CC = cc
