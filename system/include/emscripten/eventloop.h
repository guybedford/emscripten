/*
 * Copyright 2021 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#pragma once

#include "em_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void emscripten_unwind_to_js_event_loop(void) __attribute__((__noreturn__));

int emscripten_set_timeout(void (* _Nonnull cb)(void *user_data), double msecs, void *user_data);
void emscripten_clear_timeout(int id);
void emscripten_set_timeout_loop(bool (* _Nonnull cb)(double time, void *user_data), double interval_ms, void *user_data);

int emscripten_set_immediate(void (* _Nonnull cb)(void *user_data), void *user_data);
void emscripten_clear_immediate(int id);
void emscripten_set_immediate_loop(bool (*cb)(void *user_data), void *user_data);

int emscripten_set_interval(void (* _Nonnull cb)(void *user_data), double interval_ms, void *user_data);
void emscripten_clear_interval(int id);

void emscripten_runtime_keepalive_push(void);
void emscripten_runtime_keepalive_pop(void);
bool emscripten_runtime_keepalive_check(void);

// EXPERIMENTAL: this API is new and may still change.
//
// poll(2) on a single file descriptor with a callback in place of the blocking
// call, so it needs no ASYNCIFY/JSPI. `callback(fd, revents, userdata)` runs
// exactly once, on the calling thread's event loop (never on this call's stack,
// even if `fd` is ready now), once `fd` reports one of `events` - the <poll.h>
// mask (POLLIN, POLLOUT, POLLPRI, POLLRDHUP); POLLERR, POLLHUP and POLLNVAL are
// always reported, so closing the fd completes the wait with POLLNVAL. Readiness
// is the same level-derived state poll() reports: a call on an fd that is ready
// returns at once (on the next tick), and one poll() call is one completion -
// to wait again, call again. An outstanding wait keeps the calling thread's
// runtime alive until it completes or is cancelled.
//
// Any fd poll() accepts is accepted here. Readiness transitions are produced
// by sockets, pipes and epoll fds; other descriptors (regular files) are always
// ready, as with poll(). An epoll fd is readable while its set has ready
// events, so as on Linux, where poll() on an epoll fd composes with
// epoll_wait(), `emscripten_poll_callback(epfd, POLLIN, ...)` followed by
// `epoll_wait(epfd, ..., 0)` in the callback is a non-blocking epoll_wait; a
// registration that stays ready keeps the epoll fd readable, so a wait armed
// again fires again at once, exactly as poll() on it would. Several waits on
// one epoll fd behave as several blocking epoll_wait callers over the one
// ready list.
//
// Returns a wait id (> 0), or -EBADF for a bad fd.
typedef void (*em_poll_callback)(int fd, int revents, void *userdata);
int emscripten_poll_callback(int fd, int events, em_poll_callback callback, void *userdata);

// Cancel an outstanding wait armed by the calling thread. Returns 0, or -ENOENT
// if `id` is not one of this thread's outstanding waits - including a wait that
// has already completed, whose callback is (or will be) delivered regardless.
int emscripten_poll_callback_cancel(int id);

#ifdef __cplusplus
}
#endif
