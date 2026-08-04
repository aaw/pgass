#include "platform.h"

#ifndef _WIN32
#include <sys/resource.h>
#endif

void raise_stack_limit() {
#ifndef _WIN32
  rlimit limit;
  if (getrlimit(RLIMIT_STACK, &limit) != 0) return;
  if (limit.rlim_cur == limit.rlim_max) return;
  limit.rlim_cur = limit.rlim_max;
  setrlimit(RLIMIT_STACK, &limit);
#endif
}
