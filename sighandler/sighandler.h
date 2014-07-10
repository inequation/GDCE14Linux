#pragma once

// installs the signal handler and tries to enable core dumps of given max size
// in bytes
// all bits set to 1 (i.e. -1 cast to size_t) means unlimited
int sighandler_install(size_t max_core_size = (size_t)-1);

void sighandler_cleanup();
