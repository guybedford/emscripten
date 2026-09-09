/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

// emscripten_poll_callback: poll(2) on one fd with a callback (see
// _emscripten_poll_callback_js in libeventloop.js). An outstanding wait is a
// pending async operation, so it holds the calling thread's runtime keepalive
// from arm until its callback runs or it is cancelled. The hold is taken here,
// on the calling thread, before the arming call returns - with pthreads the
// wait itself is armed on the main thread (the FS lives there), and a hold
// posted back asynchronously could land after this thread had already decided
// to exit. It is released here too, on the same thread, as the callback is
// delivered.

#include <stdlib.h>

#include <emscripten/eventloop.h>

#include "emscripten_internal.h"

int emscripten_poll_callback(int fd,
                             int events,
                             em_poll_callback callback,
                             void* userdata) {
  int id = _emscripten_poll_callback_js(fd, events, callback, userdata);
  if (id > 0) {
    emscripten_runtime_keepalive_push();
  }
  return id;
}

int emscripten_poll_callback_cancel(int id) {
  int rc = _emscripten_poll_callback_cancel_js(id);
  if (rc == 0) {
    emscripten_runtime_keepalive_pop();
  }
  return rc;
}

// Runs on the thread that armed the wait, from its event loop.
void _emscripten_poll_callback_deliver(em_poll_callback callback,
                                       int fd,
                                       int revents,
                                       void* userdata) {
  emscripten_runtime_keepalive_pop();
  callback(fd, revents, userdata);
}

#ifdef __EMSCRIPTEN_PTHREADS__

#include <emscripten/proxying.h>

typedef struct poll_callback_args_t {
  em_poll_callback callback;
  int fd;
  int revents;
  void* userdata;
} poll_callback_args_t;

static void do_poll_callback(void* arg) {
  poll_callback_args_t* args = arg;
  _emscripten_poll_callback_deliver(
    args->callback, args->fd, args->revents, args->userdata);
  free(args);
}

// Like _emscripten_run_callback_on_thread (html5), but a target thread that
// has already exited is not an error: its hold went with it, so the wait is
// simply dropped.
void _emscripten_poll_callback_on_thread(
  pthread_t t, em_poll_callback callback, int fd, int revents, void* userdata) {
  poll_callback_args_t* args = malloc(sizeof(*args));
  args->callback = callback;
  args->fd = fd;
  args->revents = revents;
  args->userdata = userdata;
  if (!emscripten_proxy_async(
        emscripten_proxy_get_system_queue(), t, do_poll_callback, args)) {
    free(args);
  }
}

#endif
