#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>		// for read()
#include <assert.h>
#include <errno.h>
#include <sys/types.h>	// for pid_t

#include "watchdog.h"

// file descriptors of both ends of the pipe - [0] for reading, [1] for writing
int g_watchdog_pipe[2];

// pid of the game process
static pid_t g_game;

// watchdog exit request flag
#if WATCHDOG_IS_PARENT
	static volatile bool g_watchdog_exit = false;

void watchdog_signal_handler(int signal, siginfo_t *info, void *context)
{
	// we only react to SIGCHLD
	assert(signal == SIGCHLD && "Unsupported signal");
	
	// this call would be unsafe in the game process :)
	psiginfo(info, "[Watchdog] Received signal");
	
	// ignore for children other than the main game process
	if (info->si_pid != g_game)
		return;
	
	// quit if our chlid process has exited
	g_watchdog_exit = true;
}
#else	// WATCHDOG_IS_PARENT
	// in unix, if a parent process dies, its children are re-parented upward in
	// the process tree, so we can detect orphaning as parent PID change
	#define g_watchdog_exit	(g_game == getppid())
#endif

void watchdog_print(struct watchdog_data *wd, const char *stack)
{
	psiginfo(&wd->siginfo, "[Watchdog] Game received signal");
	printf("[Watchdog] Stack trace (%d frames):\n%s\n", wd->depth, stack);
}

int watchdog(pid_t game)
{
	// stash away the game pid
	g_game = game;
	
	// close the writing end of the pipe, we don't need it
	close(g_watchdog_pipe[1]);
	
	int retval = 0;
	
#if WATCHDOG_IS_PARENT
	// set up the watchdog signal handler; we only actually care about SIGCHLD
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = watchdog_signal_handler;
	// filter out suspend/resume signals, don't turn the child into a zombie and
	// give us extended signal info, please
	action.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO;
	retval = sigaction(SIGCHLD, &action, NULL);
	if (retval != 0)
	{
		printf("[Watchdog] Failed to set signal handler!\n");
		return retval;
	}
#endif
	
	printf("[Watchdog] Running!\n");
	
	// now just keep reading that pipe and spewing it out
	struct watchdog_data wd;
	char stack[1024], *s;
	int left;
	do
	{
		memset(&wd, 0, sizeof(wd));
		stack[0] = 0;
		s = stack;
		left = sizeof(wd);
		
		// we need to read in a loop, because read() may be interrupted by a
		// signal and return fewer bytes than the entire struct
		while (left > 0)
		{
			retval = read(g_watchdog_pipe[0],
						  ((char *)&wd) + sizeof(wd) - left,
						  left);
			if (retval <= 0)
			{
				if (retval < 0 && errno == EINTR)
				{
					printf("[Watchdog] Interrupted syscall, retrying read\n");
					continue;
				}
				else if (retval < 0 || left != sizeof(wd))
				{
					// either an I/O error occured or premature EOF was received
					char errinfo[128];
					snprintf(errinfo, sizeof(errinfo),
						"Signal information incomplete! %s",
						retval == 0 ? "EOF" : strerror(errno));
					watchdog_print(&wd, errinfo);
					goto die;
				}
				else
				{
					printf("[Watchdog] Pipe EOF\n");
					goto die;
				}
			}
			left -= retval;
		}
		
		// OK, we now have the basic information, try reading the stack trace
		// instead of bytes we'll be counting lines (LF characters)
		left = wd.depth;
		while (left > 0)
		{
			retval = read(g_watchdog_pipe[0], s, sizeof(stack) - (s - stack));
			if (retval <= 0)
			{
				if (retval < 0 && errno == EINTR)
				{
					printf("[Watchdog] Interrupted syscall, retrying read\n");
					continue;
				}
				else if (retval < 0)
				{
					// an I/O error occured
					char errinfo[128];
					snprintf(errinfo, sizeof(errinfo),
						"Signal information incomplete! %s", strerror(errno));
					watchdog_print(&wd, errinfo);
					goto die;
				}
				else
				{
					printf("[Watchdog] EOF received, truncating stack trace\n");
					left = 0;
				}
			}
			
			s[retval] = 0;
			// count the number of newlines
			int num_lf = 0;
			for (char *p = strchr(s, '\n'); p && *p; p = strchr(p + 1, '\n'))
				++num_lf;
			left -= num_lf;
			// advance the pointer
			s += retval;
		}
		
		// all information collected, print it
		watchdog_print(&wd, stack);
		
		// phew! info about one signal emitted, wait for another one
	} while (true);
	
die:
	
	close(g_watchdog_pipe[0]);
	
	printf("[Watchdog] Terminating\n");
	
	return 0;
}