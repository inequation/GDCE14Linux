// To build: g++ sighandler.cpp -o sighandler -lpthread -rdynamic

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <execinfo.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

// file descriptors of both ends of the pipe - [0] for reading, [1] for writing
static int g_handler_pipe[2];

// spinlock for blocking handler access against concurrent faulting threads
static pthread_spinlock_t g_handler_lock;

// watchdog exit request flag
static bool g_watchdog_exit = false;

// list of signals we care about
static const int g_interest[] =
{
	SIGSEGV,
	SIGILL,
	SIGCHLD,
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

// we keep default signal actions here for possible chaining
static struct sigaction g_default_actions[g_num_interest];

void game_signal_handler(int signal, siginfo_t *info, void *context)
{
	// some of the signals are supposed to dump core
	bool coredump = signal == SIGSEGV
				 || signal == SIGQUIT
				 || signal == SIGFPE;
	// and some are survivable while others aren't
	// NOTE: strictly speaking, some signals are not faults but graceful exit
	// requests, e.g. SIGTERM, and SIGCHLD informs about child execution state
	// change
	bool fatal	  = signal != SIGCHLD;
	
	// spin to block concurrent faulting threads
	// NOTE: if the contenting thread is of higher priority, we'll deadlock -
	// it would be more robust to pthread_spin_trylock() and sleep with
	// pselect() instead
	pthread_spin_lock(&g_handler_lock);
	
	// dump some information down the pipe
	// NOTE: printf() and friends are *NOT* safe! that's why the custom protocol
	write(g_handler_pipe[1], &signal, sizeof(signal));
	write(g_handler_pipe[1], &info->si_code, sizeof(info->si_code));
	write(g_handler_pipe[1], &info->si_addr, sizeof(info->si_addr));
	
	// walk the stack (array is static to avoid runtime allocs)
	static void *stack[64];	// max depth of stack that we'll walk
	int depth = backtrace(stack, sizeof(stack) / sizeof(stack[0]));
	
	// push stack info down the pipe
	write(g_handler_pipe[1], &depth, sizeof(depth));
	backtrace_symbols_fd(stack, depth, g_handler_pipe[1]);
	
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
	else if (fatal)
	{
		// make sure all other output is flushed
		fflush(stdout);
		fflush(stderr);
		
		_exit(1);
	}
}

void watchdog_signal_handler(int signal, siginfo_t *info, void *context)
{
	// we only react to SIGPIPE, just ask the main thread to exit ASAP
	g_watchdog_exit = true;
	return;
}

void watchdog_print(int signal, int code, int addr, const char *stack)
{
	printf("Game received signal %d (code: %d) at address 0x%p. "
		"Stack trace:\n%s\n", signal, code, addr, stack);
}

int watchdog()
{
	int retval = 0;
	
	// then set up the watchdog signal handler; we only actually care about
	// SIGPIPE
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = watchdog_signal_handler;
	sigaction(SIGPIPE, &action, NULL);
	
	// make our end of the pipe non-blocking
	int flags = fcntl(g_handler_pipe[0], F_GETFL, 0);
	retval = fcntl(g_handler_pipe[0], F_SETFL, flags | O_NONBLOCK);
	
	if (retval != 0)
		return retval;
	
	printf("Watchdog running!\n");
	
	// now just keep reading that pipe and spewing it out
	int val[4];
	char stack[0];
	while (!g_watchdog_exit)
	{
		val[0] = val[1] = val[2] = val[3] = 0;
		stack[0] = 0;
		
		for (size_t i = 0; i < sizeof(val) / sizeof(val[0]); ++i)
		{
			while (true)
			{
				retval = read(g_handler_pipe[0], &val[i], sizeof(val[0]));
				if (retval > 0)
					break;
				else if (errno != EAGAIN)
				{
					// oops! pipe is broken
					watchdog_print(val[0], val[1], val[2],
						"Signal information incomplete!");
					return 1;
				}
				
				// pipe empty, sleep some
				usleep(1000 * 1000);
			}
		}
		
		// OK, we now have the basic information, try reading the stack trace
		char src;
		char *dst = stack;
		while (true)
		{
			retval = read(g_handler_pipe[0], &src, 1);
			if (retval > 0)
			{
				*dst++ = src;
				if (src == 0)
					// reached the terminator, quit
					break;
			}
			else if (errno != EAGAIN)
			{
				// oops! pipe is broken
				*dst = 0;
				watchdog_print(val[0], val[1], val[2], stack);
				return 2;
			}
			
			// pipe empty, sleep some
				usleep(1000 * 1000);
		}
		
		// phew! info about one signal emitted, wait for another one
	}
	
	return 0;
}

void *segfault(void *arg = NULL)
{
	*(int *)0 = 0;
	return NULL;
}

int main(int argc, char *argv[])
{
	int retval = 0;
	
	// start by creating the pipe
	retval = pipe(g_handler_pipe);
	if (retval != 0)
		return retval;
	
	// fork before our process image grows big!
	pid_t pid = fork();
	if (pid == 0)
		// we are the watchdog child
		return watchdog();
	
	// initialize the signal handler spinlock
	pthread_spin_init(&g_handler_lock, PTHREAD_PROCESS_PRIVATE);
	
	// set up game the signal handler
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = game_signal_handler;

	// register our handler, stash away default handler
	for (size_t i = 0; i < g_num_interest; ++i)
	{
		retval = sigaction(g_interest[i], &action, &g_default_actions[i]);
		if (retval != 0)
			return retval;
	}
	
	// spawn some segfaulting threads to try competing for the handler
	/*pthread_t pool[4];
	for (size_t i = 0; i < sizeof(pool) / sizeof(pool[0]); ++i)
	{
		retval = pthread_create(&pool[i], NULL, segfault, NULL);
		if (retval != 0)
			return retval;
	}*/
	
	// also segfault deliberately here!
	segfault();
	
	// we never reach here, but this is what needs to be done for proper cleanup
	// closing the pipe sends a SIGPIPE to the watchdog, which should cause it
	// to die gracefully
	while (pthread_spin_destroy(&g_handler_lock) == EBUSY)
		usleep(10 * 1000);
	close(g_handler_pipe[0]);
	close(g_handler_pipe[1]);
	// wait for the watchdog to die
	waitpid(pid, NULL, 0);
	
	return retval;
}