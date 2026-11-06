// 공통 락 (subtree_lock 담당만 수정). 락 종류 비교용 TAS/TTAS/backoff 락은 node_lock 담당이 trees/node_lock/ 쪽 자기 파일에 만든다.
#pragma once

// Windows(MinGW)의 std::shared_mutex / std::mutex 는 winpthreads 를 거쳐 경합 시 매우 느림
// → Windows 에서는 SRWLOCK (MSVC std::shared_mutex 와 같은 기본 락)을 쓴다.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
struct RWLock {
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&l); }
    void lock_shared() { AcquireSRWLockShared(&l); }
    void unlock_shared() { ReleaseSRWLockShared(&l); }
};
using Mutex = RWLock;   // 배타 락만 쓰면 mutex 로 동작
#else
#include <mutex>
#include <shared_mutex>
using RWLock = std::shared_mutex;
using Mutex = std::mutex;
#endif
