/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Errors and cancellation: -EBADF for a bad fd; a cancelled wait never fires and
 * releases its runtime hold (so the process exits with nothing outstanding);
 * cancelling twice, or cancelling a wait that already completed, is -ENOENT -
 * and the completed wait's callback is still delivered.
 */

#include <assert.h>
#include <emscripten/eventloop.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int rfd, wfd, late_id;

void never(int fd, int revents, void* ud) { assert(0 && "cancelled wait fired"); }

void late(int fd, int revents, void* ud) {
  // Completed (the fd was ready at arm time) before main cancelled it: the
  // cancel found nothing, and the delivery still happens.
  assert(fd == rfd && (revents & POLLIN));
  printf("done\n");
}

int main(void) {
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];

  assert(emscripten_poll_callback(999, POLLIN, never, 0) == -EBADF);
  assert(emscripten_poll_callback_cancel(0) == -ENOENT);
  assert(emscripten_poll_callback_cancel(12345) == -ENOENT);

  // Arm, cancel, then make the fd ready: nothing fires.
  int id = emscripten_poll_callback(rfd, POLLIN, never, 0);
  assert(id > 0);
  assert(emscripten_poll_callback_cancel(id) == 0);
  assert(emscripten_poll_callback_cancel(id) == -ENOENT);
  // Two outstanding waits are distinct ids even for the same (fd, callback).
  int a = emscripten_poll_callback(rfd, POLLIN, never, 0);
  int b = emscripten_poll_callback(rfd, POLLIN, never, 0);
  assert(a > 0 && b > a);
  assert(emscripten_poll_callback_cancel(b) == 0);
  assert(emscripten_poll_callback_cancel(a) == 0);
  assert(write(wfd, "x", 1) == 1);

  // Already readable: the wait completes at arm time (delivery deferred), so a
  // cancel right after finds nothing, yet the callback still runs.
  late_id = emscripten_poll_callback(rfd, POLLIN, late, 0);
  assert(late_id > 0);
  assert(emscripten_poll_callback_cancel(late_id) == -ENOENT);
  return 0;
}
