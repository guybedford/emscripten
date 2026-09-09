/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Callback waits completed by real socket readiness (arriving UDP datagrams)
 * through the SOCKFS -> wait-queue bridge, with no blocking call and no
 * ASYNCIFY/JSPI - once directly on the socket fd, once via an epoll fd watching
 * it. Each datagram is a separate producer event completing a freshly armed
 * wait. With pthreads the readiness is tracked on the main thread (where the
 * syscalls are proxied) but each delivery runs on the thread that armed it,
 * which the outstanding wait holds alive until then.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <emscripten/eventloop.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

int ep, rx, tx, fires;
struct sockaddr_in addr;

void send_one(const char* msg) {
  assert(sendto(tx, msg, 4, 0, (struct sockaddr*)&addr, sizeof addr) == 4);
}

void on_ready(int fd, int revents, void* ud) {
  char b[4];
  fires++;
  if (fires == 1) {
    assert(fd == rx && (revents & POLLIN));
    assert(recv(rx, b, 4, 0) == 4 && memcmp(b, "one\0", 4) == 0);
    // Now wait through an epoll watching the socket instead.
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.fd = rx;
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, rx, &ev) == 0);
    assert(emscripten_poll_callback(ep, POLLIN, on_ready, 0) > 0);
    send_one("two");
    return;
  }
  assert(fires == 2);
  assert(fd == ep && revents == POLLIN);
  struct epoll_event ev[4];
  assert(epoll_wait(ep, ev, 4, 0) == 1 && ev[0].data.fd == rx);
  assert(recv(rx, b, 4, 0) == 4 && memcmp(b, "two\0", 4) == 0);
  printf("done\n");
  // Nothing outstanding: the runtime (and under PROXY_TO_PTHREAD, the thread
  // that armed the waits) is released and the process exits.
  close(ep);
  close(rx);
  close(tx);
}

int main(void) {
  ep = epoll_create1(0);
  rx = socket(AF_INET, SOCK_DGRAM, 0);
  tx = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET; addr.sin_port = htons(0);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  assert(bind(rx, (struct sockaddr*)&addr, sizeof addr) == 0);
  socklen_t l = sizeof addr;
  assert(getsockname(rx, (struct sockaddr*)&addr, &l) == 0);

  // Arm on the socket itself, then send; the datagram arrives after we return.
  assert(emscripten_poll_callback(rx, POLLIN, on_ready, 0) > 0);
  send_one("one");
  return 0;
}
