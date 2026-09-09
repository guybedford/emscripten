/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Nesting and dup under the callback wait. A leaf edge propagates two levels -
 * leaf -> inner epoll -> outer epoll's registration -> the wait on the outer
 * epoll fd - with no blocking and no ASYNCIFY/JSPI. A registration added via a
 * dup of the inner epoll fd is the same instance (Linux eventpoll semantics),
 * and closing the dup does not tear it down. Closing the inner epoll under an
 * outstanding wait on it completes that wait with POLLNVAL.
 */

#include <assert.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int epA, epB, rfd, wfd, fires;

void writer(void* arg) { assert(write(wfd, "x", 1) == 1); }

void on_inner(int fd, int revents, void* ud) {
  assert(fd == epB && revents == POLLNVAL);
  assert(fires == 1);
  printf("done\n");
}

void on_outer(int fd, int revents, void* ud) {
  assert(fd == epA && revents == POLLIN);
  struct epoll_event ev[4];
  assert(epoll_wait(epA, ev, 4, 0) == 1);
  assert(ev[0].data.fd == epB); // the inner epoll, surfaced through nesting
  assert(ev[0].events & EPOLLIN);
  char b[1];
  assert(read(rfd, b, 1) == 1); // drain the leaf
  fires++;
  // A wait on the inner epoll too, then close it: the wait completes as NVAL.
  assert(emscripten_poll_callback(epB, POLLIN, on_inner, 0) > 0);
  close(epB);
}

int main(void) {
  epA = epoll_create1(0);
  epB = epoll_create1(0);
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];

  // Register the leaf through a dup of the inner epoll, then close the dup.
  int epB2 = dup(epB);
  assert(epB2 >= 0 && epB2 != epB);
  struct epoll_event ev = { .events = EPOLLIN };
  ev.data.fd = rfd;
  assert(epoll_ctl(epB2, EPOLL_CTL_ADD, rfd, &ev) == 0);
  assert(close(epB2) == 0);
  ev.data.fd = epB;
  assert(epoll_ctl(epA, EPOLL_CTL_ADD, epB, &ev) == 0); // inner epoll in the outer

  // Wait on the outer epoll, then write after we return: the leaf edge
  // completes the wait through both levels with no stack switch.
  assert(emscripten_poll_callback(epA, POLLIN, on_outer, 0) > 0);
  emscripten_async_call(writer, NULL, 0);
  return 0;
}
