// premeasure_bench: 사전 실험 1~4 재현용 (본 실험 전에 문제 크기를 확인한 프로그램). 결과 보존을 위해 동작은 바꾸지 않는다.
//   cost      : 사전 실험 1. 요청-스텝당 갱신·추측 비용 (serial = vLLM 순서)
//   semantics : 사전 실험 2. serial / phase / stale 순서별 결과 비교 (1스레드, 3회)
//   specpar   : 사전 실험 3. phase 순서에서 추측만 락 없이 병렬
//   rwlock    : 사전 실험 4. 갱신 1스레드 + 추측 N스레드, 전역 reader-writer 락
//   all       : 위 4개 전부
//
// Arctic Inference SuffixTree(third_party/arctic)를 C++에서 직접 호출해
// arctic_inference.suffix_decoding.SuffixDecodingCache 와 같은 동작을 재현한다
// (전역 트리 = 지난 답변, 요청별 개인 트리 = 프롬프트 + 답변, 둘 중 점수가 높은 추측 사용, teacher forcing).
//
// 빌드 (저장소 루트에서):
//   g++ -O2 -DNDEBUG -std=c++20 -static -pthread premeasure/premeasure_bench.cpp third_party/arctic/suffix_tree.cc -o bin/premeasure_bench.exe
// 사용:
//   premeasure_bench <tokens.bin> <cost|semantics|specpar|rwlock|all> [B=256] [N_CACHED=1000] [STEPS=100]
//                    [SPEC=16] [DEPTH=24] [THREADS=1,2,4,6,8,12] [REPEAT=3]
// tokens.bin 형식: "KTOK"(파일 형식 표시) + 요청 수 n(int32) + 요청마다 [프롬프트 길이, 답변 길이, 토큰들] (모두 int32)
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../third_party/arctic/suffix_tree.h"

// rwlock 실험용 읽기·쓰기 락.
// Windows(MinGW)의 std::shared_mutex 는 winpthreads 의 pthread_rwlock(mutex + condvar)을 거쳐 경합 시 매우 느리다
// → Windows 에서는 운영체제 기본 락 SRWLOCK 을 쓴다 (common/locks.h 와 같은 이유).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
struct RWLock {
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
    void lock_shared() { AcquireSRWLockShared(&l); }
    void unlock_shared() { ReleaseSRWLockShared(&l); }
};
#else
using RWLock = std::shared_mutex;
#endif

// ---------------------------------------------------------------- 시간, 옵션, 데이터
using Clock = std::chrono::steady_clock;
static double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

struct Rec { std::vector<int32_t> prompt, resp; };   // 요청 하나 = 프롬프트 토큰 + 답변 토큰

static std::map<std::string, std::string> g_opt;    // KEY=VAL 옵션 (B=256 등)
static int opt_i(const char* k, int d) { auto it = g_opt.find(k); return it == g_opt.end() ? d : std::atoi(it->second.c_str()); }
static std::vector<int> opt_list(const char* k, const char* d) {
    std::string s = g_opt.count(k) ? g_opt[k] : d;
    std::vector<int> v; std::stringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) if (!t.empty()) v.push_back(std::atoi(t.c_str()));
    return v;
}

// 토큰 파일 읽기 (형식은 파일 맨 위 주석)
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
// 스텝마다 스레드를 새로 만들지 않고, 배리어로 깨워서 일을 시킨다
struct Pool {
    int n; std::vector<std::thread> th; std::barrier<> go, done; std::function<void(int)> job; bool stop = false;
    explicit Pool(int n_) : n(n_), go(n_ + 1), done(n_ + 1) {
        for (int t = 0; t < n; t++) th.emplace_back([this, t] {
            for (;;) { go.arrive_and_wait(); if (stop) return; job(t); done.arrive_and_wait(); }
        });
    }
    void run(std::function<void(int)> f) { job = std::move(f); go.arrive_and_wait(); done.arrive_and_wait(); }
    ~Pool() { stop = true; go.arrive_and_wait(); for (auto& t : th) t.join(); }
};

