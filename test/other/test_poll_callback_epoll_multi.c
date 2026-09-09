/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Multiple waits on one epoll fd behave as multiple blocking epoll_wait callers:
 * both complete when the set is ready, and the collectors race over the one
 * shared ready list, so each event is collected exactly once. Two waits each
 * collecting one event split two ready fds one each. A wait that keeps
 * collecting one per callback and re-arming drains the rest across ticks - no
 * app loop is needed to re-call it, only the re-arm.
 */

#include <assert.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int ep, rfd[3];
int seen[3];
int fires_a, fires_b, collected;

int idx(int fd) {
  for (int i = 0; i < 3; i++) if (rfd[i] == fd) return i;
  return -1;
}

void collect(void) {
  struct epoll_event ev[1];
  int n = epoll_wait(ep, ev, 1, 0); // collect at most one per fire
  assert(n == 1); // a wait only completes with something to collect
  int i = idx(ev[0].data.fd);
  assert(i >= 0 && !seen[i]); // disjoint: each fd collected exactly once
  seen[i] = 1;
  char b[1];
  assert(read(rfd[i], b, 1) == 1); // drain so it is no longer ready
  collected++;
}

void waiter_a(int fd, int revents, void* ud) {
  if (revents == POLLNVAL) return; // the epoll was closed under the final wait
  fires_a++;
  collect();
  // Keep draining one per tick until the set is empty (then this wait stays
  // outstanding until closed below).
  assert(emscripten_poll_callback(ep, POLLIN, waiter_a, 0) > 0);
}
void waiter_b(int fd, int revents, void* ud) { fires_b++; collect(); }

void check(void* ud) {
  // Both waits completed on the same readiness; B took one event, A took the
  // other two across two ticks. A's third wait is still outstanding: closing the
  // epoll completes it with POLLNVAL, releasing the runtime.
  assert(collected == 3 && seen[0] && seen[1] && seen[2]);
  assert(fires_a == 2 && fires_b == 1);
  printf("done\n");
  close(ep);
}

int main(void) {
  ep = epoll_create1(0);
  for (int i = 0; i < 3; i++) {
    int p[2];
    assert(pipe(p) == 0);
    rfd[i] = p[0];
    assert(write(p[1], "x", 1) == 1); // read end readable (level)
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.fd = rfd[i];
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, rfd[i], &ev) == 0);
  }

  assert(emscripten_poll_callback(ep, POLLIN, waiter_a, 0) > 0);
  assert(emscripten_poll_callback(ep, POLLIN, waiter_b, 0) > 0);
  // All three fds are already ready: A's tick collects one and re-arms, B's
  // collects one, A's second tick collects the last; a macrotask then checks.
  emscripten_async_call(check, NULL, 0);
  return 0;
}
