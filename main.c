#include <stdio.h>
#include <assert.h>

#include "malloc.h"

int main(int argc, char **argv) {
	printf("Hello Deathly Abstract Machina!\n");
	
	assert(!mm_init());

	int i, j=2;
	for (i = 0; i < 20; i ++) {
	  char *m1 = mm_malloc(j);
	  int k;
	  for (k = 0; k < j; k++){
	    m1[k] = 0;
	  }  
	  printf("address of m%d: %p\n", i, m1);
	  j *= 2;
	}
	
	return 0;
}



