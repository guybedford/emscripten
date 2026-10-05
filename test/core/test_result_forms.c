/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Result forms (`_fd` / `_promise` aliases) of one asynchronous library
 * function, defined in test_result_forms.js: `answer_fd()` yields an fd readable once settled
 * whose read() is the value; `answer_promise()` an em_promise_t fulfilled with
 * it; `answer()` the value itself where the stack can wait (ASYNCIFY/JSPI, or
 * a pthread), else what the body returns when told it cannot.
 */

#include <assert.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/promise.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

intptr_t answer(int ms, intptr_t value);
int answer_fd(int ms, intptr_t value);
em_promise_t answer_promise(int ms, intptr_t value);

int stage;

int readable(int fd, short expect) {
  struct pollfd p = { .fd = fd, .events = POLLIN };
  int n = poll(&p, 1, 0);
  assert(n == 0 || (n == 1 && p.revents == expect));
  return n;
}

intptr_t take(int fd) {
  intptr_t v;
  assert(read(fd, &v, sizeof v) == sizeof v);
  assert(close(fd) == 0);
  return v;
}

em_promise_result_t fail(void** result, void* data, void* value) {
  assert(0 && "promise rejected");
}

em_promise_result_t on_rejected(void** result, void* data, void* value) {
  assert(stage++ == 1);
  printf("done\n");
#ifdef __EMSCRIPTEN_PTHREADS__
  exit(0);
#endif
  return EM_PROMISE_FULFILL;
}

em_promise_result_t on_fulfilled(void** result, void* data, void* value) {
  assert(stage++ == 0);
  assert((intptr_t)value == 42);
  // A rejected promise.
  em_promise_t p = answer_promise(5, 0);
  em_promise_t next = emscripten_promise_then(p, fail, on_rejected, NULL);
  emscripten_promise_destroy(p);
  emscripten_promise_destroy(next);
  return EM_PROMISE_FULFILL;
}

void check_fd(void* arg) {
  int fd = (int)(intptr_t)arg;
  if (!readable(fd, POLLIN)) {
    emscripten_set_timeout(check_fd, 1, arg);
    return;
  }
  assert(take(fd) == 42);

  // Rejection: readable with POLLERR, read() fails with EIO.
  fd = answer_fd(0, 7);
  assert(readable(fd, POLLIN));
  assert(take(fd) == 7);

  em_promise_t p = answer_promise(5, 42);
  em_promise_t next = emscripten_promise_then(p, on_fulfilled, fail, NULL);
  emscripten_promise_destroy(p);
  emscripten_promise_destroy(next);
}

int main() {
  // Synchronous completion is readable on return.
  int fd = answer_fd(0, 7);
  assert(readable(fd, POLLIN));
  assert(take(fd) == 7);

  // A dup shares the result.
  fd = answer_fd(0, 7);
  int d = dup(fd);
  assert(close(fd) == 0);
  assert(take(d) == 7);

  // Rejected: POLLERR, EIO.
  fd = answer_fd(0, 0);
  assert(take(fd) == 0);

  // Pending until the timer fires; read() before then is EAGAIN.
  fd = answer_fd(5, 42);
  intptr_t v;
  assert(!readable(fd, POLLIN));
  assert(read(fd, &v, sizeof v) == -1 && errno == EAGAIN);

  // Closing a pending fd drops its result.
  assert(close(answer_fd(5, 1)) == 0);

  // The synchronous variant.
  assert(answer(0, 7) == 7);
#if defined(__EMSCRIPTEN_PTHREADS__) || defined(ASYNC)
  assert(answer(5, 42) == 42);
#else
  // Where the stack cannot wait, the body is told so.
  assert(answer(5, 42) == -1);
#endif

  emscripten_set_timeout(check_fd, 1, (void*)(intptr_t)fd);
  return 0;
}
