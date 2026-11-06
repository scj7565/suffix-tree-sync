// --mode=global_lock : 트리 전체에 읽기·쓰기 락 1개 (Coarse-grained, 모든 방식의 비교 기준)
// 갱신·삭제 = 배타 락, 추측 = 공유 락. 트리 코드는 Arctic 원본 그대로.
// 새 방식을 만들 때 이 파일을 "어댑터 예시"로 보면 된다.
#include "../common/locks.h"
#include "../common/tree_iface.h"

class GlobalLockTree : public ConcurrentTree {
    SuffixTree t;
    RWLock mu;
public:
    explicit GlobalLockTree(int depth) : t(depth) {}
    TreeCaps caps() const override { return {true, true, true, true}; }
    void extend(int id, std::span<const int32_t> tok) override {
        { LockWait w; mu.lock(); }
        t.extend(id, tok);
        mu.unlock();
    }
    void remove(int id) override {
        { LockWait w; mu.lock(); }
        t.remove(id);
        mu.unlock();
    }
    Draft speculate(std::span<const int32_t> ctx, const SpecParams& p) override {
        { LockWait w; mu.lock_shared(); }
        Draft d = t.speculate(ctx, p.max_spec_tokens, p.max_spec_factor, p.max_spec_offset,
                              p.min_token_prob, p.use_tree_spec);
        mu.unlock_shared();
        return d;
    }
    std::string check() override { return t.check_integrity(); }
    uint64_t digest() override { return t.content_digest(); }
};
REGISTER_TREE("global_lock", "one reader-writer lock for the whole tree (baseline)", GlobalLockTree);
