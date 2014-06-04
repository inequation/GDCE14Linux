// Valgrind Memcheck error detection demo
// To compile with libc malloc():	gcc valgrind-example.c -g -o valgrind-example
// To compile with dlmalloc:		gcc valgrind-example.c -g -DDLMALLOC -o valgrind-example
// To run:				valgrind ./valgrind-example

#ifdef DLMALLOC
	#define MSPACES 1
	#define ONLY_MSPACES 1
	#define USE_LOCKS 0
	#define HAVE_VALGRIND_VALGRIND_H
	#define HAVE_VALGRIND_MEMCHECK_H
	#include "malloc.c"
#else
	#include <stdlib.h>
#endif

int main(int argc, char *argv[])
{
	int foo;
	int *ptr1 = &foo;
	int *ptr2 = malloc(sizeof(int));	// leak
	int *ptr3 = malloc(2 * sizeof(int)); free(ptr3);
	
	if (*ptr1)	// jump depends on uninitialized value
		ptr2[1] = 0xabad1dea;	// invalid write
	else
		ptr2[1] = 0x15bad700;	// invalid write
	ptr2[0] = ptr2[2];	// invalid read
	ptr2[0] = ptr3[1];	// read after free
	ptr3[0] = ptr2[0];	// write after free
	return *ptr1;	// invalid syscall param
}

