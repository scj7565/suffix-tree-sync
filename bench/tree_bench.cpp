// tree_bench: 공유 suffix 트리 동기화 방식 측정 틀 (subtree_lock 담당, 다른 사람은 수정하지 않음)
// ---------------------------------------------------------------------------
//  - 전역 트리 구현을 --mode=<이름> 으로 고른다 (trees/*.cc 에서 REGISTER_TREE 로 등록, --mode=all 은 전부)
//  - 실행 순서를 --sched=<serial|phase|stale|mixed> 로 고른다
//  - 갱신 비용을 개인 트리 / 전역 트리 / 삭제로 나눠 잰다 (단계별 벽시계 + 작업별 시간 합)
//  - --check=1: 같은 sched 의 serial(1스레드)과 스텝마다 트리 내용·추측을 비교 → PASS / PASS* / FAIL
//  - --profile=1: 전역 갱신이 첫 토큰(서브트리)별로 얼마나 쏠리는지 → 서브트리 락 이론 상한
//  - --quick: 작은 설정 + --check 로 10초 안에 확인 (고칠 때마다 돌려 보기용)
//
// 빌드 (Windows, cmd):  scripts\build.bat      → bin\tree_bench.exe
// 빌드 (Linux/Colab):   bash scripts/build.sh   → bin/tree_bench   (TSAN=1 → bin/tree_bench_tsan)
//
// 사용: tree_bench <tokens.bin> [--mode=serial,global_lock|all] [--sched=phase] [--threads=1,2,4]
//         [--b=256] [--n_cached=10000] [--steps=100] [--evict=0] [--repeat=3]
//         [--ref=1] [--check=0] [--profile=0] [--local=par|serial] [--csv=out.csv] [--quick] [--list]
//   KEY=VAL 형식(사전 측정 프로그램 방식)도 그대로 받는다: B=256 N_CACHED=10000 ...
//
// sched 의미 (스텝 하나 기준, 배치의 살아 있는 요청 lv 에 대해)
//   serial : vLLM 순서. 요청마다 [개인 갱신 → 전역 갱신 → 추측], 한 스레드
//   phase  : [개인 갱신 전부] → [삭제 + 전역 갱신 전부] → [추측 전부]. 각 단계 안은 병렬
//   stale  : [추측 전부] → [개인 갱신] → [삭제 + 전역 갱신]  (개발사 비동기 계획과 같은 의미)
//   mixed  : 요청마다 [개인 갱신 → 전역 갱신 → 추측]을 여러 스레드가 동시에 (비결정적)
//
// 병렬화 여부는 트리의 caps() 를 따른다. 예: serial 트리는 par_update=false 라서
// --threads=4 여도 전역 갱신은 한 스레드로 돌고 개인 갱신·추측만 병렬이 된다.
//
// --check 판정
//   PASS  : 모든 스텝 끝에서 트리 내용 요약값이 serial 과 같고, 추측도 100% 같고, 구조 검사 통과
//   PASS* : 트리 내용은 같지만 추측이 일부 다름 → 같은 count 형제의 순서 차이(정상일 수 있음).
//           mixed 는 실행마다 추측 순서가 달라지므로 트리 내용만 본다
//   FAIL  : 트리 내용이 다르거나(갱신 누락·중복 = 경쟁 버그) 구조 검사 실패 → 종료 코드 2
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../third_party/arctic/suffix_tree.h"
#include "../common/tree_iface.h"

// ---------------------------------------------------------------- 옵션
static std::map<std::string, std::string> g_opt;
static std::string norm_key(std::string k) {
    while (!k.empty() && k[0] == '-') k.erase(0, 1);
    for (auto& ch : k) ch = (ch == '-') ? '_' : (char)std::toupper((unsigned char)ch);
    return k;
}
static int opt_i(const char* k, int d) { auto it = g_opt.find(k); return it == g_opt.end() ? d : std::atoi(it->second.c_str()); }
static std::string opt_s(const char* k, const char* d) { auto it = g_opt.find(k); return it == g_opt.end() ? d : it->second; }
static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v; std::stringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) if (!t.empty()) v.push_back(t);
    return v;
}