// ---------------------------------------------------------------- 시뮬레이션
// B: 배치(동시 요청 수), N_CACHED: 전역 트리에 미리 넣는 지난 답변 수, SPEC: 최대 추측 토큰 수, DEPTH: 트리 깊이
struct Cfg { int B, N_CACHED, STEPS, SPEC, DEPTH; };
struct Result {
    double t_add = 0, t_spec = 0, t_wall = 0, lock_wait = 0;   // 초 단위
    long long req_steps = 0, accepted = 0, drafted = 0;
    uint64_t hash = 1469598103934665603ULL;   // 모든 추측(요청, 스텝, 토큰)의 FNV-1a 해시 → 결정성 비교
    std::vector<uint64_t> step_hash;          // 스텝별 해시 (처음 달라진 스텝 찾기)
    std::map<std::pair<int,int>, uint64_t> rs_hash;   // (요청, 스텝) → 추측 해시 (같은 추측 비율 계산)
};
static inline void fnv(uint64_t& h, uint64_t x) { for (int i = 0; i < 8; i++) { h ^= (x >> (8 * i)) & 0xff; h *= 1099511628211ULL; } }

struct Sim {
    const Cfg& c; const std::vector<Rec>& data;
    std::unique_ptr<SuffixTree> global;                 // 전역 트리 (모든 요청이 공유)
    std::vector<std::unique_ptr<SuffixTree>> local;     // 요청별 개인 트리
    std::vector<std::vector<int32_t>> seq;    // 요청별 프롬프트 + 답변 (정답 토큰열)
    std::vector<int> plen, pos;               // pos = 지금까지 확정된 토큰 수
    std::vector<std::vector<int32_t>> pending;  // 이번 스텝에 트리에 붙일 토큰
    std::vector<Draft> drafts;

    Sim(const Cfg& c_, const std::vector<Rec>& d) : c(c_), data(d) {
        global = std::make_unique<SuffixTree>(c.DEPTH);
        for (int k = 0; k < c.N_CACHED; k++) global->extend(k, std::span<const int32_t>(data[k].resp));
        for (int i = 0; i < c.B; i++) {
            const Rec& r = data[c.N_CACHED + i];
            std::vector<int32_t> s = r.prompt; s.insert(s.end(), r.resp.begin(), r.resp.end());
            seq.push_back(s); plen.push_back((int)r.prompt.size());
            local.push_back(std::make_unique<SuffixTree>(c.DEPTH));
            local.back()->extend(0, std::span<const int32_t>(r.prompt));
            pending.push_back({s[plen[i]]});          // 첫 토큰은 추측 없이 확정
            pos.push_back(plen[i] + 1);
        }
        drafts.resize(c.B);
    }
    bool live(int i) const { return pos[i] < (int)seq[i].size() - 1; }   // 아직 답변이 남은 요청인가
    void add(int i) {          // 갱신: 개인 트리 + 전역 트리에 확정 토큰 기록
        std::span<const int32_t> t(pending[i]);
        local[i]->extend(0, t);
        global->extend(c.N_CACHED + i, t);
    }
    Draft spec(int i) const {  // 추측: 개인·전역 트리 중 점수가 높은 쪽 (vLLM 과 같은 규칙)
        int lo = std::max(0, pos[i] - c.DEPTH);
        std::span<const int32_t> ctx(seq[i].data() + lo, pos[i] - lo);
        Draft d1 = local[i]->speculate(ctx, c.SPEC, 1.0f, 0.0f, 0.1f, false);
        Draft d2 = global->speculate(ctx, c.SPEC, 1.0f, 0.0f, 0.1f, false);
        return d1.score >= d2.score ? d1 : d2;
    }
    // 검증 = GPU 역할. 정답(teacher forcing)과 비교해 맞은 길이만큼 확정하고 다음 pending 을 만든다
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
};

