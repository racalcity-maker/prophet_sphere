from __future__ import annotations

import numpy as np


class Int16RingBuffer:
    """Fixed-size int16 ring buffer."""

    def __init__(self, capacity_samples: int) -> None:
        if capacity_samples <= 0:
            raise ValueError("capacity_samples must be > 0")
        self._buf = np.zeros((capacity_samples,), dtype=np.int16)
        self._capacity = int(capacity_samples)
        self._write = 0
        self._count = 0

    @property
    def capacity(self) -> int:
        return self._capacity

    @property
    def count(self) -> int:
        return self._count

    def clear(self) -> None:
        self._write = 0
        self._count = 0

    def append(self, samples: np.ndarray) -> None:
        if samples.size == 0:
            return
        arr = np.asarray(samples, dtype=np.int16).reshape(-1)
        n = int(arr.size)

        if n >= self._capacity:
            self._buf[:] = arr[-self._capacity :]
            self._write = 0
            self._count = self._capacity
            return

        end = self._write + n
        if end <= self._capacity:
            self._buf[self._write : end] = arr
        else:
            first = self._capacity - self._write
            self._buf[self._write :] = arr[:first]
            self._buf[: n - first] = arr[first:]

        self._write = end % self._capacity
        self._count = min(self._capacity, self._count + n)

    def latest(self, n_samples: int) -> np.ndarray:
        if n_samples <= 0:
            return np.empty((0,), dtype=np.int16)
        n = min(int(n_samples), self._count)
        if n <= 0:
            return np.empty((0,), dtype=np.int16)

        start = (self._write - n) % self._capacity
        if start + n <= self._capacity:
            return self._buf[start : start + n].copy()

        first = self._capacity - start
        return np.concatenate((self._buf[start:], self._buf[: n - first])).astype(np.int16, copy=False)