// ---------------------------------------------------------------- 데이터
struct Rec { std::vector<int32_t> prompt, resp; };
static std::vector<Rec> load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
    char magic[4]; f.read(magic, 4);
    if (std::memcmp(magic, "KTOK", 4)) { std::fprintf(stderr, "bad magic\n"); std::exit(1); }
    int32_t n; f.read((char*)&n, 4);
    std::vector<Rec> v(n);
    for (auto& r : v) {
        int32_t pl, rl; f.read((char*)&pl, 4); f.read((char*)&rl, 4);
        r.prompt.resize(pl); r.resp.resize(rl);
        f.read((char*)r.prompt.data(), 4 * pl); f.read((char*)r.resp.data(), 4 * rl);
    }
    return v;
}

// ---------------------------------------------------------------- 스레드 풀
// 스텝마다 스레드를 새로 만들지 않고, 배리어로 깨워서 일을 시킨다 (사전 측정 프로그램과 같음)
struct Pool {
    int n; std::vector<std::thread> th; std::barrier<> go, done; std::function<void(int)> job; bool stop = false;
    explicit Pool(int n_) : n(n_), go(n_ + 1), done(n_ + 1) {
        for (int t = 0; t < n; t++) th.emplace_back([this, t] {
            g_tid = t + 1;                       // 메인 스레드는 0
            for (;;) { go.arrive_and_wait(); if (stop) return; job(t); done.arrive_and_wait(); }
        });
    }
    void run(std::function<void(int)> f) { job = std::move(f); go.arrive_and_wait(); done.arrive_and_wait(); }
    ~Pool() { stop = true; go.arrive_and_wait(); for (auto& t : th) t.join(); }
};
// 0..n-1 을 공유 카운터로 chunk 개씩 나눠 가짐 (수업 4장). pool 이 없으면 한 스레드로
template <class F>
static void par_for(Pool* pool, int n, int chunk, F&& f) {
    if (!pool) { for (int k = 0; k < n; k++) f(k); return; }
    std::atomic<int> next{0};
    pool->run([&](int) {
        for (int k; (k = next.fetch_add(chunk)) < n;)
            for (int j = k; j < std::min(k + chunk, n); j++) f(j);
    });
}

// ---------------------------------------------------------------- 시뮬레이션
struct Cfg { int B, N_CACHED, STEPS, SPEC, DEPTH, EVICT; bool local_par, profile, check; };

// 서브트리 쏠림 측정: 토큰 하나를 전역 트리에 붙이면 활성 노드 최대 DEPTH개를 고친다.
// 시작 위치 j 의 접미사를 담은 활성 노드는 첫 토큰 seq[j] 의 서브트리에 있다.
// → 스텝마다 "서브트리별 노드 작업 수"를 세면, 서브트리 락의 이론 상한을 미리 알 수 있다.
//   상한(T) = 전체 작업 / max(전체/T, 가장 붐비는 서브트리 작업[, 루트 작업])  (작업 비용이 같다고 가정)
static const int PROF_T[] = {2, 4, 6, 8, 12};
struct Prof {
    long long steps = 0; double total = 0, root = 0, maxsub = 0, distinct = 0;
    double den_sub[5] = {}, den_root[5] = {};
    std::unordered_map<int32_t, long long> all;
};

struct Result {
    double ph_local = 0, ph_global = 0, ph_remove = 0, ph_spec = 0, ph_mixed = 0, ph_end = 0, wall = 0;
    double work[W_N] = {}, lock_wait = 0;
    long long retries = 0, extra[4] = {};
    long long req_steps = 0, accepted = 0, drafted = 0, removed = 0;
    uint64_t hash = 1469598103934665603ULL;
    std::vector<uint64_t> step_hash;
    std::vector<uint64_t> tree_digest;               // --check: 스텝마다 트리 내용 요약값
    std::map<std::pair<int, int>, uint64_t> rs_hash;
    std::string integrity = "-", note, verdict = "-";
    double spec_pct = -1;                            // --check: 같은 sched serial 과 추측 일치율
    Prof prof;
    bool skipped = false;
    double step_s() const { return ph_local + ph_global + ph_remove + ph_spec + ph_mixed + ph_end; }
};
static inline void fnv(uint64_t& h, uint64_t x) { for (int i = 0; i < 8; i++) { h ^= (x >> (8 * i)) & 0xff; h *= 1099511628211ULL; } }