// mode: serial(vLLM 순서) | phase(갱신 전부 → 추측 전부) | stale(추측 전부 → 갱신 전부) | concurrent(갱신·추측 동시)
// threads: 추측 스레드 수
static Result run(const Cfg& c, const std::vector<Rec>& data, const std::string& mode, int threads) {
    Sim s(c, data);
    Result r;
    std::unique_ptr<Pool> pool;
    int workers = (mode == "concurrent") ? threads + 1 : threads;
    if (workers > 1) pool = std::make_unique<Pool>(workers);
    RWLock mtx;
    std::vector<double> lw(workers + 1, 0.0);
    double w0 = now_s();
    for (int step = 0; step < c.STEPS; step++) {
        std::vector<int> lv;
        for (int i = 0; i < c.B; i++) if (s.live(i)) lv.push_back(i);
        if (lv.empty()) break;
        if (mode == "serial") {                       // vLLM 순서: 요청마다 갱신 → 추측, 한 스레드
            for (int i : lv) {
                double t0 = now_s(); s.add(i); double t1 = now_s();
                s.drafts[i] = s.spec(i); double t2 = now_s();
                r.t_add += t1 - t0; r.t_spec += t2 - t1;
            }
        } else if (mode == "concurrent") {            // 갱신 스레드 1개 + 추측 스레드 N개, 전역 읽기·쓰기 락
            std::atomic<int> next{0};
            double t0 = now_s();
            pool->run([&](int t) {
                if (t == 0) {
                    for (int i : lv) {
                        double a = now_s(); std::unique_lock lk(mtx); lw[t] += now_s() - a;
                        s.add(i);
                    }
                } else {
                    for (int k; (k = next.fetch_add(1)) < (int)lv.size();) {
                        int i = lv[k];
                        double a = now_s(); std::shared_lock lk(mtx); lw[t] += now_s() - a;
                        s.drafts[i] = s.spec(i);
                    }
                }
            });
            r.t_spec += now_s() - t0;
        } else {                                      // phase / stale: 갱신은 한 스레드, 추측은 락 없이 병렬 (공유 카운터로 4개씩 나눠 가짐)
            auto do_add = [&] { double t0 = now_s(); for (int i : lv) s.add(i); r.t_add += now_s() - t0; };
            if (mode == "phase") do_add();
            double t0 = now_s();
            if (pool) {
                std::atomic<int> next{0};
                pool->run([&](int) {
                    for (int k; (k = next.fetch_add(4)) < (int)lv.size();)
                        for (int j = k; j < std::min(k + 4, (int)lv.size()); j++) s.drafts[lv[j]] = s.spec(lv[j]);
                });
            } else {
                for (int i : lv) s.drafts[i] = s.spec(i);
            }
            r.t_spec += now_s() - t0;
            if (mode == "stale") do_add();
        }
        s.verify(lv, step, r);
    }
    r.t_wall = now_s() - w0;
    for (double x : lw) r.lock_wait += x;
    return r;
}

// rep 번 반복해 (갱신 + 추측) 시간의 중앙값 결과를 쓴다
static Result median_run(const Cfg& c, const std::vector<Rec>& d, const std::string& m, int th, int rep) {
    std::vector<Result> v;
    for (int k = 0; k < rep; k++) v.push_back(run(c, d, m, th));
    std::sort(v.begin(), v.end(), [](const Result& a, const Result& b) { return a.t_add + a.t_spec < b.t_add + b.t_spec; });
    return v[v.size() / 2];
}

