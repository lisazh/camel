#include "mm_thread.h"


/* Set thread attributes */

extern void initialize_pthread_attr(int detachstate, int schedpolicy, int priority, 
				    int inheritsched, int scope, pthread_attr_t *attr)
{
	pthread_attr_init(attr);
	pthread_attr_setdetachstate(attr, detachstate);
	if (inheritsched == PTHREAD_EXPLICIT_SCHED) {
		pthread_attr_setschedpolicy(attr, schedpolicy);
		struct sched_param p;
		p.sched_priority = priority;
		pthread_attr_setschedparam(attr, &p);
	}
	pthread_attr_setscope(attr, scope);
}


int getNumProcessors (void)
{
	static int np = 0;
	if (!np) {
		// Ugly workaround.  Linux's sysconf indirectly calls malloc() (at
		// least on multiprocessors).  So we just read the info from the
		// proc file ourselves and count the occurrences of the word
		// "processor".
  
	        int MAX_LINE_SIZE = 512;
		char line[MAX_LINE_SIZE];
		int fd = open ("/proc/cpuinfo", O_RDONLY);
		if (!fd) {
			return 1;
		} else {
			int bytes;
			while ( (bytes = read (fd, line, MAX_LINE_SIZE)) != 0) {
				char * str = line;
				while (str) {
					str = strstr(str, "processor");
					if (str) {
						np++;
						str++;
					}
				}
			}
			close (fd);
			return np;
		}
	} else {
		return np;
	}
}

int getTID(void) {
  return syscall(__NR_gettid);
}

void setCPU (int n) {
	/* Set CPU affinity to CPU n only. */
	pid_t tid = syscall(__NR_gettid);
	cpu_set_t mask;
	CPU_ZERO(&mask);
	CPU_SET(n, &mask);
	if (sched_setaffinity(tid, sizeof(cpu_set_t), &mask) != 0) {
		perror("sched_setaffinity failed");
	} 
}

