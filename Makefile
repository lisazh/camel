CFLAGS=-Wall -finline-limit=65000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer
RELEASEFLAGS= -DNDEBUG -O3
DEBUGFLAGS=-g
LIBS=-lm -lpthread

.PHONY: all release threadtest threadtest-release clean

all:
	gcc -o main ${CFLAGS} ${DEBUGFLAGS} main.c malloc.c memlib.c mm_thread.c tsc.c ${LIBS}

release:
	gcc -o main ${CFLAGS} ${DEBUGFLAGS} main.c malloc.c memlib.c mm_thread.c tsc.c ${LIBS}

threadtest:
	gcc -o threadtest ${CFLAGS} ${DEBUGFLAGS} threadtest.c malloc.c memlib.c mm_thread.c tsc.c ${LIBS}

threadtest-release:
	gcc -o threadtest ${CFLAGS} ${RELEASEFLAGS} threadtest.c malloc.c memlib.c mm_thread.c tsc.c ${LIBS}

clean:
	rm -f main
