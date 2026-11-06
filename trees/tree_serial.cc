// --mode=serial : Arctic 원본 그대로, 락 없음 (기준). 갱신은 한 스레드만 가능.
#include "../common/tree_iface.h"

class ArcticSerial : public ConcurrentTree {
    SuffixTree t;
public:
    explicit ArcticSerial(int depth) : t(depth) {}
    // 추측끼리는 동시에 불러도 됨(사전 실험 3에서 확인), 갱신은 한 스레드만
    TreeCaps caps() const override { return {false, false, true, false}; }
    void extend(int id, std::span<const int32_t> tok) override { t.extend(id, tok); }
    void remove(int id) override { t.remove(id); }
    Draft speculate(std::span<const int32_t> ctx, const SpecParams& p) override {
        return t.speculate(ctx, p.max_spec_tokens, p.max_spec_factor, p.max_spec_offset,
                           p.min_token_prob, p.use_tree_spec);
    }
    std::string check() override { return t.check_integrity(); }
    uint64_t digest() override { return t.content_digest(); }
};
REGISTER_TREE("serial", "Arctic original, no lock (update: 1 thread only)", ArcticSerial);
