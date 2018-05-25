# customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/man

# compiler and linker
#CC = cc

GITINC = /usr/local/include
GITLIB = /usr/local/lib

# includes and libs
INCS = -I${GITINC}
LIBS = -L${GITLIB} -lgit2

# debug
#CFLAGS = -fstack-protector-all -O0 -g -std=c99 -Wall -Wextra -pedantic ${INCS}
#LDFLAGS = ${LIBS}

# optimized
CFLAGS = -O2 -std=c99 ${INCS}
LDFLAGS = -s ${LIBS}

# optimized static
#CFLAGS = -static -O2 -std=c99 ${INCS}
#LDFLAGS = -static -s ${LIBS}

CPPFLAGS = -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_BSD_SOURCE ${INCS}
