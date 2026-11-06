// 공통 틀: 전역 트리 구현이 지켜야 할 인터페이스 (subtree_lock 담당만 수정)
// ---------------------------------------------------------------------------
// 각 동기화 방식(global_lock, subtree_lock, node_lock, optimistic, lazy)은
//   1) ConcurrentTree 를 상속한 클래스를 만들고
//   2) 파일 끝에 REGISTER_TREE("이름", "설명", 클래스) 한 줄을 쓰면
// tree_bench 에서 --mode=이름 으로 고를 수 있다. 측정 틀 코드는 건드리지 않는다.
// 새 방식의 뼈대는 python tools/new_tree.py <이름> 으로 만든다.
//
// 개인(로컬) 트리는 요청마다 따로라서 항상 Arctic 원본 SuffixTree 를 쓴다.
// 이 인터페이스는 "모든 요청이 공유하는 전역 트리"에만 쓴다.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "../third_party/arctic/suffix_tree.h"   // Draft 구조체를 그대로 쓴다

struct SpecParams {
    int max_spec_tokens = 16;
    float max_spec_factor = 1.0f;
    float max_spec_offset = 0.0f;
    float min_token_prob = 0.1f;
    bool use_tree_spec = false;
};

// 이 구현이 "무엇을 동시에 불러도 안전한가"를 하네스에 알려 준다.
// 하네스는 false 인 조합을 절대 동시에 부르지 않는다 (그 단계는 한 스레드로 실행).
struct TreeCaps {
    bool par_update;        // extend() 를 여러 스레드가 동시에 불러도 됨 (서로 다른 seq_id)
    bool par_remove;        // remove() 를 extend()/remove() 와 동시에 불러도 됨
    bool par_read;          // speculate() 끼리 동시에 불러도 됨 (쓰는 스레드가 없을 때)
    bool read_with_update;  // speculate() 를 extend()/remove() 와 동시에 불러도 됨
};

class ConcurrentTree {
public:
    virtual ~ConcurrentTree() = default;
    virtual TreeCaps caps() const = 0;

    // [한 스레드] 측정 전에 쓸 seq_id 와 최대 길이를 미리 알려 준다.
    // 동시 갱신 구현은 여기서 _seqs 맵에 자리를 만들고 vector 를 reserve 해 두면
    // "맵 rehash / vector 재할당 중에 다른 스레드가 읽는" 경쟁을 처음부터 없앨 수 있다.
    virtual void prepare_seq(int /*seq_id*/, int /*max_len*/) {}

    // 갱신: seq_id 시퀀스 끝에 tokens 를 붙인다 (Arctic SuffixTree::extend 와 같은 의미)
    virtual void extend(int seq_id, std::span<const int32_t> tokens) = 0;

    // 삭제: 캐시 한도 초과로 오래된 응답을 트리에서 뺀다 (Arctic SuffixTree::remove)
    virtual void remove(int seq_id) = 0;

    // 추측: 문맥 끝과 맞는 경로를 찾아 다음 토큰들을 추측 (읽기 전용이어야 함)
    virtual Draft speculate(std::span<const int32_t> context, const SpecParams& p) = 0;

    // [한 스레드] 스텝이 끝날 때마다 불린다. 다른 호출과 겹치지 않는다.
    // Lazy 삭제의 "나중에 한꺼번에 정리", 버전 번호 정리 등에 쓴다.
    virtual void end_step() {}

    // [한 스레드] 트리 구조 검사. 정상이면 "" 반환
    virtual std::string check() { return "(check not implemented)"; }

    // [한 스레드] 트리 내용 요약값 (--check 가 스텝마다 serial 과 비교). 0 = 구현 안 함
    // 보통은 트리 복사본의 content_digest() 를 그대로 부르면 된다.
    virtual uint64_t digest() { return 0; }
};

// ---------------------------------------------------------------- 등록
using TreeFactory = std::function<std::unique_ptr<ConcurrentTree>(int max_depth)>;
struct TreeEntry { TreeFactory make; std::string desc; };

inline std::map<std::string, TreeEntry>& tree_registry() {
    static std::map<std::string, TreeEntry> r;
    return r;
}
struct TreeRegistrar {
    TreeRegistrar(const char* name, const char* desc, TreeFactory f) {
        tree_registry()[name] = TreeEntry{std::move(f), desc};
    }
};
#define REGISTER_TREE(name, desc, cls)                                              \
    static TreeRegistrar reg_tree_##cls(name, desc, [](int d) {                     \
        return std::unique_ptr<ConcurrentTree>(new cls(d));                         \
    })

// ---------------------------------------------------------------- 스레드별 카운터
// 하네스가 스레드 번호(g_tid: 메인 0, 작업 스레드 1..)를 정해 준다.
// 구현은 락 대기 시간·재시도 횟수를 tctr() 에 더하면 결과 표에 나온다.
inline double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
enum Work { W_LOCAL_ADD, W_GLOBAL_ADD, W_REMOVE, W_SPEC_LOCAL, W_SPEC_GLOBAL, W_N };
struct alignas(64) ThreadCounters {
    double work[W_N] = {};     // 하네스가 채움: 작업별 시간 합 (초)
    double lock_wait_s = 0;    // 구현이 채움: 락을 기다린 시간 (초)
    long long retries = 0;     // 구현이 채움: 낙관적 읽기 재시도 등
    long long extra[4] = {};   // 구현이 자유롭게 (결과 표에 extra0..3 으로 출력)
};
constexpr int MAX_THREADS = 64;
inline thread_local int g_tid = 0;
inline ThreadCounters g_ctr[MAX_THREADS + 1];
inline ThreadCounters& tctr() { return g_ctr[g_tid]; }

// 락 대기 시간 재기:  { LockWait w; mu.lock(); }   (w 가 사라질 때 대기 시간이 더해짐)
struct LockWait {
    double t0 = now_s();
    ~LockWait() { tctr().lock_wait_s += now_s() - t0; }
};