struct Sim {
    const Cfg& c; const std::vector<Rec>& data; SpecParams sp;
    std::unique_ptr<ConcurrentTree> global;
    std::vector<std::unique_ptr<SuffixTree>> local;   // 개인 트리: 항상 Arctic 원본
    std::vector<std::vector<int32_t>> seq;            // 요청별 prompt + response (정답)
    std::vector<int> plen, pos;                       // pos = 지금까지 확정된 토큰 수
    std::vector<std::vector<int32_t>> pending;        // 이번 스텝에 트리에 붙일 토큰
    std::vector<Draft> drafts;
    int evict_next = 0;

    Sim(const Cfg& c_, const std::vector<Rec>& d, const TreeFactory& make) : c(c_), data(d) {
        sp.max_spec_tokens = c.SPEC;
        global = make(c.DEPTH);
        for (int k = 0; k < c.N_CACHED; k++) global->prepare_seq(k, (int)data[k].resp.size());
        for (int i = 0; i < c.B; i++) global->prepare_seq(c.N_CACHED + i, (int)data[c.N_CACHED + i].resp.size());
        for (int k = 0; k < c.N_CACHED; k++) global->extend(k, std::span<const int32_t>(data[k].resp));
        for (int i = 0; i < c.B; i++) {
            const Rec& r = data[c.N_CACHED + i];
            std::vector<int32_t> s = r.prompt; s.insert(s.end(), r.resp.begin(), r.resp.end());
            seq.push_back(s); plen.push_back((int)r.prompt.size());
            local.push_back(std::make_unique<SuffixTree>(c.DEPTH));
            local.back()->extend(0, std::span<const int32_t>(r.prompt));
            pending.push_back({s[plen[i]]});          // 첫 샘플 토큰
            pos.push_back(plen[i] + 1);
        }
        drafts.resize(c.B);
    }
    bool live(int i) const { return pos[i] < (int)seq[i].size() - 1; }

    void add_local(int i) {
        double t0 = now_s();
        local[i]->extend(0, std::span<const int32_t>(pending[i]));
        tctr().work[W_LOCAL_ADD] += now_s() - t0;
    }
    void add_global(int i) {
        double t0 = now_s();
        global->extend(c.N_CACHED + i, std::span<const int32_t>(pending[i]));
        tctr().work[W_GLOBAL_ADD] += now_s() - t0;
    }
    void remove_cached(int id) {
        double t0 = now_s();
        global->remove(id);
        tctr().work[W_REMOVE] += now_s() - t0;
    }
    void spec(int i) {
        int lo = std::max(0, pos[i] - c.DEPTH);
        std::span<const int32_t> ctx(seq[i].data() + lo, pos[i] - lo);
        double t0 = now_s();
        Draft d1 = local[i]->speculate(ctx, sp.max_spec_tokens, sp.max_spec_factor, sp.max_spec_offset,
                                       sp.min_token_prob, sp.use_tree_spec);
        double t1 = now_s();
        Draft d2 = global->speculate(ctx, sp);
        double t2 = now_s();
        tctr().work[W_SPEC_LOCAL] += t1 - t0;
        tctr().work[W_SPEC_GLOBAL] += t2 - t1;
        drafts[i] = d1.score >= d2.score ? std::move(d1) : std::move(d2);
    }
    // 검증 = GPU 역할. 정답(teacher forcing)과 비교해 수락 길이를 정하고 다음 pending 을 만든다
    void verify(const std::vector<int>& lv, int step, Result& r) {
        uint64_t sh = 1469598103934665603ULL;
        for (int i : lv) {
            const auto& d = drafts[i].token_ids;
            int a = 0;
            while (a < (int)d.size() && pos[i] + a < (int)seq[i].size() - 1 && d[a] == seq[i][pos[i] + a]) a++;
            fnv(r.hash, (uint64_t)i << 32 | (uint32_t)step); fnv(sh, i);
            uint64_t dh = 1469598103934665603ULL;
            for (int32_t t : d) { fnv(r.hash, (uint32_t)t); fnv(sh, (uint32_t)t); fnv(dh, (uint32_t)t); }
            r.rs_hash[{i, step}] = dh;
            r.accepted += a; r.drafted += d.size(); r.req_steps++;
            int n_new = std::min(a + 1, (int)seq[i].size() - pos[i]);
            pending[i].assign(seq[i].begin() + pos[i], seq[i].begin() + pos[i] + n_new);
            pos[i] += n_new;
        }
        r.step_hash.push_back(sh);
    }
    void profile_step(const std::vector<int>& lv, Prof& P) {
        std::unordered_map<int32_t, long long> cnt;
        long long total = 0, root = 0;
        for (int i : lv) {
            int n = (int)pending[i].size();
            for (int p = pos[i] - n; p < pos[i]; p++) {
                root++;
                for (int j = std::max(plen[i], p - c.DEPTH + 1); j <= p; j++) { cnt[seq[i][j]]++; total++; }
            }
        }
        long long mx = 0;
        for (auto& [k, v] : cnt) { mx = std::max(mx, v); P.all[k] += v; }
        P.steps++; P.total += total; P.root += root; P.maxsub += mx; P.distinct += cnt.size();
        for (int t = 0; t < 5; t++) {
            double even = (double)total / PROF_T[t];
            P.den_sub[t] += std::max(even, (double)mx);
            P.den_root[t] += std::max({even, (double)mx, (double)root});
        }
    }
};

