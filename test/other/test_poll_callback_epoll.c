/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Non-blocking epoll_wait via emscripten_poll_callback on the epoll fd
 * (readable when its set has ready events), with no ASYNCIFY/JSPI: the callback
 * collects the events itself with a zero-timeout epoll_wait and re-arms. Arming
 * a registration is itself an event source, as on Linux, where the set becomes
 * ready with no producer wakeup to follow:
 *   - EPOLL_CTL_ADD of an already-readable fd completes the wait.
 *   - EPOLL_CTL_MOD re-arming a still-readable EPOLLONESHOT fd completes it again.
 * Not re-arming leaves nothing outstanding and the runtime exits.
 */

#include <assert.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int ep, rfd, wfd, fires;

void arm_rfd(int op) {
  struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT };
  ev.data.u32 = 0x1234;
  assert(epoll_ctl(ep, op, rfd, &ev) == 0);
}

void on_ready(int fd, int revents, void* ud) {
  assert(fd == ep && revents == POLLIN && (long)ud == 42);
  struct epoll_event events[4];
  int nready = epoll_wait(ep, events, 4, 0);
  assert(nready == 1);
  assert(events[0].events & EPOLLIN);
  assert(events[0].data.u32 == 0x1234);
  fires++;

  if (fires == 1) {
    // EPOLLONESHOT disabled the registration on this delivery, but the byte is
    // still in the pipe (level-readable). Wait again, then re-arm with MOD
    // WITHOUT draining: only the MOD poke can re-evaluate readiness.
    assert(emscripten_poll_callback(ep, POLLIN, on_ready, ud) > 0);
    arm_rfd(EPOLL_CTL_MOD);
    return;
  }

  assert(fires == 2);
  char b[1];
  assert(read(rfd, b, 1) == 1);
  // Make the set ready again with no wait outstanding: nothing fires, and the
  // runtime exits cleanly.
  assert(write(wfd, "x", 1) == 1);
  arm_rfd(EPOLL_CTL_MOD);
  printf("done\n");
}

int main(void) {
  ep = epoll_create1(0);
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];

  // Wait on an empty set: nothing ready, no fire.
  assert(emscripten_poll_callback(ep, POLLIN, on_ready, (void*)42) > 0);

  // Make rfd readable, then ADD it. The fd is already ready with no producer
  // wakeup to come, so the ADD itself must complete the wait.
  assert(write(wfd, "x", 1) == 1);
  arm_rfd(EPOLL_CTL_ADD);
  return 0;
}
