// A library function with result forms: `answer(ms, value)` resolves to
// `value` after `ms` milliseconds (synchronously if `ms` is 0, rejecting if
// `value` is 0), as `answer()`, `answer_fd()` and `answer_promise()`. Where
// the caller cannot wait, `answer()` is -1.
addToLibrary({
  answer__sig: 'pip',
  answer__async: 'auto',
  answer: (ms, value, canWait) => {
    if (!ms) return value;
    if (!canWait) return -1;
    return new Promise((resolve, reject) =>
      setTimeout(() => value ? resolve(value) : reject(new Error('zero')), ms));
  },
  answer_fd: 'answer',
  answer_promise: 'answer',
});
