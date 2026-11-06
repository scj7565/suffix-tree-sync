"""새 동기화 방식의 뼈대를 만든다 (각자 1번만 실행).

  python tools/new_tree.py subtree_lock    # subtree_lock 담당 (Coarse-grained: 서브트리 락)
  python tools/new_tree.py node_lock       # node_lock 담당 (Fine-grained: 노드별 락)
  python tools/new_tree.py optimistic      # optimistic 담당 (Optimistic)
  python tools/new_tree.py lazy            # lazy 담당 (Lazy)

만들어지는 것
  trees/<name>/st_<name>.h, .cc  : Arctic suffix_tree 원본 복사본 (namespace <name> 로 감쌈)
                                   → 여기에 락·버전 번호를 넣는다 (자기 파일만 수정)
  trees/tree_<name>.cc           : 하네스 연결부(어댑터). caps() 를 구현 상태에 맞게 바꾼다
다시 scripts\\build.bat 하면 --mode=<name> 으로 실행된다. 처음엔 원본과 결과가 완전히 같아야 정상.
"""
import pathlib
import re
import sys

if len(sys.argv) != 2 or not re.fullmatch(r"[a-z][a-z0-9_]*", sys.argv[1]):
    sys.exit("usage: python new_tree.py <name>   (lowercase letters, digits, _)")
name = sys.argv[1]
root = pathlib.Path(__file__).resolve().parent.parent   # 저장소 루트
d = root / "trees" / name
if d.exists():
    sys.exit(f"{d} already exists")
d.mkdir(parents=True)

h = (root / "third_party" / "arctic" / "suffix_tree.h").read_text(encoding="utf-8")
h = h.replace('#include "int32_map.h"',
              '#include "../../third_party/arctic/int32_map.h"\n'
              '#include "../../third_party/arctic/suffix_tree.h"   // ::Draft (측정 틀과 같은 타입)\n\n'
              f'namespace {name} {{\nusing ::Draft;')
h = re.sub(r"struct Draft \{.*?\n\};\n", "", h, flags=re.S)
h = h.rstrip() + f"\n\n}}  // namespace {name}\n"
(d / f"st_{name}.h").write_text(h, encoding="utf-8")

c = (root / "third_party" / "arctic" / "suffix_tree.cc").read_text(encoding="utf-8")
c = c.replace('#include "suffix_tree.h"', f'#include "st_{name}.h"\n\nnamespace {name} {{')
c = c.rstrip() + f"\n\n}}  // namespace {name}\n"
(d / f"st_{name}.cc").write_text(c, encoding="utf-8")

cls = "".join(p.capitalize() for p in name.split("_")) + "Tree"
(root / "trees" / f"tree_{name}.cc").write_text(f'''// --mode={name} : (설명을 쓰세요)
#include "../common/locks.h"
#include "../common/tree_iface.h"
#include "{name}/st_{name}.h"

class {cls} : public ConcurrentTree {{
    {name}::SuffixTree t;
public:
    explicit {cls}(int depth) : t(depth) {{}}
    // 구현이 진행되면 true 로 바꾼다: {{par_update, par_remove, par_read, read_with_update}}
    TreeCaps caps() const override {{ return {{false, false, true, false}}; }}
    void prepare_seq(int /*id*/, int /*max_len*/) override {{}}   // 필요하면 st_{name} 에 reserve 함수를 만들어 호출
    void extend(int id, std::span<const int32_t> tok) override {{ t.extend(id, tok); }}
    void remove(int id) override {{ t.remove(id); }}
    Draft speculate(std::span<const int32_t> ctx, const SpecParams& p) override {{
        return t.speculate(ctx, p.max_spec_tokens, p.max_spec_factor, p.max_spec_offset,
                           p.min_token_prob, p.use_tree_spec);
    }}
    void end_step() override {{}}                                 // Lazy 정리 등
    std::string check() override {{ return t.check_integrity(); }}
    uint64_t digest() override {{ return t.content_digest(); }}   // --check 가 serial 과 비교
}};
REGISTER_TREE("{name}", "(description)", {cls});
''', encoding="utf-8")
print(f"created trees/{name}/st_{name}.h, st_{name}.cc and trees/tree_{name}.cc -> run scripts\\build.bat, then bin\\tree_bench data\\tokens_code.bin --quick")
