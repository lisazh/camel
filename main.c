#include <stdio.h>
#include <assert.h>

#include "malloc.h"

int main(int argc, char **argv) {
	printf("Hello Deathly Abstract Machina!\n");
	
	assert(!mm_init());
	
	return 0;
}
