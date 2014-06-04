// Simple example of raising priority in Linux
// To build:				gcc niceness.c -o niceness
// To grant capability – as root:	setcap cap_sys_resource+eip niceness
// To run:				./niceness <new niceness>	# where new niceness is in [-20, 19] range

#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

int main(int argc, char *argv[])
{
	int niceness = atoi(argv[1]);

	// allow setting negative niceness values
	// the formula for allowed niceness is 20 - rlim_cur, so by setting 40 we get the full range of [-20, 19]
	struct rlimit rlim;
	rlim.rlim_cur = 40;
	rlim.rlim_max = RLIM_INFINITY;

	if (setrlimit(RLIMIT_NICE, &rlim) == 0)
		printf("Resource limit set\n");
	else
	{
		printf("Failed to set resource limit: 0x%X\n"
			"Make sure the binary has the capabilities by running as root:\n"
			"# setcap cap_sys_resource+eip %s\n",
			errno, argv[0]);
		return 1;
	}

	// because of a Linux peculiarity (see manual for nice(2)), reliable error detection is only possible via errno
	errno = 0;
	// this call actually returns the new niceness, which may have been clamped, so read it
	niceness = nice(niceness);
	if (errno == 0)
		printf("Successfully set niceness to %d\n", niceness);
	else
		printf("Failed to set new niceness: 0x%X\n", errno);
	return errno;
}
