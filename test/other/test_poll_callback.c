/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * emscripten_poll_callback on a pipe: a one-shot, non-blocking readiness
 * wait with no ASYNCIFY/JSPI. The callback fires once with the ready revents;
 * an fd that is already ready still fires deferred (never on the arming stack);
 * waiting again means arming again; closing the waited fd completes the wait
 * with POLLNVAL; and the runtime exits once no wait is outstanding.
 */

#include <assert.h>
#include <emscripten/eventloop.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int rfd, wfd, fires, armed_returned;

void on_writable(int fd, int revents, void* ud);

void on_ready(int fd, int revents, void* ud) {
  assert(fd == rfd);
  assert((long)ud == 42);
  assert(armed_returned); // never delivered inside the arming call
  fires++;
  if (fires == 1) {
    assert(revents == POLLIN);
    // Leave the byte unread: the fd is already readable when we arm again, so
    // this second wait must still be delivered on a later tick.
    armed_returned = 0;
    assert(emscripten_poll_callback(rfd, POLLIN, on_ready, ud) > 0);
    armed_returned = 1;
    return;
  }
  if (fires == 2) {
    assert(revents == POLLIN);
    char b[1];
    assert(read(rfd, b, 1) == 1);
    // Wait for the write end (always writable): fires immediately (deferred).
    armed_returned = 0;
    assert(emscripten_poll_callback(wfd, POLLOUT, on_writable, ud) > 0);
    armed_returned = 1;
    return;
  }
  assert(fires == 4);
  // Closing the waited fd completes the wait with POLLNVAL.
  assert(revents == POLLNVAL);
  printf("done\n");
}

void on_writable(int fd, int revents, void* ud) {
  assert(fd == wfd);
  assert(revents == POLLOUT);
  assert(armed_returned);
  fires++;
  assert(fires == 3);
  armed_returned = 0;
  assert(emscripten_poll_callback(rfd, POLLIN, on_ready, ud) > 0);
  armed_returned = 1;
  close(rfd);
}

int main(void) {
  int p[2];
  assert(pipe(p) == 0);
  rfd = p[0];
  wfd = p[1];
  assert(emscripten_poll_callback(rfd, POLLIN, on_ready, (void*)42) > 0);
  armed_returned = 1;
  assert(write(wfd, "x", 1) == 1);
  return 0;
}
