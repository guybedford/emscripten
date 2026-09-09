/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * A wait armed from a pthread is delivered on that pthread, and holds it alive:
 * the thread arms and returns from its entry point straight away, with nothing
 * else keeping it open, and must still be there when the readiness arrives
 * later (from a write on a third thread, proxied through the main thread). The
 * hold is released on delivery, so the thread then exits and the join returns.
 */

#include <assert.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int rfd, wfd;
pthread_t waiter_thread;

void on_ready(int fd, int revents, void* ud) {
  assert(pthread_equal(pthread_self(), waiter_thread)); // delivered on the arming thread
  assert(fd == rfd && revents == POLLIN);
  char b[1];
  assert(read(rfd, b, 1) == 1);
  printf("done\n");
}

void* waiter(void* arg) {
  assert(emscripten_poll_callback(rfd, POLLIN, on_ready, 0) > 0);
  return NULL; // the outstanding wait keeps this thread alive
}

void* writer(void* arg) {
  usleep(100 * 1000);
  assert(write(wfd, "x", 1) == 1);
  return NULL;
}

int main(void) {
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];
  pthread_t w;
  assert(pthread_create(&waiter_thread, NULL, waiter, NULL) == 0);
  assert(pthread_create(&w, NULL, writer, NULL) == 0);
  assert(pthread_join(w, NULL) == 0);
  assert(pthread_join(waiter_thread, NULL) == 0);
  return 0;
}