static Result run(const Cfg& c, const std::vector<Rec>& data, const std::string& mode,
                  const std::string& sched, int threads) {
    Result r;
    auto& reg = tree_registry();
    Sim s(c, data, reg.at(mode).make);
    TreeCaps cap = s.global->caps();
    if (sched == "mixed" && threads > 1 && !(cap.par_update && cap.read_with_update)) {
        r.skipped = true; r.note = "mixed needs par_update+read_with_update"; return r;
    }
    for (auto& x : g_ctr) x = ThreadCounters{};
    std::unique_ptr<Pool> pool;
    if (threads > 1) pool = std::make_unique<Pool>(threads);
    Pool* P = pool.get();
    Pool* Plocal = c.local_par ? P : nullptr;
    Pool* Pupd = cap.par_update ? P : nullptr;
    Pool* Pread = cap.par_read ? P : nullptr;
    bool rm_par = cap.par_update && cap.par_remove;
    if (threads > 1 && !cap.par_update && sched != "serial") r.note = "global update 1 thread (caps)";
    auto timed = [](double& acc, auto&& fn) { double t0 = now_s(); fn(); acc += now_s() - t0; };

    double w0 = now_s();
    for (int step = 0; step < c.STEPS; step++) {
        std::vector<int> lv;
        for (int i = 0; i < c.B; i++) if (s.live(i)) lv.push_back(i);
        if (lv.empty()) break;
        if (c.profile) s.profile_step(lv, r.prof);
        std::vector<int> ev;                                   // 이번 스텝에 캐시에서 뺄 응답 (FIFO)
        for (int e = 0; e < c.EVICT && s.evict_next < c.N_CACHED; e++) ev.push_back(s.evict_next++);
        r.removed += ev.size();
        int nl = (int)lv.size(), ne = (int)ev.size();

        if (sched == "serial") {
            timed(r.ph_mixed, [&] {
                for (int id : ev) s.remove_cached(id);
                for (int i : lv) { s.add_local(i); s.add_global(i); s.spec(i); }
            });
        } else if (sched == "phase" || sched == "stale") {
            auto upd = [&] {
                timed(r.ph_local, [&] { par_for(Plocal, nl, 4, [&](int k) { s.add_local(lv[k]); }); });
                if (!rm_par) {
                    timed(r.ph_remove, [&] { for (int id : ev) s.remove_cached(id); });
                    timed(r.ph_global, [&] { par_for(Pupd, nl, 1, [&](int k) { s.add_global(lv[k]); }); });
                } else {
                    timed(r.ph_global, [&] {
                        par_for(Pupd, ne + nl, 1, [&](int k) { if (k < ne) s.remove_cached(ev[k]); else s.add_global(lv[k - ne]); });
                    });
                }
            };
            auto spc = [&] { timed(r.ph_spec, [&] { par_for(Pread, nl, 4, [&](int k) { s.spec(lv[k]); }); }); };
            if (sched == "phase") { upd(); spc(); } else { spc(); upd(); }
        } else if (sched == "mixed") {
            int ner = rm_par ? ne : 0;
            if (!rm_par) timed(r.ph_remove, [&] { for (int id : ev) s.remove_cached(id); });
            timed(r.ph_mixed, [&] {
                par_for(P, ner + nl, 1, [&](int k) {
                    if (k < ner) { s.remove_cached(ev[k]); return; }
                    int i = lv[k - ner]; s.add_local(i); s.add_global(i); s.spec(i);
                });
            });
        } else {
            std::fprintf(stderr, "unknown sched %s\n", sched.c_str()); std::exit(1);
        }
        timed(r.ph_end, [&] { s.global->end_step(); });
        if (c.check) r.tree_digest.push_back(s.global->digest());   // 시간 측정 밖
        s.verify(lv, step, r);
    }
    r.wall = now_s() - w0;
    for (auto& x : g_ctr) {
        for (int w = 0; w < W_N; w++) r.work[w] += x.work[w];
        r.lock_wait += x.lock_wait_s; r.retries += x.retries;
        for (int e = 0; e < 4; e++) r.extra[e] += x.extra[e];
    }
    if (c.check) {
        if (c.EVICT > 0) r.integrity = "skip(evict)";   // Arctic check_integrity 는 seq_id 가 0..n-1 연속이라고 가정 (트리 내용 비교는 함)
        else { std::string e = s.global->check(); r.integrity = e.empty() ? "ok" : e; }
    }
    return r;
}

