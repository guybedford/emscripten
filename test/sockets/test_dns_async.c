/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * emscripten_dns_lookup_async(): a getaddrinfo() that never blocks, returning
 * a promise settled on the calling thread. Numeric addresses and errors still
 * settle asynchronously; a hostname completes after a real node:dns lookup
 * (or with a fake address without -sNODERAWSOCKETS).
 */

#include <arpa/inet.h>
#include <assert.h>
#include <emscripten.h>
#include <emscripten/promise.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

int pending;
int returned;
pthread_t caller;

void check_v4(struct addrinfo* res, const char* addr) {
  int n = 0;
  for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
    assert(ai->ai_socktype == SOCK_STREAM);
    assert(ai->ai_protocol == IPPROTO_TCP);
    assert(ai->ai_family == AF_INET);
    struct sockaddr_in* sin = (struct sockaddr_in*)ai->ai_addr;
    assert(ai->ai_addrlen == sizeof(*sin));
    assert(ntohs(sin->sin_port) == 80);
    if (!addr || sin->sin_addr.s_addr == inet_addr(addr)) n++;
  }
  assert(n >= 1);
}

void finish(void) {
  if (--pending) return;
  printf("done\n");
}

// Settled on the calling thread, never on the caller's stack.
em_promise_result_t on_fulfilled(void** result, void* data, void* value) {
  assert(pthread_equal(pthread_self(), caller));
  assert(returned);
  const char* expect = data;
  assert(expect);
  check_v4(value, *expect ? expect : NULL);
  freeaddrinfo(value);
  finish();
  return EM_PROMISE_FULFILL;
}

em_promise_result_t on_rejected(void** result, void* data, void* value) {
  assert(pthread_equal(pthread_self(), caller));
  assert(returned);
  assert(!data);
  assert((int)(intptr_t)value == EAI_SERVICE);
  finish();
  return EM_PROMISE_FULFILL;
}

void start(const char* name, int family, const char* service, const char* expect) {
  struct addrinfo hints = {0};
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  pending++;
  em_promise_t p = emscripten_dns_lookup_async(name, service, &hints);
  // The inputs are consumed before return; the hints are about to go away.
  em_promise_t done = emscripten_promise_then(p, on_fulfilled, on_rejected, (void*)expect);
  emscripten_promise_destroy(done);
  emscripten_promise_destroy(p);
}

int main() {
  caller = pthread_self();
  // A numeric address needs no lookup, but still settles asynchronously.
  start("10.9.8.7", AF_UNSPEC, "80", "10.9.8.7");
  // So does an error.
  start("10.9.8.7", AF_UNSPEC, "http", NULL);
  // A hostname: a real node:dns lookup, or a fake address without it.
#ifdef REAL_DNS
  start("localhost", AF_INET, "80", "127.0.0.1");
#else
  start("localhost", AF_INET, "80", "");
#endif
  // Pending lookups hold the runtime alive past main's return.
  returned = 1;
  return 0;
}
