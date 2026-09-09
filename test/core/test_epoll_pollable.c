/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * An epoll fd is itself pollable, as on Linux (ep_eventpoll_poll): poll() and
 * select() report it readable exactly when epoll_wait() would return events,
 * and never writable. Zero-timeout probes cover not-ready, ready, and drained;
 * under pthreads a blocking poll() on the epoll fd is woken by a leaf edge from
 * another thread. This is the composition emscripten_poll_callback builds on.
 */

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN_PTHREADS__
#include <pthread.h>
#endif

int ep, rfd, wfd;

int probe_poll(int events) {
  struct pollfd p = { .fd = ep, .events = events };
  int n = poll(&p, 1, 0);
  assert(n >= 0);
  return n ? p.revents : 0;
}

int probe_select(void) {
  fd_set r, w;
  FD_ZERO(&r);
  FD_ZERO(&w);
  FD_SET(ep, &r);
  FD_SET(ep, &w);
  struct timeval tv = {0, 0};
  int n = select(ep + 1, &r, &w, NULL, &tv);
  assert(n >= 0);
  assert(!FD_ISSET(ep, &w)); // an epoll fd is never writable
  return FD_ISSET(ep, &r);
}

#ifdef __EMSCRIPTEN_PTHREADS__
void* writer(void* arg) {
  usleep(100 * 1000);
  assert(write(wfd, "y", 1) == 1);
  return NULL;
}
#endif

int main(void) {
  ep = epoll_create1(0);
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];
  struct epoll_event ev = { .events = EPOLLIN };
  ev.data.fd = rfd;
  assert(epoll_ctl(ep, EPOLL_CTL_ADD, rfd, &ev) == 0);

  // Nothing ready: neither poll() nor select() report the epoll fd.
  assert(probe_poll(POLLIN | POLLOUT) == 0);
  assert(!probe_select());

  // Leaf readable: the epoll fd is readable (and only readable).
  assert(write(wfd, "x", 1) == 1);
  assert(probe_poll(POLLIN | POLLOUT) == POLLIN);
  assert(probe_select());

  // Collect and drain the leaf: back to not ready.
  struct epoll_event out[4];
  assert(epoll_wait(ep, out, 4, 0) == 1 && out[0].data.fd == rfd);
  char b[1];
  assert(read(rfd, b, 1) == 1);
  assert(probe_poll(POLLIN) == 0);
  assert(!probe_select());

#ifdef __EMSCRIPTEN_PTHREADS__
  // Blocking poll() on the epoll fd, woken by a leaf edge from another thread.
  pthread_t t;
  assert(pthread_create(&t, NULL, writer, NULL) == 0);
  struct pollfd pfd = { .fd = ep, .events = POLLIN };
  assert(poll(&pfd, 1, -1) == 1 && pfd.revents == POLLIN);
  assert(epoll_wait(ep, out, 4, 0) == 1 && out[0].data.fd == rfd);
  assert(read(rfd, b, 1) == 1);
  assert(pthread_join(t, NULL) == 0);
#endif

  printf("done\n");
  return 0;
}
