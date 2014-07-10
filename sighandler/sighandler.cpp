#include <pthread.h>
#include <signal.h>
#include <unistd.h>			// for write() and usleep()
#include <execinfo.h>		// for backtrace() and backtrace_symbols_fd()
#include <stdio.h>			// for stdin/stdout/fflush/printf etc.
#include <stdlib.h>			// for abort()
#include <string.h>			// for memset()
#include <errno.h>			// for EBUSY
#include <sys/wait.h>		// for waitpid()
#include <sys/resource.h>	// for struct rlimit
#include <algorithm>		// for std::min

#include "watchdog.h"

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
	// NOTE: in the real world, these two might be worth catching – could mean
	// something happened to a child process, e.g. the watchdog
	SIGCHLD,
	SIGPIPE
};
static const size_t g_num_ignore = sizeof(g_ignore) / sizeof(g_ignore[0]);

// spinlock for blocking handler access against concurrent faulting threads
static pthread_spinlock_t g_handler_lock;

// we keep default signal actions here for possible chaining
static struct sigaction g_default_actions[g_num_interest];

static pid_t g_watchdog_pid = (pid_t)-1;


// ============================================================================


void game_signal_handler(int signum, siginfo_t *info, void *context)
{
	// some of the signals are supposed to dump core
	bool coredump	= signum == SIGSEGV
				   || signum == SIGQUIT
				   || signum == SIGFPE;
	// and some are survivable while others aren't
	// NOTE: strictly speaking, some signals are not faults but graceful exit
	// requests, e.g. SIGTERM, and SIGCHLD informs about child execution state
	// change
	bool fatal		= signum != SIGCHLD && signum != SIGTERM && signum != SIGQUIT;
	bool clean		= signum == SIGTERM;
	
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
	wd.siginfo	= *info;
	// actual stack walking happens here
	wd.depth	= backtrace(stack, sizeof(stack) / sizeof(stack[0]));
	write(g_watchdog_pipe[1], &wd, sizeof(wd));
	
	// push stack trace down the pipe
	backtrace_symbols_fd(stack, wd.depth, g_watchdog_pipe[1]);
	
	// at this point we can let the other threads in
	pthread_spin_unlock(&g_handler_lock);
	
	if (coredump)
	{
		// NOTE: printf is unsafe! just for illustration purposes!
		printf("[Sighandler] Want core dump, raising\n");
		
		// restore default handler
		size_t index;
		for (size_t i = 0; i < g_num_interest; ++i)
		{
			if (g_interest[i] == signum)
			{
				index = i;
				break;
			}
		}
		sigaction(signum, &g_default_actions[index], NULL);
		// re-raise so that the default handler dumps core
		raise(signum);
	}
	else if (clean)
	{
		// no-op in this example, but a real program would queue a request for a
		// graceful exit here
	}
	else if (fatal)
	{
		// NOTE: printf is unsafe! just for illustration purposes!
		printf("[Sighandler] Signal is fatal, aborting\n");
		
		// make sure all other output is flushed
		fflush(stdout);
		fflush(stderr);
		
		abort();
	}
}

int sighandler_install(size_t max_core_size)
{
	int retval = 0;
	
	// start by creating the pipe
	retval = pipe(g_watchdog_pipe);
	if (retval != 0)
		return retval;
	
	// fork ASAP, before our process image grows big!
	// NOTE:	the debugger (gdb) will by default follow the parent upon a
	//			fork; if you want to debug the child process instead, you will
	//			need to either change the fork following mode in gdb:
	//
	//			(gdb) set follow-fork-mode child
	//
	//			or simply run another gdb instance and attach to the child
	//			process (the variable pid below will contain its PID)
	g_watchdog_pid = fork();
#if WATCHDOG_IS_PARENT
	if (g_watchdog_pid != 0)
		// we are the watchdog as parent
		exit(watchdog(g_watchdog_pid));
#else
	if (g_watchdog_pid == 0)
		// we are the watchdog as child
		exit(watchdog(getppid()));
#endif
	
	// enable core dumping
	struct rlimit rlim;
	retval = getrlimit(RLIMIT_CORE, &rlim);
	if (retval == 0)
	{
		if (rlim.rlim_max != RLIM_INFINITY)
			rlim.rlim_cur = std::min(max_core_size, rlim.rlim_max);
		else if (max_core_size == (size_t)-1)
			rlim.rlim_cur = RLIM_INFINITY;
		else
			rlim.rlim_cur = max_core_size;
		retval = getrlimit(RLIMIT_CORE, &rlim);
		if (retval == 0)
			printf("[Sighandler] Core dump size set to %d\n", rlim.rlim_cur);
		else
			printf("[Sighandler] Cannot set core dump size, core dumping "
				"probably won't work. Error: %s\n", strerror(errno));
	}
	else
		printf("[Sighandler] Cannot get maximum core dump size, core dumping "
			"probably won't work. Error: %s\n", strerror(errno));
	
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
			fprintf(stderr, "[Sighandler] Failed to set handler for signal %s: %s\n",
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
			fprintf(stderr, "[Sighandler] Failed to ignore signal %s: %s\n",
				strsignal(g_ignore[i]), strerror(errno));
		}
	}
	
	return retval;
}

void sighandler_cleanup()
{
	while (pthread_spin_destroy(&g_handler_lock) == EBUSY)
		usleep(10 * 1000);
	close(g_watchdog_pipe[0]);
	close(g_watchdog_pipe[1]);
	// no need to wait for the watchdog – if it's the parent, it will react to
	// SIGCHLD; if it's the child, it will die once orphaned
}