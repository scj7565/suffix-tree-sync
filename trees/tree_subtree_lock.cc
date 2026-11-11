// --mode=subtree_lock : 첫 토큰(서브트리)마다 락 (Coarse-grained)
// 갱신은 서브트리 락 64개 + 루트 락 1개, 삭제는 한 스레드, 추측은 읽기만.
#include "../common/locks.h"
#include "../common/tree_iface.h"
#include "subtree_lock/st_subtree_lock.h"

class SubtreeLockTree : public ConcurrentTree {
    subtree_lock::SuffixTree t;
public:
    explicit SubtreeLockTree(int depth) : t(depth) {}
    // {par_update, par_remove, par_read, read_with_update}: 갱신끼리만 동시에 불러도 안전
    TreeCaps caps() const override { return {true, false, true, false}; }
    void prepare_seq(int id, int max_len) override { t.prepare_seq(id, max_len); }
    void extend(int id, std::span<const int32_t> tok) override { t.extend(id, tok); }
    void remove(int id) override { t.remove(id); }
    Draft speculate(std::span<const int32_t> ctx, const SpecParams& p) override {
        return t.speculate(ctx, p.max_spec_tokens, p.max_spec_factor, p.max_spec_offset,
                           p.min_token_prob, p.use_tree_spec);
    }
        std::string check() override { return t.check_integrity(); }
    uint64_t digest() override { return t.content_digest(); }   // --check 가 serial 과 비교
};
REGISTER_TREE("subtree_lock", "one lock per subtree (first token, 64 locks) plus a root lock", SubtreeLockTree);
