/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Per-fd trigger modes behave under the callback wait exactly as under
 * epoll_wait, because the app drives each wait by re-arming:
 *   - Level (EPOLLOUT on a pipe write end, always writable): every re-arm
 *     completes again at once. The loop is the app's, so the app stops it.
 *   - EPOLLET: after a delivery with the fd left undrained, a re-armed wait does
 *     NOT complete until a fresh edge (a new write) - not while the fd merely
 *     stays readable.
 */

#include <assert.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int ep_level, ep_edge, rfd, wfd, level_fires, edge_fires;

void on_level(int fd, int revents, void* ud) {
  struct epoll_event ev[4];
  assert(epoll_wait(ep_level, ev, 4, 0) == 1 && (ev[0].events & EPOLLOUT));
  if (++level_fires < 3) {
    // Nothing changed and nothing was drained: re-arming completes again.
    assert(emscripten_poll_callback(ep_level, POLLIN, on_level, 0) > 0);
  }
}

void second_edge(void* arg) {
  // The fd stayed readable the whole time (fire 1 did not drain it) and a wait
  // has been outstanding, yet it did not complete: once per edge.
  assert(edge_fires == 1);
  assert(write(wfd, "y", 1) == 1); // a fresh edge -> exactly one more delivery
}

void on_edge(int fd, int revents, void* ud) {
  struct epoll_event ev[4];
  assert(epoll_wait(ep_edge, ev, 4, 0) == 1 && ev[0].data.fd == rfd);
  edge_fires++;
  if (edge_fires == 1) {
    // Do NOT drain; re-arm; check it stays silent, then poke a fresh edge.
    assert(emscripten_poll_callback(ep_edge, POLLIN, on_edge, 0) > 0);
    emscripten_async_call(second_edge, NULL, 0);
    return;
  }
  assert(edge_fires == 2);
  char b[2];
  assert(read(rfd, b, 2) == 2); // drain both bytes
  assert(level_fires == 3);
  printf("done\n");
}

int main(void) {
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];

  ep_level = epoll_create1(0);
  struct epoll_event ev = { .events = EPOLLOUT }; // level; a write end is always writable
  ev.data.fd = wfd;
  assert(epoll_ctl(ep_level, EPOLL_CTL_ADD, wfd, &ev) == 0);
  assert(emscripten_poll_callback(ep_level, POLLIN, on_level, 0) > 0);

  ep_edge = epoll_create1(0);
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = rfd;
  assert(epoll_ctl(ep_edge, EPOLL_CTL_ADD, rfd, &ev) == 0);
  assert(emscripten_poll_callback(ep_edge, POLLIN, on_edge, 0) > 0);
  assert(write(wfd, "x", 1) == 1); // first edge
  return 0;
}
