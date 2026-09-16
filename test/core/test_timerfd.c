/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
 * Exercises the timerfd syscall surface (timerfd_create/settime/gettime) in
 * the non-blocking readiness model: creation flags and errors, arming and
 * disarming, gettime, and expiry observed through poll(), epoll and read()
 * after returning to the event loop.
 */

#include <assert.h>
#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

int tfd, rt, periodic, ep;

void check_periodic(void* arg);

// Runs after the one-shot has expired: readable exactly once, then EAGAIN.
void check_oneshot(void* arg) {
  struct pollfd pfd = {.fd = tfd, .events = POLLIN};
  assert(poll(&pfd, 1, 0) == 1);
  assert(pfd.revents == POLLIN);

  struct epoll_event out[2];
  assert(epoll_wait(ep, out, 2, 0) == 1);
  assert(out[0].events == EPOLLIN);
  assert(out[0].data.u32 == 42);

  // A fired one-shot reports as disarmed.
  struct itimerspec cur;
  assert(timerfd_gettime(tfd, &cur) == 0);
  assert(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

  uint64_t n = 0;
  assert(read(tfd, &n, sizeof(n)) == 8);
  assert(n == 1);
  // The past-deadline absolute timer expired too.
  assert(read(rt, &n, sizeof(n)) == 8);
  assert(n == 1);
  assert(close(rt) == 0);
  // Reading consumes the expirations: no longer readable.
  assert(read(tfd, &n, sizeof(n)) == -1 && errno == EAGAIN);
  assert(poll(&pfd, 1, 0) == 0);
  assert(epoll_wait(ep, out, 2, 0) == 0);

  // A periodic timer: 5ms initial, 5ms period, watched by the same epoll.
  periodic = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  assert(periodic >= 0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLET};
  ev.data.u32 = 7;
  assert(epoll_ctl(ep, EPOLL_CTL_ADD, periodic, &ev) == 0);
  struct itimerspec its = {
    .it_value = {.tv_nsec = 5 * 1000000},
    .it_interval = {.tv_nsec = 5 * 1000000},
  };
  assert(timerfd_settime(periodic, 0, &its, NULL) == 0);
  emscripten_async_call(check_periodic, NULL, 40);
}

// After ~40ms several periods have elapsed: the expirations coalesce into one
// count, the timer stays armed with its period, and the epoll edge fired.
void check_periodic(void* arg) {
  struct epoll_event out[2];
  assert(epoll_wait(ep, out, 2, 0) == 1);
  assert(out[0].data.u32 == 7);

  uint64_t n = 0;
  assert(read(periodic, &n, sizeof(n)) == 8);
  assert(n >= 2);
  assert(read(periodic, &n, sizeof(n)) == -1 && errno == EAGAIN);

  struct itimerspec cur;
  assert(timerfd_gettime(periodic, &cur) == 0);
  assert(cur.it_interval.tv_nsec == 5 * 1000000);
  assert(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0);

  // Disarming: it_value zero. The pending count is dropped and the epoll
  // registration is untouched but no longer ready.
  struct itimerspec zero = {0};
  assert(timerfd_settime(periodic, 0, &zero, &cur) == 0);
  assert(cur.it_interval.tv_nsec == 5 * 1000000);
  assert(timerfd_gettime(periodic, &cur) == 0);
  assert(cur.it_value.tv_nsec == 0 && cur.it_interval.tv_nsec == 0);
  assert(epoll_wait(ep, out, 2, 0) == 0);

  // Closing removes it from the set; the one-shot fd is still registered.
  assert(close(periodic) == 0);
  assert(epoll_ctl(ep, EPOLL_CTL_DEL, tfd, NULL) == 0);
  assert(close(tfd) == 0);
  assert(close(ep) == 0);
  printf("done\n");
}

int main(void) {
  // Invalid clocks and unknown flags are rejected.
  assert(timerfd_create(999, 0) == -1 && errno == EINVAL);
  assert(timerfd_create(CLOCK_MONOTONIC, 0x1) == -1 && errno == EINVAL);
  // Both clocks are accepted; TFD_CLOEXEC is a no-op.
  rt = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
  assert(rt >= 0);
  assert(close(rt) == 0);

  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  assert(tfd >= 0);
  assert(fcntl(tfd, F_GETFL) & O_NONBLOCK);

  // settime/gettime on a non-timerfd is EINVAL; on a bad fd EBADF.
  struct itimerspec its = {.it_value = {.tv_nsec = 5 * 1000000}};
  int p[2];
  assert(pipe(p) == 0);
  assert(timerfd_settime(p[0], 0, &its, NULL) == -1 && errno == EINVAL);
  assert(timerfd_gettime(p[0], &its) == -1 && errno == EINVAL);
  assert(timerfd_settime(9999, 0, &its, NULL) == -1 && errno == EBADF);
  // Unknown settime flags and out-of-range timespecs are EINVAL.
  assert(timerfd_settime(tfd, 0x4, &its, NULL) == -1 && errno == EINVAL);
  struct itimerspec bad = {.it_value = {.tv_nsec = 1000000000}};
  assert(timerfd_settime(tfd, 0, &bad, NULL) == -1 && errno == EINVAL);
  bad.it_value.tv_nsec = -1;
  assert(timerfd_settime(tfd, 0, &bad, NULL) == -1 && errno == EINVAL);

  // Unarmed: not readable, read is EAGAIN, gettime is all zero, a short read
  // is EINVAL, and writes are rejected.
  uint64_t n;
  assert(read(tfd, &n, sizeof(n)) == -1 && errno == EAGAIN);
  assert(read(tfd, &n, 4) == -1 && errno == EINVAL);
  assert(write(tfd, &n, sizeof(n)) == -1 && errno == EINVAL);
  struct itimerspec cur;
  memset(&cur, 0xff, sizeof(cur));
  assert(timerfd_gettime(tfd, &cur) == 0);
  assert(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);
  assert(cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 0);
  struct pollfd pfd = {.fd = tfd, .events = POLLIN};
  assert(poll(&pfd, 1, 0) == 0);

  ep = epoll_create1(0);
  assert(ep >= 0);
  struct epoll_event ev = {.events = EPOLLIN};
  ev.data.u32 = 42;
  assert(epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) == 0);
  struct epoll_event out[2];
  assert(epoll_wait(ep, out, 2, 0) == 0);

  // Arm a one-shot far out, then re-arm: old value reports the remaining time
  // of the first setting, and only the second is live.
  struct itimerspec far = {.it_value = {.tv_sec = 100}};
  assert(timerfd_settime(tfd, 0, &far, NULL) == 0);
  assert(timerfd_gettime(tfd, &cur) == 0);
  assert(cur.it_value.tv_sec >= 99 && cur.it_value.tv_sec <= 100);
  assert(timerfd_settime(tfd, 0, &its, &cur) == 0);
  assert(cur.it_value.tv_sec >= 99 && cur.it_value.tv_sec <= 100);
  assert(timerfd_gettime(tfd, &cur) == 0);
  assert(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 &&
         cur.it_value.tv_nsec <= 5 * 1000000);
  // Not yet expired: nothing to read or collect until the event loop runs.
  assert(read(tfd, &n, sizeof(n)) == -1 && errno == EAGAIN);
  assert(epoll_wait(ep, out, 2, 0) == 0);

  // An absolute deadline on CLOCK_REALTIME.
  struct timespec now;
  assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
  struct itimerspec abs = {
    .it_value = {.tv_sec = now.tv_sec + 50, .tv_nsec = now.tv_nsec}};
  assert(timerfd_settime(rt = timerfd_create(CLOCK_REALTIME, 0),
                         TFD_TIMER_ABSTIME,
                         &abs,
                         NULL) == 0);
  assert(timerfd_gettime(rt, &cur) == 0);
  assert(cur.it_value.tv_sec >= 49 && cur.it_value.tv_sec <= 50);
  // An absolute deadline already in the past expires on the next turn.
  abs.it_value.tv_sec = now.tv_sec - 1;
  assert(timerfd_settime(rt, TFD_TIMER_ABSTIME, &abs, NULL) == 0);
  assert(read(rt, &n, sizeof(n)) == -1 && errno == EAGAIN);

  // dup() shares the timer; closing the duplicate leaves it armed.
  int d = dup(tfd);
  assert(d >= 0);
  assert(close(d) == 0);

  emscripten_async_call(check_oneshot, NULL, 20);
  return 0;
}