// ---------------------------------------------------------------- 출력
static void line(const char* tag, const Cfg& c, const Result& r) {
    double n = std::max<long long>(r.req_steps, 1);
    double us_add = r.t_add / n * 1e6, us_spec = r.t_spec / n * 1e6;
    std::printf("%-22s | %8.2f | %8.2f | %10.3f | %7.3f | %6.3f | %016llx\n", tag, us_add, us_spec,
                (us_add + us_spec) * c.B / 1000.0, (double)(r.accepted + r.req_steps) / n,
                (double)r.accepted / std::max<long long>(r.drafted, 1), (unsigned long long)r.hash);
}
static void header() {
    std::printf("%-22s | %8s | %8s | %10s | %7s | %6s | %s\n", "mode", "add us", "spec us", "ms/step@B", "tok/st", "acc", "draft-hash");
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: premeasure_bench tokens.bin cost|semantics|specpar|rwlock|all [KEY=VAL...]\n"); return 1; }
    for (int i = 3; i < argc; i++) { std::string a = argv[i]; auto p = a.find('='); if (p != std::string::npos) g_opt[a.substr(0, p)] = a.substr(p + 1); }
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 출력을 바로 파일에 씀 (진행 상황 확인용)
    auto data = load(argv[1]);
    Cfg c{opt_i("B", 256), opt_i("N_CACHED", 1000), opt_i("STEPS", 100), opt_i("SPEC", 16), opt_i("DEPTH", 24)};
    int rep = opt_i("REPEAT", 3);
    std::string what = argv[2];
    if (what != "cost" && what != "semantics" && what != "specpar" && what != "rwlock" && what != "all") {
        std::fprintf(stderr, "unknown experiment %s (cost|semantics|specpar|rwlock|all)\n", what.c_str()); return 1;
    }
    if ((int)data.size() < c.N_CACHED + c.B) { std::fprintf(stderr, "need %d records, have %zu\n", c.N_CACHED + c.B, data.size()); return 1; }
    std::printf("file=%s records=%zu B=%d N_CACHED=%d STEPS=%d SPEC=%d DEPTH=%d hw_threads=%u\n", argv[1], data.size(),
                c.B, c.N_CACHED, c.STEPS, c.SPEC, c.DEPTH, std::thread::hardware_concurrency());

    if (what == "cost" || what == "all") {
        std::printf("\n[1 cost] cost per request-step (serial = vLLM order), median of %d\n", rep); header();
        line("serial", c, median_run(c, data, "serial", 1, rep));
    }
    if (what == "semantics" || what == "all") {
        std::printf("\n[2 semantics] serial vs phase vs stale (single thread), 3 runs each\n"); header();
        Result base;
        for (std::string m : {"serial", "phase", "stale"}) {
            Result r0 = run(c, data, m, 1); bool det = true;
            for (int k = 0; k < 2; k++) det &= run(c, data, m, 1).hash == r0.hash;
            if (m == "serial") base = r0;
            line(m.c_str(), c, r0);
            size_t same = 0; while (same < std::min(base.step_hash.size(), r0.step_hash.size()) && base.step_hash[same] == r0.step_hash[same]) same++;
            double dacc = 100.0 * ((double)r0.accepted - base.accepted) / std::max<long long>(base.accepted, 1);
            size_t eq = 0;
            for (auto& [k, h] : base.rs_hash) { auto it = r0.rs_hash.find(k); if (it != r0.rs_hash.end() && it->second == h) eq++; }
            std::printf("    -> deterministic(3 runs)=%s, identical drafts vs serial=%.2f%% (req-steps), first differing step=%zu/%zu, accepted diff vs serial=%+.3f%%\n",
                        det ? "yes" : "NO", 100.0 * eq / std::max<size_t>(base.rs_hash.size(), 1), same, base.step_hash.size(), dacc);
        }
    }
    if (what == "specpar" || what == "all") {
        std::printf("\n[3 specpar] phase mode, read-only parallel speculate (no lock), median of %d\n", rep);
        std::printf("%-8s | %10s | %10s | %8s | %s\n", "threads", "add ms/st", "spec ms/st", "speedup", "same-as-1T");
        double base = 0; uint64_t h1 = 0;
        for (int th : opt_list("THREADS", "1,2,4,6,8,12")) {
            Result r = median_run(c, data, "phase", th, rep);
            double norm = (double)c.B / std::max<long long>(r.req_steps, 1) * 1e3;   // 배치 B 가 꽉 찬 스텝 1번당 ms
            double spec_ms = r.t_spec * norm, add_ms = r.t_add * norm;
            if (th == 1) { base = spec_ms; h1 = r.hash; }
            std::printf("%-8d | %10.3f | %10.3f | %8.2f | %s\n", th, add_ms, spec_ms, base / spec_ms, r.hash == h1 ? "yes" : "NO");
        }
    }
    if (what == "rwlock" || what == "all") {
        std::printf("\n[4 rwlock] 1 updater + N speculators, global reader-writer lock (SRWLOCK on Windows), nondeterministic, median of %d\n", rep);
        std::printf("%-8s | %12s | %12s | %8s | %s\n", "spec thr", "step ms", "lockwait ms", "wait %", "acc");
        for (int th : opt_list("THREADS", "1,2,4,6,8,12")) {
            Result r = median_run(c, data, "concurrent", th, rep);
            double norm = (double)c.B / std::max<long long>(r.req_steps, 1) * 1e3;
            double step_ms = r.t_spec * norm, lw_ms = r.lock_wait * norm;
            std::printf("%-8d | %12.3f | %12.3f | %7.1f%% | %.4f\n", th, step_ms, lw_ms,
                        100.0 * lw_ms / (step_ms * (th + 1)), (double)r.accepted / std::max<long long>(r.drafted, 1));
        }
    }
    return 0;
}
