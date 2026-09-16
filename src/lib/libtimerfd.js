/**
 * @license
 * Copyright 2026 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

// timerfd(2) for the JS filesystem. A timerfd is a detached FS stream whose
// readiness (POLLIN once the timer has expired) feeds the per-inode wait-queue
// (FSNode.notifyListeners), so poll()/epoll see it like a socket or pipe. The
// timer itself is a single setTimeout, re-armed on each expiry for a periodic
// timer, holding a runtime keepalive while armed (Node's timer semantics: an
// armed timer keeps the event loop alive, a disarmed or closed one does not).

var TimerFDLibrary = {
  // The timer state lives on the stream's `shared` object (the open file
  // description dup'd fds share) as `shared.timerfd`; the last close reclaims
  // it. Times are ms floats in the timer's clock: Date.now() for
  // CLOCK_REALTIME, emscripten_get_now() otherwise (the same source
  // clock_gettime(CLOCK_MONOTONIC) reads, so TFD_TIMER_ABSTIME lines up).
  $timerfdNow__internal: true,
  $timerfdNow__deps: ['emscripten_get_now'],
  $timerfdNow: (t) => t.clockid == {{{ cDefs.CLOCK_REALTIME }}} ? Date.now() : _emscripten_get_now(),

  $timerfdDisarm__internal: true,
  $timerfdDisarm: (t) => {
    if (!t.timer) return;
    clearTimeout(t.timer);
    t.timer = null;
    {{{ runtimeKeepalivePop() }}}
  },

  $timerfdArm__internal: true,
  $timerfdArm__deps: ['$timerfdNow', '$callUserCallback'],
  $timerfdArm: (t) => {
    {{{ runtimeKeepalivePush() }}}
    t.timer = setTimeout(() => {
      t.timer = null;
      {{{ runtimeKeepalivePop() }}}
      // Not user code, but callUserCallback's maybeExit lets a runtime that was
      // only kept alive by this timer exit now that it has fired.
      callUserCallback(() => {
        if (t.interval) {
          // Overrun expirations coalesce into the count, as on Linux; the next
          // expiry stays on the original period grid.
          var n = Math.max(1, 1 + Math.floor((timerfdNow(t) - t.next) / t.interval));
          t.expirations += n;
          t.next += n * t.interval;
          timerfdArm(t);
        } else {
          t.expirations++;
          t.next = 0;
        }
        t.node.notifyListeners({{{ cDefs.POLLIN }}});
      });
    }, Math.max(0, t.next - timerfdNow(t)));
  },

  $timerfdNewInstance__internal: true,
  $timerfdNewInstance__deps: ['$FS', '$timerfdDisarm'],
  $timerfdNewInstance: (clockid, flags) => {
    // Its own (detached) node carries the readiness wait-queue; shared across
    // dups.
    var node = new FS.FSNode(0, '', 0, 0);
    var stream = FS.createStream({
      node,
      flags: {{{ cDefs.O_RDWR }}} | (flags & {{{ cDefs.O_NONBLOCK }}}),
      seekable: false,
      stream_ops: {
        poll(stream) {
          return stream.shared.timerfd.expirations ? {{{ cDefs.POLLIN }}} : 0;
        },
        // read(2) returns the u64 count of expirations since the last read (or
        // settime) and resets it; EAGAIN when none have occurred. A blocking
        // read cannot block here, so it behaves as non-blocking (like a pipe).
        read(stream, buffer, offset, length) {
          var t = stream.shared.timerfd;
          if (length < 8) throw new FS.ErrnoError({{{ cDefs.EINVAL }}});
          var n = t.expirations;
          if (!n) throw new FS.ErrnoError({{{ cDefs.EAGAIN }}});
          t.expirations = 0;
          for (var i = 0; i < 8; i++) {
            buffer[offset + i] = n % 256;
            n = Math.floor(n / 256);
          }
          return 8;
        },
        dup(stream) {
          stream.shared.timerfd.refcount++;
        },
        close(stream) {
          var t = stream.shared.timerfd;
          if (--t.refcount) return;
          timerfdDisarm(t);
        },
      },
    });
    stream.shared.timerfd = {
      node,
      clockid,
      timer: null,
      // Absolute expiry time in the timer's clock (0 when disarmed) and the
      // period (0 for one-shot).
      next: 0,
      interval: 0,
      expirations: 0,
      refcount: 1,
    };
    return stream;
  },

  // timerfd_settime: `value`/`interval` are ms (0 value disarms); the previous
  // setting is stored to `ovalue` when non-null.
  $timerfdSetTime__internal: true,
  $timerfdSetTime__deps: ['$timerfdNow', '$timerfdDisarm', '$timerfdArm'],
  $timerfdSetTime: (t, flags, value, interval) => {
    timerfdDisarm(t);
    t.expirations = 0;
    t.interval = 0;
    t.next = 0;
    if (!value) return;
    t.interval = interval;
    t.next = flags & {{{ cDefs.TFD_TIMER_ABSTIME }}} ? value : timerfdNow(t) + value;
    timerfdArm(t);
  },

  // Store the current setting as an itimerspec: the time remaining until the
  // next expiry (0 only when disarmed) and the period.
  $timerfdGetTime__internal: true,
  $timerfdGetTime__deps: ['$timerfdNow', '$timerfdStoreTimespec'],
  $timerfdGetTime: (t, out) => {
    var remaining = 0;
    if (t.next) {
      remaining = t.next - timerfdNow(t);
      // Due but the setTimeout has not run yet: still armed, so report the
      // time to the next period boundary (Linux forwards the timer here), or
      // a nominal 1ns for a one-shot.
      if (remaining <= 0) {
        remaining = t.interval ? remaining % t.interval + t.interval : 1e-6;
      }
    }
    timerfdStoreTimespec(out + {{{ C_STRUCTS.itimerspec.it_interval }}}, t.next ? t.interval : 0);
    timerfdStoreTimespec(out + {{{ C_STRUCTS.itimerspec.it_value }}}, remaining);
  },

  // ms -> timespec; a non-zero time never rounds down to zero, since zero
  // means "disarmed".
  $timerfdStoreTimespec__internal: true,
  $timerfdStoreTimespec__deps: ['$writeI53ToI64'],
  $timerfdStoreTimespec: (ptr, ms) => {
    var sec = Math.floor(ms / 1000);
    var nsec = Math.round((ms - sec * 1000) * 1e6);
    if (nsec >= 1e9) {
      sec++;
      nsec -= 1e9;
    }
    if (ms > 0 && !sec && !nsec) nsec = 1;
    {{{ makeSetValue('ptr', C_STRUCTS.timespec.tv_sec, 'sec', 'i53') }}};
    {{{ makeSetValue('ptr', C_STRUCTS.timespec.tv_nsec, 'nsec', LONG_TYPE) }}};
  },

  // timespec -> ms, or -1 if invalid (negative, or tv_nsec out of range).
  $timerfdLoadTimespec__internal: true,
  $timerfdLoadTimespec__deps: ['$readI53FromI64'],
  $timerfdLoadTimespec: (ptr) => {
    var sec = {{{ makeGetValue('ptr', C_STRUCTS.timespec.tv_sec, 'i53') }}};
    // tv_nsec is a long; read it unsigned so a negative value is out of range.
    var nsec = {{{ makeGetValue('ptr', C_STRUCTS.timespec.tv_nsec, '*') }}};
    if (sec < 0 || nsec >= 1e9) return -1;
    return sec * 1000 + nsec / 1e6;
  },
};

addToLibrary(TimerFDLibrary);