// ---------------------------------------------------------------- --check 판정
static int g_n_pass = 0, g_n_pass_star = 0, g_n_fail = 0, g_n_na = 0;
static std::vector<std::string> g_fail_rows;
static double g_spec_pct = -1;   // 마지막 판정의 추측 일치율 (CSV 용)
static std::string judge(const Result& r, const Result& ref, const std::string& sched) {
    g_spec_pct = -1;
    if (r.tree_digest.empty() || r.tree_digest[0] == 0) { g_n_na++; return "n/a(digest 없음)"; }
    size_t n = std::min(r.tree_digest.size(), ref.tree_digest.size());
    for (size_t k = 0; k < n; k++)
        if (r.tree_digest[k] != ref.tree_digest[k]) { g_n_fail++; return "FAIL tree@step" + std::to_string(k); }
    if (r.tree_digest.size() != ref.tree_digest.size()) { g_n_fail++; return "FAIL steps"; }
    bool integ_ok = r.integrity == "ok" || r.integrity == "-" || r.integrity.rfind("skip", 0) == 0;
    if (!integ_ok) { g_n_fail++; return "FAIL integrity: " + r.integrity; }
    size_t eq = 0;
    for (auto& [k, h] : ref.rs_hash) { auto it = r.rs_hash.find(k); if (it != r.rs_hash.end() && it->second == h) eq++; }
    g_spec_pct = 100.0 * eq / std::max<size_t>(ref.rs_hash.size(), 1);
    if (sched != "mixed" && eq != ref.rs_hash.size()) {
        g_n_pass_star++;
        char b[64]; std::snprintf(b, sizeof b, "PASS* spec %.2f%%", g_spec_pct); return b;
    }
    g_n_pass++;
    return "PASS";
}

