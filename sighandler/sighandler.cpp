#include <pthread.h>
#include <signal.h>
#include <unistd.h>		// for write() and usleep()
#include <execinfo.h>	// for backtrace() and backtrace_symbols_fd()
#include <stdio.h>		// for stdin/stdout/fflush/printf etc.
#include <stdlib.h>		// for abort()
#include <string.h>		// for memset()
#include <errno.h>		// for EBUSY
#include <sys/wait.h>	// for waitpid()

#include "watchdog.h"

// spinlock for blocking handler access against concurrent faulting threads
static pthread_spinlock_t g_handler_lock;

// list of signals we care about
static const int g_interest[] =
{
	SIGSEGV,
	SIGILL,
	SIGHUP,
	SIGQUIT,
	SIGTRAP,
	SIGIOT,
	SIGBUS,
	SIGFPE,
	SIGTERM,
	SIGINT
};
static const size_t g_num_interest = sizeof(g_interest) / sizeof(g_interest[0]);

// list of signals we ignore
static const int g_ignore[] =
{
	SIGCHLD,
	SIGPIPE	// NOTE: in the real world, this may be worth catching – could mean
			// something happened to the watchdog
};
static const size_t g_num_ignore = sizeof(g_ignore) / sizeof(g_ignore[0]);

// we keep default signal actions here for possible chaining
static struct sigaction g_default_actions[g_num_interest];

void game_signal_handler(int signal, siginfo_t *info, void *context)
{
	// some of the signals are supposed to dump core
	bool coredump	= signal == SIGSEGV
				   || signal == SIGQUIT
				   || signal == SIGFPE;
	// and some are survivable while others aren't
	// NOTE: strictly speaking, some signals are not faults but graceful exit
	// requests, e.g. SIGTERM, and SIGCHLD informs about child execution state
	// change
	bool fatal		= signal != SIGCHLD && signal != SIGTERM && signal != SIGQUIT;
	bool clean		= signal == SIGTERM;
	
	// spin to block concurrent faulting threads
	// NOTE: if the contenting thread is of higher priority, we'll deadlock -
	// it would be more robust to pthread_spin_trylock() and sleep with
	// pselect() instead
	pthread_spin_lock(&g_handler_lock);
	
	// array is static to avoid runtime allocs
	static void *stack[64];	// max depth of stack that we'll walk is arbitrary
	
	// dump the information down the pipe
	// NOTE: printf() and friends are *NOT* safe! that's why the custom protocol
	struct watchdog_data wd;
	wd.signal	= signal;
	wd.code		= info->si_code;
	wd.addr		= info->si_addr;
	// actual stack walking happens here
	wd.depth	= backtrace(stack, sizeof(stack) / sizeof(stack[0]));
	write(g_watchdog_pipe[1], &wd, sizeof(wd));
	
	// push stack trace down the pipe
	backtrace_symbols_fd(stack, wd.depth, g_watchdog_pipe[1]);
	
	// at this point we can let the other threads in
	pthread_spin_unlock(&g_handler_lock);
	
	if (coredump)
	{
		// restore default handler
		size_t index;
		for (size_t i = 0; i < g_num_interest; ++i)
		{
			if (g_interest[i] == signal)
			{
				index = i;
				break;
			}
		}
		sigaction(signal, &g_default_actions[index], NULL);
		// re-raise so that the default handler dumps core
		raise(signal);
	}
	else if (clean)
	{
		// no-op in this example, but a real program would queue a request for a
		// graceful exit here
	}
	else if (fatal)
	{
		// make sure all other output is flushed
		fflush(stdout);
		fflush(stderr);
		
		abort();
	}
}

void *segfault(void *arg = NULL)
{
	// try to sleep some so that different threads get a chance to compete for
	// the signal handler
	usleep(rand() % 100);
	*(int *)0xabad1dea = 0;
	return NULL;
}

int main(int argc, char *argv[])
{
	int retval = 0;
	
	// start by creating the pipe
	retval = pipe(g_watchdog_pipe);
	if (retval != 0)
		return retval;
	
	// fork ASAP, before our process image grows big!
	// NOTE:	the debugger (gdb) will by default follow the parent upon a
	//			fork, which will be the watchdog; if you want to debug the game
	//			process instead, you will need to either change the fork
	//			following mode in gdb:
	//
	//			(gdb) set follow-fork-mode child
	//
	//			(this command can be put in your ~/.gdbinit file, for instance)
	//			or simply run another gdb instance and attach to the child
	//			process (the variable pid below will contain its PID)
	pid_t pid = fork();
	if (pid != 0)
		// we are the watchdog
		return watchdog(pid);
	
	// initialize the signal handler spinlock
	pthread_spin_init(&g_handler_lock, PTHREAD_PROCESS_PRIVATE);
	
	// set up game the signal handler
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = game_signal_handler;
	// give us extended signal info, please
	action.sa_flags = SA_SIGINFO;

	// register our handler, stash away the default one
	for (size_t i = 0; i < g_num_interest; ++i)
	{
		retval = sigaction(g_interest[i], &action, &g_default_actions[i]);
		if (retval != 0)
		{
			fprintf(stderr, "[Game] Failed to set handler for signal %s: %s\n",
				strsignal(g_interest[i]), strerror(errno));
		}
	}
	
	// ignore the signals we don't want
	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_IGN;
	for (size_t i = 0; i < g_num_ignore; ++i)
	{
		retval = sigaction(g_ignore[i], &action, NULL);
		if (retval != 0)
		{
			fprintf(stderr, "[Game] Failed to ignore signal %s: %s\n",
				strsignal(g_ignore[i]), strerror(errno));
		}
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
	// closing the pipe sends a SIGPIPE to the watchdog, which should cause it
	// to die gracefully
	while (pthread_spin_destroy(&g_handler_lock) == EBUSY)
		usleep(10 * 1000);
	close(g_watchdog_pipe[0]);
	close(g_watchdog_pipe[1]);
	// wait for the watchdog to die
	waitpid(pid, NULL, 0);
	
	return retval;
}