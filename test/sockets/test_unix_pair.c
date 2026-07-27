/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * socketpair(AF_UNIX, SOCK_STREAM): a connected, unnamed pair. Exercises
 * creation, getsockname/getpeername (unnamed ends), non-blocking would-block
 * on an empty read, both transfer directions, half-close via shutdown(SHUT_WR)
 * (peer EOF while the other direction stays open), and write-after-shutdown ->
 * EPIPE. Plain POSIX, so it also builds and runs natively against the host
 * stack.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

int fd[2];
enum { PING_WAIT, PONG_WAIT, EOF_WAIT, LAST_WAIT } state = PING_WAIT;

void set_nonblocking(int f) {
  fcntl(f, F_SETFL, O_NONBLOCK);
}

void test_success(void) {
  printf("done\n");
  close(fd[0]);
  close(fd[1]);
#ifdef __EMSCRIPTEN__
  emscripten_cancel_main_loop();
#else
  exit(0);
#endif
}

void main_loop(void) {
  fd_set fdr;
  struct timeval tv = {0};
  FD_ZERO(&fdr);
  FD_SET(fd[0], &fdr);
  FD_SET(fd[1], &fdr);
  select(64, &fdr, NULL, NULL, &tv);

  char buf[8];
  ssize_t n;
  switch (state) {
    case PING_WAIT:
      // fd[1] receives the ping and echoes a pong.
      if (!FD_ISSET(fd[1], &fdr)) return;
      n = recv(fd[1], buf, sizeof(buf), 0);
      if (n < 0 && errno == EAGAIN) return;
      assert(n == 4 && memcmp(buf, "ping", 4) == 0 && "ping");
      assert(send(fd[1], "pong", 4, 0) == 4);
      state = PONG_WAIT;
      return;
    case PONG_WAIT:
      // fd[0] receives the pong, then half-closes its write side.
      if (!FD_ISSET(fd[0], &fdr)) return;
      n = recv(fd[0], buf, sizeof(buf), 0);
      if (n < 0 && errno == EAGAIN) return;
      assert(n == 4 && memcmp(buf, "pong", 4) == 0 && "pong");
      assert(shutdown(fd[0], SHUT_WR) == 0);
      state = EOF_WAIT;
      return;
    case EOF_WAIT:
      // fd[1] sees EOF from the half-close, but its own write side stays
      // open: the reverse direction still delivers.
      if (!FD_ISSET(fd[1], &fdr)) return;
      n = recv(fd[1], buf, sizeof(buf), 0);
      if (n < 0 && errno == EAGAIN) return;
      assert(n == 0 && "EOF after peer shutdown(SHUT_WR)");
      assert(send(fd[1], "last", 4, 0) == 4 && "half-open write");
      state = LAST_WAIT;
      return;
    case LAST_WAIT:
      if (!FD_ISSET(fd[0], &fdr)) return;
      n = recv(fd[0], buf, sizeof(buf), 0);
      if (n < 0 && errno == EAGAIN) return;
      assert(n == 4 && memcmp(buf, "last", 4) == 0 && "last");
      // Writing on the shut-down side is a broken pipe.
      n = send(fd[0], "x", 1, 0);
      assert(n == -1 && errno == EPIPE && "EPIPE after shutdown(SHUT_WR)");
      test_success();
      return;
  }
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  // Only AF_UNIX pairs exist.
  int bad[2];
  assert(socketpair(AF_INET, SOCK_STREAM, 0, bad) == -1);
#ifdef __EMSCRIPTEN__
  assert(errno == EOPNOTSUPP);
  // Datagram pairs are not supported by the node backend.
  assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, bad) == -1);
  assert(errno == EPROTONOSUPPORT);
#endif

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  set_nonblocking(fd[0]);
  set_nonblocking(fd[1]);

  // Both ends are connected but unnamed.
  struct sockaddr_un sa;
  socklen_t sl = sizeof(sa);
  assert(getsockname(fd[0], (struct sockaddr*)&sa, &sl) == 0);
  assert(sa.sun_family == AF_UNIX);
  sl = sizeof(sa);
  assert(getpeername(fd[0], (struct sockaddr*)&sa, &sl) == 0);
  assert(sa.sun_family == AF_UNIX);

  // Nothing has been sent yet: an empty non-blocking read would-blocks.
  char buf[8];
  assert(recv(fd[1], buf, sizeof(buf), 0) == -1 && errno == EAGAIN);

  assert(send(fd[0], "ping", 4, 0) == 4);

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(main_loop, 0, 0);
#else
  while (1) {
    main_loop();
    usleep(1000);
  }
#endif
  return 0;
}
