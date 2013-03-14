CFLAGS=-Wall -finline-limit=65000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer
RELEASEFLAGS= -DNDEBUG -O3
DEBUGFLAGS=-g

all:
	gcc -o main ${CFLAGS} ${DEBUGFLAGS} main.c malloc.c memlib.c mm_thread.c tsc.c -lm

release:
	gcc -o main ${CFLAGS} ${DEBUGFLAGS} main.c malloc.c memlib.c mm_thread.c tsc.c -lm

clean:
	rm -f main
