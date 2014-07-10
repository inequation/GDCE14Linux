#pragma once

#include <signal.h>		// for siginfo_t
#include <sys/types.h>	// for pid_t

// define this to 1 to build with the watchdog running as the parent process;
// can be useful for debugging it, for instance
#define WATCHDOG_IS_PARENT	0

// file descriptors of both ends of the pipe - [0] for reading, [1] for writing
extern int g_watchdog_pipe[2];

// watchdog process entry point
int watchdog(pid_t game);

// struct of the signal data we want to forward to the watchdog
struct watchdog_data
{
	siginfo_t siginfo;
	int depth;	// depth of backtrace
};
