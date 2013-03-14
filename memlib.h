#ifndef __MEMLIB_H_
#define __MEMLIB_H_

/* $Id$ */

/*
 *  CSC 469 - Assignment 1
 *
 */

#include <unistd.h>

#ifndef ptrdiff_t
# define ptrdiff_t       long
#endif


#define DSEG_MAX 40*1024*1024  /* 40 Mb */

extern char *dseg_lo, *dseg_hi;
extern long dseg_size;

extern int mem_init (void);
extern void *mem_sbrk (ptrdiff_t increment);
extern int mem_pagesize (void);
extern int mem_usage (void);

#endif /* __MEMLIB_H_ */

