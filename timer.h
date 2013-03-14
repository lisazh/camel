#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "tsc.h"


static double getFrequency (void) {
	static double freq = 0.0;
	unsigned long LTime0, LTime1, HTime0, HTime1;
	ssize_t bytes_read;

	if (freq == 0.0) {

		// Parse out the CPU frequency from the /proc/cpuinfo file.

		enum { MAX_PROCFILE_SIZE = 32768 };
		const char searchStr[] = "cpu MHz		: ";
		char line[MAX_PROCFILE_SIZE];
		int fd = open ("/proc/cpuinfo", O_RDONLY);
		if ((bytes_read = read(fd, line, MAX_PROCFILE_SIZE)) == -1) {
			perror("getFrequency: Error reading from /proc/cpuinfo");
			return freq;
		}
		char * pos = strstr (line, searchStr);
		if (pos == NULL) {
			
			// Compute MHz directly.
			// Wait for approximately one second.
			
			start_counter();
			sleep (1);
			freq = get_counter();
			
		} else {
			// Move the pointer over to the MHz number itself.
			pos += strlen(searchStr);
			float f;
			sscanf (pos, "%f", &f);
			freq = (double) f * 1000000.0;
		}
		
	}
	return freq;
}

void timer_start (void) {
	start_counter();
}

double timer_stop (void) {
	u_int64_t elapsed = get_counter();
	double freq = getFrequency ();
	return elapsed / freq;
}

#endif /* _TIMER_H */
