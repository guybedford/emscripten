/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * A blocking epoll_wait() and poll() that suspend the wasm stack
 * (ASYNCIFY/JSPI) and are woken by a timerfd expiring.
 */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

int main(void) {
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  assert(tfd >= 0);
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN};
  ev.data.u32 = 0xabcd;
  assert(epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) == 0);

  struct itimerspec its = {.it_value = {.tv_nsec = 10 * 1000000}};
  assert(timerfd_settime(tfd, 0, &its, NULL) == 0);

  struct epoll_event out[4];
  int n = epoll_wait(ep, out, 4, -1);
  assert(n == 1);
  assert(out[0].events & EPOLLIN);
  assert(out[0].data.u32 == 0xabcd);

  uint64_t count = 0;
  assert(read(tfd, &count, sizeof(count)) == 8);
  assert(count == 1);

  // Re-arm and block in poll() this time.
  assert(timerfd_settime(tfd, 0, &its, NULL) == 0);
  struct pollfd pfd = {.fd = tfd, .events = POLLIN};
  assert(poll(&pfd, 1, -1) == 1);
  assert(pfd.revents == POLLIN);
  assert(read(tfd, &count, sizeof(count)) == 8);
  assert(count == 1);

  printf("done\n");
  return 0;
}
