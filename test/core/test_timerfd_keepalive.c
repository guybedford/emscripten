/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Built with EXIT_RUNTIME: an armed timerfd holds the runtime alive after main
 * returns, until it expires (the runtime then exits and the atexit handler sees
 * the expiration). With DISARM, disarming (or closing) it releases the
 * runtime immediately, so the atexit handler sees nothing pending.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>

int tfd;

void at_exit(void) {
  uint64_t n = 0;
#ifdef DISARM
  assert(read(tfd, &n, sizeof(n)) == -1 && errno == EAGAIN);
#else
  assert(read(tfd, &n, sizeof(n)) == 8);
  assert(n == 1);
#endif
  printf("done\n");
}

int main(void) {
  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  assert(tfd >= 0);
  atexit(at_exit);
  struct itimerspec its = {.it_value = {.tv_nsec = 20 * 1000000}};
  assert(timerfd_settime(tfd, 0, &its, NULL) == 0);
#ifdef DISARM
  // A second armed fd is released by close() rather than settime().
  int other = timerfd_create(CLOCK_MONOTONIC, 0);
  assert(timerfd_settime(other, 0, &its, NULL) == 0);
  assert(close(other) == 0);
  struct itimerspec zero = {0};
  assert(timerfd_settime(tfd, 0, &zero, NULL) == 0);
#endif
  return 0;
}
