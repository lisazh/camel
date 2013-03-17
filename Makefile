CFLAGS=-Wall -finline-limit=65000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer
RELEASEFLAGS= ${CFLAGS} -DNDEBUG -O3
DEBUGFLAGS=${CFLAGS} -g
LIBS=malloc.c memlib.c mm_thread.c tsc.c -lm -lpthread

.PHONY: clean all threadtest cache-thrash cache-scratch larson

all:
	gcc -o main ${DEBUGFLAGS} main.c ${LIBS}

release:
	gcc -o main ${RELEASEFLAGS} main.c ${LIBS}


threadtest:
	gcc -o threadtest ${DEBUGFLAGS} threadtest.c ${LIBS}

threadtest-release:
	gcc -o threadtest ${RELEASEFLAGS} threadtest.c ${LIBS}

cache-thrash:
	gcc -o cache-thrash ${DEBUGFLAGS} cache-thrash.c ${LIBS}

cache-thrash-release:
	gcc -o cache-thrash ${RELEASEFLAGS} cache-thrash.c ${LIBS}

cache-scratch:
	gcc -o cache-scratch ${DEBUGFLAGS} cache-scratch.c ${LIBS}

cache-scratch-release:
	gcc -o cache-scratch ${RELEASEFLAGS} cache-scratch.c ${LIBS}

larson:
	gcc -o larson ${DEBUGFLAGS} larson.c ${LIBS}

larson-release:
	gcc -o larson ${RELEASEFLAGS} larson.c ${LIBS}

clean:
	rm -f main
