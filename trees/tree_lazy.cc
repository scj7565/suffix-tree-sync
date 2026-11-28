// --mode=lazy : 삭제는 표시만 하고 실제 제거는 나중에 (Lazy)
//  remove() = 큐에 넣기, 첫 speculate = count 줄이기(서브트리별 병렬), end_step() = 노드 정리
//  extend() 끼리는 서브트리 락 64개 + 루트 락
// CSV extra0 = 정리한 seq 수, extra1 = 루트 락 횟수, extra2 = 떼어 낸 좀비 수, extra3 = count 줄인 노드 수
#include "../common/locks.h"
#include "../common/tree_iface.h"
#include "lazy/st_lazy.h"

class LazyTree : public ConcurrentTree {
    lazy::SuffixTree t;
public:
    explicit LazyTree(int depth) : t(depth) {}
    // {par_update, par_remove, par_read, read_with_update}: 추측은 락이 없어 갱신과 겹치면 안 됨
    TreeCaps caps() const override { return {true, true, true, false}; }
    void prepare_seq(int id, int max_len) override { t.prepare_seq(id, max_len); }
    void extend(int id, std::span<const int32_t> tok) override { t.extend(id, tok); }
    void remove(int id) override { t.mark_removed(id); }
    Draft speculate(std::span<const int32_t> ctx, const SpecParams& p) override {
        return t.speculate(ctx, p.max_spec_tokens, p.max_spec_factor, p.max_spec_offset,
                           p.min_token_prob, p.use_tree_spec);
    }
    void end_step() override {
        tctr().extra[0] += t.flush_removed();
    }
    std::string check() override { return t.check_integrity(); }
    uint64_t digest() override { return t.content_digest(); }   // --check 가 serial 과 비교
};
REGISTER_TREE("lazy", "remove = queue; counts in batch before speculate (per subtree); physical removal in end_step", LazyTree);
