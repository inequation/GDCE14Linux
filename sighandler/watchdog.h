#pragma once

#include <sys/types.h>	// for pid_t

// file descriptors of both ends of the pipe - [0] for reading, [1] for writing
extern int g_watchdog_pipe[2];

// watchdog process entry point
int watchdog(pid_t game);

// struct of the signal data we want to forward to the watchdog
struct watchdog_data
{
	int signal;	// signal number
	int code;	// reason for sending the signal
	void *addr;	// address of fault (if relevant)
	int depth;	// depth of backtrace
};
