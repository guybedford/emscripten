/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * A JS exception escaping an event loop callback (a timer, or with -DEPOLL an
 * epoll listener delivery) aborts the program. The host keeps running past
 * the uncaught exception, but no later callback reaches the program.
 */

#include <sys/epoll.h>
#include <emscripten.h>
#include <emscripten/epoll.h>
#include <emscripten/eventloop.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>

void later(void* arg) {
  printf("callback after abort\n");
}

void fail(void) {
  emscripten_set_timeout(later, 0, NULL);
  EM_ASM({ throw new Error('boom'); });
}

void on_timeout(void* arg) {
  fail();
}

void on_ready(int epfd, void* arg) {
  fail();
}

int main() {
  EM_ASM({
    process.on('uncaughtException', (e) => setTimeout(() => out('done'), 10));
  });
#ifdef EPOLL
  int p[2];
  assert(pipe(p) == 0);
  int ep = epoll_create1(0);
  struct epoll_event ev = { .events = EPOLLIN };
  ev.data.fd = p[0];
  assert(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
  assert(emscripten_epoll_listener_add(ep, on_ready, 0) == 0);
  assert(write(p[1], "x", 1) == 1);
  emscripten_runtime_keepalive_push();
#else
  emscripten_set_timeout(on_timeout, 0, NULL);
#endif
  return 0;
}
