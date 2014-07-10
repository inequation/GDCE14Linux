#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>	// for usleep()

#include "sighandler.h"

void *segfault(void *arg = NULL)
{
	// try to sleep some so that different threads get a chance to compete for
	// the signal handler
	usleep(rand() % 10000);	// up to 10 ms
	*(int *)0xabad1dea = 0;
	return NULL;
}

int main(int argc, char *argv[])
{
	int retval = 0;
	
	// set up signal handling as the very first thing after start!
	retval = sighandler_install();
	if (retval != 0)
	{
		printf("[Game] Failed to set up the signal handler!\n");
		return retval;
	}
	
	printf("[Game] Init done, attempting segfault\n");
	
	// spawn some segfaulting threads to try competing for the handler
	pthread_t pool[4];
	for (size_t i = 0; i < sizeof(pool) / sizeof(pool[0]); ++i)
	{
		retval = pthread_create(&pool[i], NULL, segfault, NULL);
		if (retval != 0)
			return retval;
	}
	
	// also segfault deliberately here!
	segfault();
	
	// we never reach here, but this is what needs to be done for proper cleanup
	sighandler_cleanup();
	
	return retval;
}