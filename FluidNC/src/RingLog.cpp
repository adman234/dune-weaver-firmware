// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "RingLog.h"

#include <Arduino.h>  // millis()
#include <cstdio>
#include <mutex>

namespace {
    // kCapacity holds roughly the last 100-150 log lines - enough to see why
    // a day-long playlist ended without competing with the heap the log
    // exists to watch.
    constexpr size_t kCap = RingLog::kCapacity;

    char       _buf[kCap];
    size_t     _head  = 0;  // next write index
    size_t     _count = 0;  // bytes stored, saturates at kCap
    size_t     _total = 0;  // bytes ever pushed (absolute write position)
    std::mutex _mu;

    // Named ringPush, NOT push: inside RingLog::print_msg an unqualified
    // push() resolves to the inherited Channel::push(uint8_t) (member lookup
    // hides namespace scope), which feeds the never-drained input queue --
    // that leak boot-looped a board by exhausting the heap within a minute.
    void ringPush(char c) {
        _buf[_head] = c;
        _head       = (_head + 1) % kCap;
        if (_count < kCap) {
            ++_count;
        }
        ++_total;
    }
}

void RingLog::print_msg(MsgLevel level, const char* msg) {
    if (_message_level < level) {
        return;
    }
    char prefix[16];
    int  n = snprintf(prefix, sizeof(prefix), "[+%lu] ", millis() / 1000UL);

    std::lock_guard<std::mutex> lock(_mu);
    for (int i = 0; i < n; ++i) {
        ringPush(prefix[i]);
    }
    for (const char* p = msg; *p; ++p) {
        ringPush(*p);
    }
    ringPush('\n');
}

size_t RingLog::logBegin() {
    std::lock_guard<std::mutex> lock(_mu);
    size_t first = _total - _count;
    // After a wrap the oldest entry is usually a partial line; start at the
    // first boundary so the dump starts clean.
    if (_count == kCap) {
        for (size_t i = 0; i < _count; ++i) {
            if (_buf[(first + i) % kCap] == '\n') {
                return first + i + 1;
            }
        }
    }
    return first;
}

size_t RingLog::logEnd() {
    std::lock_guard<std::mutex> lock(_mu);
    return _total;
}

size_t RingLog::logRead(size_t& pos, size_t stop, char* out, size_t len) {
    std::lock_guard<std::mutex> lock(_mu);
    size_t first = _total - _count;
    if ((ptrdiff_t)(pos - first) < 0) {
        pos = first;  // overwritten while the caller was sending
    }
    size_t n = 0;
    while (n < len && (ptrdiff_t)(stop - pos) > 0) {
        out[n++] = _buf[pos % kCap];
        ++pos;
    }
    return n;
}

RingLog ringLog;