// ---------------------------------------------------------------- 출력
static FILE* g_csv = nullptr;
static std::string g_dname, g_date;
static void header() {
    std::printf("%-12s %-6s %3s | %8s | %7s %7s %7s %7s %6s | %7s %7s %7s %7s %7s | %6s %6s | %6s %7s | %7s %8s %3s | %s\n",
                "mode", "sched", "thr", "step ms", "ph:loc", "glob", "remove", "spec", "end",
                "w:locAd", "glbAd", "remove", "specL", "specG", "tok/st", "acc", "lockw%", "retry",
                "vllm%", "dAcc%", "det", "check / note");
}
static void report(const Cfg& c, const std::string& mode, const std::string& sched, int th,
                   const Result& r, const Result* ref, bool det) {
    if (r.skipped) { std::printf("%-12s %-6s %3d | skipped: %s\n", mode.c_str(), sched.c_str(), th, r.note.c_str()); return; }
    double rs = (double)std::max<long long>(r.req_steps, 1);
    double norm = c.B / rs * 1e3;                       // 초 → "배치 B 가 꽉 찬 스텝 1번당 ms"
    double step = r.step_s() * norm;
    double same = -1, dacc = 0;
    if (ref) {
        size_t eq = 0;
        for (auto& [k, h] : ref->rs_hash) { auto it = r.rs_hash.find(k); if (it != r.rs_hash.end() && it->second == h) eq++; }
        same = 100.0 * eq / std::max<size_t>(ref->rs_hash.size(), 1);
        dacc = 100.0 * ((double)r.accepted - ref->accepted) / std::max<long long>(ref->accepted, 1);
    }
    double lockw = 100.0 * r.lock_wait / std::max(r.step_s() * th, 1e-12);
    double tok = (double)(r.accepted + r.req_steps) / rs, acc = (double)r.accepted / std::max<long long>(r.drafted, 1);
    std::string chk = (r.verdict != "-" ? r.verdict : r.integrity) + (r.note.empty() ? "" : " " + r.note);
    char same_s[16] = "      -"; if (ref) std::snprintf(same_s, sizeof same_s, "%7.2f", same);
    std::printf("%-12s %-6s %3d | %8.3f | %7.3f %7.3f %7.3f %7.3f %6.3f | %7.3f %7.3f %7.3f %7.3f %7.3f | %6.3f %6.3f | %6.1f %7lld | %s %+8.3f %3s | %s\n",
                mode.c_str(), sched.c_str(), th, step, r.ph_local * norm, r.ph_global * norm, r.ph_remove * norm,
                r.ph_spec * norm, r.ph_end * norm, r.work[W_LOCAL_ADD] * norm, r.work[W_GLOBAL_ADD] * norm,
                r.work[W_REMOVE] * norm, r.work[W_SPEC_LOCAL] * norm, r.work[W_SPEC_GLOBAL] * norm,
                tok, acc, lockw, r.retries, same_s, dacc, det ? "yes" : "NO", chk.c_str());
    // 1스레드면 Amdahl 예측: 개인 갱신·추측은 완전 병렬, 전역 갱신+삭제만 순차로 남을 때
    if (th == 1 && sched != "mixed") {
        double tot = 0; for (double w : r.work) tot += w;
        double f = (r.work[W_GLOBAL_ADD] + r.work[W_REMOVE]) / std::max(tot, 1e-12);
        std::printf("    amdahl: global add+remove = %.1f%% of work -> if only that stays serial: x%.2f @4thr, x%.2f @12thr (limit x%.2f)\n",
                    100 * f, 1 / (f + (1 - f) / 4), 1 / (f + (1 - f) / 12), 1 / f);
    }
    static std::map<std::string, bool> prof_done;          // 같은 sched 의 프로파일은 한 번만 출력
    if (c.profile && r.prof.steps && !prof_done[sched]) {
        prof_done[sched] = true;
        const Prof& p = r.prof;
        std::vector<std::pair<long long, int32_t>> top;
        long long all = 0;
        for (auto& [k, v] : p.all) { top.push_back({v, k}); all += v; }
        std::sort(top.rbegin(), top.rend());
        std::printf("    subtree profile: node-ops/step %.0f, root-ops/step %.0f (%.1f%%), subtrees touched/step %.0f, "
                    "busiest subtree/step %.1f%%\n", p.total / p.steps, p.root / p.steps, 100 * p.root / p.total,
                    p.distinct / p.steps, 100 * p.maxsub / p.total);
        std::printf("    top subtrees (token id: share of all ops):");
        for (size_t k = 0; k < std::min<size_t>(5, top.size()); k++) std::printf(" %d:%.1f%%", top[k].second, 100.0 * top[k].first / all);
        std::printf("\n    subtree-lock ideal bound for global add (equal op cost):");
        for (int t = 0; t < 5; t++) std::printf("  T=%d x%.2f", PROF_T[t], p.total / p.den_sub[t]);
        std::printf("\n    ... if root ops also serialize under one root lock:  ");
        for (int t = 0; t < 5; t++) std::printf("  T=%d x%.2f", PROF_T[t], p.total / p.den_root[t]);
        std::printf("\n");
    }
    if (g_csv) {
        std::fprintf(g_csv, "%s,%u,%s,%s,%s,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%lld,%lld,%lld,%lld,%lld,%.3f,%.4f,%s,%s,%s,%.3f,%016llx\n",
                     g_date.c_str(), std::thread::hardware_concurrency(), g_dname.c_str(), mode.c_str(), sched.c_str(), th,
                     c.B, c.N_CACHED, c.STEPS, c.EVICT, c.local_par ? 1 : 0, step,
                     r.ph_local * norm, r.ph_global * norm, r.ph_remove * norm, r.ph_spec * norm, r.ph_end * norm,
                     r.work[W_LOCAL_ADD] * norm, r.work[W_GLOBAL_ADD] * norm, r.work[W_REMOVE] * norm,
                     r.work[W_SPEC_LOCAL] * norm, r.work[W_SPEC_GLOBAL] * norm, tok, acc, lockw, r.retries,
                     r.extra[0], r.extra[1], r.extra[2], r.extra[3], same, dacc, det ? "yes" : "no",
                     r.integrity.c_str(), r.verdict.c_str(), r.spec_pct, (unsigned long long)r.hash);
        std::fflush(g_csv);
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<std::string> pos_args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i]; auto p = a.find('=');
        if (p != std::string::npos) g_opt[norm_key(a.substr(0, p))] = a.substr(p + 1);
        else if (a.rfind("--", 0) == 0) g_opt[norm_key(a)] = "1";
        else pos_args.push_back(a);
    }
    if (g_opt.count("QUICK")) {   // 작은 설정 + 검사 (이미 준 옵션은 그대로)
        for (auto [k, v] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"B", "32"}, {"N_CACHED", "300"}, {"STEPS", "40"}, {"REPEAT", "1"}, {"CHECK", "1"},
                 {"THREADS", "1,4"}, {"SCHED", "phase,mixed"}, {"MODE", "all"}})
            g_opt.emplace(k, v);
    }
    if (g_opt.count("LIST") || pos_args.empty()) {
        std::printf("registered --mode values:\n");
        for (auto& [k, e] : tree_registry()) std::printf("  %-14s %s\n", k.c_str(), e.desc.c_str());
        if (pos_args.empty()) std::printf("usage: tree_bench tokens.bin --mode=serial,global_lock --sched=phase --threads=1,2,4 [--b=256 ...]\n");
        return 0;
    }
    auto data = load(pos_args[0].c_str());
    Cfg c{opt_i("B", 256), opt_i("N_CACHED", 10000), opt_i("STEPS", 100), opt_i("SPEC", 16), opt_i("DEPTH", 24),
          opt_i("EVICT", 0), opt_s("LOCAL", "par") == "par", opt_i("PROFILE", 0) != 0, opt_i("CHECK", 0) != 0};
    int rep = std::max(1, opt_i("REPEAT", 3));
    if ((int)data.size() < c.N_CACHED + c.B) { std::fprintf(stderr, "need %d records, have %zu\n", c.N_CACHED + c.B, data.size()); return 1; }
    auto modes = split(opt_s("MODE", "serial"));
    if (modes.size() == 1 && modes[0] == "all") { modes.clear(); for (auto& [k, e] : tree_registry()) modes.push_back(k); }
    auto scheds = split(opt_s("SCHED", "phase"));
    std::vector<int> ths; for (auto& t : split(opt_s("THREADS", "1"))) ths.push_back(std::atoi(t.c_str()));
    for (auto& m : modes) if (!tree_registry().count(m)) { std::fprintf(stderr, "unknown --mode=%s (see --list)\n", m.c_str()); return 1; }
    for (int t : ths) if (t < 1 || t > MAX_THREADS) { std::fprintf(stderr, "threads must be 1..%d\n", MAX_THREADS); return 1; }

    std::string csv = opt_s("CSV", "");
    if (!csv.empty()) {
        bool fresh = true;
        { std::ifstream f(csv); fresh = !f.good() || f.peek() == std::ifstream::traits_type::eof(); }
        g_csv = std::fopen(csv.c_str(), "a");
        if (g_csv && fresh)
            std::fprintf(g_csv, "date,hw_threads,data,mode,sched,threads,B,N_CACHED,STEPS,EVICT,local_par,step_ms,ph_local_ms,ph_global_ms,ph_remove_ms,ph_spec_ms,ph_end_ms,"
                                "w_local_add_ms,w_global_add_ms,w_remove_ms,w_spec_local_ms,w_spec_global_ms,tok_per_step,acc,lockwait_pct,retries,"
                                "extra0,extra1,extra2,extra3,same_as_vllm_order_pct,acc_diff_pct,deterministic,integrity,check,check_spec_pct,hash\n");
    }
    { std::time_t t = std::time(nullptr); char b[32]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&t)); g_date = b; }
    g_dname = pos_args[0];
    { auto s = g_dname.find_last_of("/\\"); if (s != std::string::npos) g_dname = g_dname.substr(s + 1); }

    std::printf("file=%s records=%zu B=%d N_CACHED=%d STEPS=%d SPEC=%d DEPTH=%d EVICT=%d local=%s repeat=%d hw_threads=%u\n",
                pos_args[0].c_str(), data.size(), c.B, c.N_CACHED, c.STEPS, c.SPEC, c.DEPTH, c.EVICT,
                c.local_par ? "par" : "serial", rep, std::thread::hardware_concurrency());
    std::printf("times: ms per full-batch step. ph:* = wall time of each phase, w:* = work time summed over threads.\n");

    Result ref; bool have_ref = opt_i("REF", 1) != 0;
    if (have_ref) { Cfg c1 = c; c1.profile = false; c1.check = false; ref = run(c1, data, "serial", "serial", 1); }
    std::map<std::string, Result> check_ref;   // --check 기준: 같은 sched, serial 트리, 1스레드
    if (c.check) for (auto& sc : scheds) { Cfg c1 = c; c1.profile = false; check_ref[sc] = run(c1, data, "serial", sc, 1); }
    if (c.check) std::printf("check: each row is compared with mode=serial, same sched, 1 thread (tree content every step + speculation)\n");
    header();
    for (auto& m : modes) for (auto& sc : scheds) for (int th : ths) {
        if (sc == "serial" && th != 1) continue;                  // serial 은 정의상 1스레드
        std::vector<Result> v;
        for (int k = 0; k < rep; k++) {
            Cfg ck = c; ck.profile = c.profile && k == 0; ck.check = c.check && k == 0;
            v.push_back(run(ck, data, m, sc, th));
            if (v.back().skipped) break;
        }
        bool det = true;
        for (auto& x : v) det &= x.hash == v[0].hash;
        std::string verdict0 = "-"; double spec0 = -1;
        if (c.check && !v[0].skipped) {
            verdict0 = judge(v[0], check_ref[sc], sc); spec0 = g_spec_pct;
            if (verdict0.rfind("FAIL", 0) == 0) g_fail_rows.push_back(m + " " + sc + " " + std::to_string(th) + "thr: " + verdict0);
        }
        Prof prof0 = v[0].prof; std::string integ0 = v[0].integrity;
        std::sort(v.begin(), v.end(), [](const Result& a, const Result& b) { return a.step_s() < b.step_s(); });
        Result med = v[v.size() / 2];
        med.prof = prof0; med.integrity = integ0; med.verdict = verdict0; med.spec_pct = spec0;
        report(c, m, sc, th, med, have_ref ? &ref : nullptr, det);
    }
    if (g_csv) std::fclose(g_csv);
    if (c.check) {
        std::printf("\nCHECK SUMMARY: PASS %d, PASS* %d, FAIL %d, n/a %d\n", g_n_pass, g_n_pass_star, g_n_fail, g_n_na);
        for (auto& f : g_fail_rows) std::printf("  %s\n", f.c_str());
        if (g_n_fail) return 2;
    }
    return 0;
}
