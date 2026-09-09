/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * A blocking epoll_wait() (suspended under ASYNCIFY/JSPI) and a callback wait on
 * the SAME epoll fd. Both are consumers on the epoll's wait-queue, so a readiness
 * edge wakes both - but they share ONE ready list, which is consumed rather than
 * copied. So they take DISJOINT slices: no edge is ever delivered twice, and
 * together they cover the whole ready set. This mirrors Linux, where multiple
 * waiters on one epoll pull different items off the shared rdllist.
 *
 * The split is deterministic: the blocking wait's waiter runs synchronously in
 * the producer's stack and drains the ready list immediately, so it wins the one
 * edge ready at the instant it is woken; whatever became ready afterwards is left
 * on the shared list for the callback's deferred tick. What is NOT guaranteed is
 * the relative order of the two completions, so "done" is reported once both
 * slices have arrived, whichever lands last.
 */

#include <assert.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int ep, rfd[3], wfd[3];
int seen[3];      // which fds have been delivered, across BOTH consumers
int done_printed; // guard: report "done" exactly once

int idx(int fd) {
  for (int i = 0; i < 3; i++) if (rfd[i] == fd) return i;
  return -1;
}

void maybe_done(void) {
  if (seen[0] && seen[1] && seen[2] && !done_printed) {
    done_printed = 1;
    printf("done\n");
  }
}

void make_ready(void* arg) {
  // Runs after epoll_wait has suspended. The first write wakes the blocking
  // wait, which drains synchronously and resolves with just the one fd ready at
  // that instant; the next two edges land on the shared ready list, with no
  // blocking waiter left to take them, for the callback's tick.
  for (int i = 0; i < 3; i++) assert(write(wfd[i], "x", 1) == 1);
}

void on_ready(int fd, int revents, void* ud) {
  assert(fd == ep && revents == POLLIN);
  struct epoll_event ev[8];
  int n = epoll_wait(ep, ev, 8, 0); // collect our slice off the shared list
  for (int k = 0; k < n; k++) {
    int i = idx(ev[k].data.fd);
    assert(i >= 0 && !seen[i]); // disjoint: never an fd the blocking wait took
    seen[i] = 1;
  }
  maybe_done();
}

int main(void) {
  ep = epoll_create1(0);
  for (int i = 0; i < 3; i++) {
    int p[2];
    assert(pipe(p) == 0);
    rfd[i] = p[0];
    wfd[i] = p[1];
    // Edge-triggered: each readiness is reported once, so "delivered to exactly
    // one consumer" is unambiguous (no level re-cycling between the two).
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET };
    ev.data.fd = rfd[i];
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, rfd[i], &ev) == 0);
  }

  // Arm the callback wait and schedule the writes, then block. Both consumers
  // are now on the epoll's wait-queue with an empty ready list.
  assert(emscripten_poll_callback(ep, POLLIN, on_ready, 0) > 0);
  emscripten_async_call(make_ready, NULL, 0);

  struct epoll_event out[8];
  int n = epoll_wait(ep, out, 8, -1); // ASYNCIFY/JSPI: suspends until readiness
  // Woken on the first edge, the blocking wait sees only what was ready then -
  // exactly one fd, not the whole burst that arrived after it drained.
  assert(n == 1);
  int wi = idx(out[0].data.fd);
  assert(wi >= 0 && !seen[wi]);
  seen[wi] = 1;

  // The outstanding callback wait keeps the runtime alive and delivers the
  // remaining two off the shared list; "done" prints once both slices are in.
  maybe_done();
  return 0;
}
