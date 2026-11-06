# suffix-tree-sync

빅데이터처리 기말 프로젝트 코드 저장소입니다.

LLM 서빙 엔진 vLLM에는 **suffix decoding**이라는 가속 기능이 있습니다. 지난 답변들을 **트리 하나**에 모아 두고, 그걸 보고 다음 토큰을 미리 추측합니다. 이 트리는 모든 요청이 같이 쓰기 때문에 지금은 **한 스레드가 순서대로** 고칩니다.
우리는 이 트리 갱신을 수업 8장의 동기화 방식 4가지로 병렬화합니다. 그리고 같은 측정 틀(`tree_bench`)로 **속도**와 **순차로 했을 때와 같은 결과인지**를 비교합니다.

실제로 하는 일은 **C++ 트리 자료구조에 락을 넣는 것**입니다.
마지막에 이 방식들을 vLLM의 실제 Python 호출 경로에 넣어서, C++에서 빨라진 것이 실제로도 남는지 확인합니다 (7장). 각 방식은 7장 끝의 규칙만 지키면 그대로 쓰입니다.

| 담당 | 방식 이름 (`--mode`, Git 브랜치) | 수업 패턴 | 한 줄 설명 | 함께 맡는 일 |
|---|---|---|---|---|
| 공통 | `serial`, `global_lock` (틀에 포함) | 기준 | 순차 원본 / 트리 전체에 락 1개 | |
| subtree_lock 담당 | `subtree_lock` | Coarse-grained | 첫 토큰(서브트리)마다 락 | 공통 틀 유지, PR 합치기, vLLM 실제 경로 측정 (7장) |
| node_lock 담당 | `node_lock` | Fine-grained | 노드마다 락, 부모·자식 두 칸씩 잡으며 내려가기 | 락 종류 비교, 통합 측정 |
| optimistic 담당 | `optimistic` | Optimistic | 안 잠그고 내려가서 고칠 곳만 잠근 뒤 다시 확인 | ThreadSanitizer 전 방식 검사 |
| lazy 담당 | `lazy` | Lazy | 삭제는 표시만 하고 실제 제거는 나중에 | 측정 스크립트 `run_all.bat`·`plot.py` |

---

## 1. 처음 한 번만

### 준비물
| 준비물 | 설치 | 확인 |
|---|---|---|
| Git | `winget install Git.Git` | `git --version` |
| C++ 컴파일러 (g++ 13 이상) | `winget install BrechtSanders.WinLibs.POSIX.UCRT` | 새 cmd 창에서 `g++ --version` |
| Python 3 | `winget install Python.Python.3.12` | `python --version` |

- 명령은 전부 **cmd**(명령 프롬프트) 기준입니다.
- 설치한 뒤에는 **cmd 창을 새로 열어야** 새 프로그램이 잡힙니다
- 저장소는 **영어로 된 경로**에 두세요 (예: `C:\work`). 한글 경로는 일부 도구가 못 읽습니다

### 순서
```
cd /d C:\work
git clone https://github.com/scj7565/suffix-tree-sync.git
cd suffix-tree-sync
scripts\build.bat
bin\tree_bench data\tokens_code.bin --quick
```
- `build.bat` 끝에 `registered --mode values:`와 방식 목록이 나오면 빌드 성공입니다 (첫 빌드는 1분 정도)
- `--quick` 맨 끝에 `CHECK SUMMARY: PASS ..., FAIL 0`이 나오면 준비 끝입니다

---

## 2. 코드 읽기

이 순서로 보면 됩니다. 트리 코드(`suffix_tree.cc`)가 제일 중요합니다.

| 순서 | 파일 | 볼 것 |
|---|---|---|
| 1 | `common/tree_iface.h` | 내 방식이 구현할 함수들 (`extend`, `remove`, `speculate`, `end_step`, `caps`) |
| 2 | `trees/tree_global_lock.cc` | 가장 간단한 예시. 원본 트리 앞뒤에 락 하나 |
| 3 | `third_party/arctic/suffix_tree.h` | `Node` 구조: `count`, `children`(자식 맵), `head_child`·`next_sibling`(count 순 형제 리스트), `parent` |
| 4 | `third_party/arctic/suffix_tree.cc`의 `append` | 토큰 하나를 넣을 때 고치는 곳. **우리가 병렬화하는 부분** |
| 5 | 같은 파일의 `remove`, `speculate` | 삭제와 추측(읽기) |

방식별로 특히 볼 곳은 이렇습니다.

| 담당 | 특히 볼 곳 |
|---|---|
| `subtree_lock` | `append`가 루트 바로 아래(첫 토큰) 자식을 어떻게 찾고 만드는지. 루트에서 무엇이 바뀌는지 (count, 형제 순서) |
| `node_lock` | 노드 쪼개기(경로 압축된 노드를 둘로 나눔)와 형제 재정렬. 한 번에 어떤 노드들을 같이 고치는지 |
| `optimistic` | `speculate`와 `_match_context`가 트리를 내려가는 방식. 내려가는 도중 노드가 바뀌면 어디가 깨지는지 |
| `lazy` | `remove`가 무엇을 지우고 어떤 포인터를 고치는지 |

실행 순서 옵션 `--sched`의 뜻도 알아 두면 좋습니다.

| `--sched` | 한 스텝 안에서 하는 일 |
|---|---|
| `serial` | 실제 vLLM 순서. 요청마다 [갱신 → 추측], 한 스레드 |
| `phase` | **기본값.** 갱신을 전부 먼저 하고, 추측을 전부 나중에. 각 단계 안은 병렬 |
| `stale` | 추측을 먼저, 갱신을 나중에 |
| `mixed` | 요청마다 [갱신 → 추측]을 여러 스레드가 동시에 (읽기와 쓰기가 섞임) |

---

## 3. 내 방식 만들기

### 시작 (한 번만)
```
git checkout -b subtree_lock
python tools\new_tree.py subtree_lock
scripts\build.bat
bin\tree_bench data\tokens_code.bin --quick --mode=subtree_lock
```
`subtree_lock` 자리에 자기 방식 이름을 넣으세요.

만들어지는 파일 (**이 파일들만 고칩니다**):

| 파일 | 내용 |
|---|---|
| `trees/<이름>/st_<이름>.h`, `st_<이름>.cc` | 트리 원본의 복사본 (자기 이름 namespace). **여기에 락을 넣습니다** |
| `trees/tree_<이름>.cc` | 측정 틀과 연결하는 짧은 파일. `caps()`와 설명 문구를 여기서 고침 |

처음엔 원본과 똑같아서 `PASS`가 나와야 정상입니다.

- 남의 파일과 `bench/`, `common/`, `third_party/`, `scripts/`, `vllm_path/`는 고치지 않습니다 (lazy 담당의 `run_all.bat`·`plot.py`는 예외)
- 작업은 자기 브랜치에서만. 다 되면 GitHub에서 내 브랜치 → `main`으로 Pull Request (`main`에 직접 push 금지)
- 공통 틀이 바뀌었다는 공지를 받으면 `git fetch` → `git merge origin/main`
- `results/`, `bin/`은 올리지 않습니다 (`.gitignore`가 막아 줌). `data/`의 두 파일은 고치지 않습니다

### 매일 하는 반복
```
(코드 수정) → scripts\build.bat → bin\tree_bench data\tokens_code.bin --quick --mode=<이름>
→ FAIL 0 이면  git add trees  →  git commit -m "무엇을 했는지"  →  git push
```

### `caps()`: 측정 틀에게 "무엇을 동시에 불러도 되는지" 알려 주기
처음에는 전부 안전한 쪽(false)이라, `--threads=4`를 줘도 **갱신은 한 스레드로** 돌아갑니다. 결과 표에 `global update 1 thread (caps)`가 보이는 게 그 뜻입니다.
락을 넣어 안전해지면 해당 항목을 `true`로 바꾸세요. 그래야 실제로 여러 스레드가 동시에 부릅니다.

| 항목 (순서대로) | true로 바꾸는 조건 |
|---|---|
| `par_update` | `extend()`를 여러 스레드가 동시에 불러도 안전 (**모든 방식의 목표**) |
| `par_remove` | `remove()`를 `extend()`/`remove()`와 동시에 불러도 안전 (`lazy` 목표) |
| `par_read` | `speculate()`끼리 동시에 불러도 안전 (원본도 true) |
| `read_with_update` | `speculate()`를 갱신·삭제와 동시에 불러도 안전 (`--sched=mixed`에 필요) |

### 기록하면 결과 표에 나오는 것
| 하고 싶은 것 | 코드 | 결과 표 열 |
|---|---|---|
| 락 기다린 시간 | `{ LockWait w; mu.lock(); }` (중괄호까지 그대로) | `lockw%` |
| 재시도 횟수 (낙관적 등) | `tctr().retries++;` | `retry` |
| 그 밖의 횟수 | `tctr().extra[0]++;` (0~3번 칸) | CSV `extra0~3` |
| 스텝 사이 정리 (미뤄 둔 삭제 등) | `tree_<이름>.cc`의 `end_step()` 안에 | `end` |

공통 락 `RWLock`, `Mutex`는 `common/locks.h`에 있습니다. Windows에서는 빠른 SRWLOCK을 씁니다.

---

## 4. 검사 결과 읽기 (`--check`, `--quick`에 포함)

각 실행을 **같은 `--sched`의 serial 1스레드**(정답)와 비교합니다. 스텝마다 트리 내용(경로별 count)과 추측 결과를 봅니다.

| 판정 | 뜻 | 할 일 |
|---|---|---|
| `PASS` | 트리 내용·추측·구조 모두 정답과 같음 | 없음 |
| `PASS*` | 트리 내용은 같고 추측만 일부 다름 | count가 같은 형제의 순서 차이일 수 있음 (정상일 수 있음). 비율을 기록해 두고 optimistic 담당에게 공유 |
| `FAIL tree@step5` | 5번째 스텝 끝에서 트리 내용이 다름 | 갱신이 빠졌거나 두 번 들어감 = **경쟁 버그**. 스레드 1로도 FAIL이면 락이 아니라 로직 버그 |
| `FAIL integrity: ...` | 부모 포인터, count 합 등 구조가 깨짐 | 버그 |
| `n/a(digest 없음)` | `tree_<이름>.cc`에 `digest()`가 없음 | `new_tree.py`로 만든 파일이면 생기지 않음 |
| 프로그램이 그냥 꺼짐 | 동시 접근으로 메모리가 깨짐 | 버그. 아래 6장 ThreadSanitizer로 위치 찾기 |

- 삭제까지 섞어서 검사: `--quick --evict=2` (이때 구조 검사는 건너뛰고 트리 내용만 비교)
- 반복해서 돌려 보기: 경쟁 버그는 가끔만 나옵니다. `--quick`을 여러 번, `--threads=12`로도 돌려 보세요

---

## 5. 측정하기

```
bin\tree_bench data\tokens_code.bin --mode=global_lock,subtree_lock --sched=phase --threads=1,2,4,6,8,12 --b=256 --n_cached=10000 --csv=results\my.csv
```

| 옵션 | 기본값 | 뜻 |
|---|---|---|
| `--mode` | serial | 방식. 쉼표로 여러 개, `all`은 전부 |
| `--sched` | phase | 실행 순서 (2장 표) |
| `--threads` | 1 | 스레드 수. 쉼표로 여러 개 |
| `--b` | 256 | 한 스텝에 같이 처리하는 요청 수 (배치) |
| `--n_cached` | 10000 | 전역 트리에 미리 넣어 두는 지난 답변 수 (트리 크기) |
| `--steps`, `--repeat` | 100, 3 | 스텝 수, 반복 횟수 (중앙값 사용) |
| `--evict` | 0 | 스텝마다 지우는 지난 답변 수 |
| `--profile=1` | | 서브트리별 갱신 쏠림, 서브트리 락의 이론 상한 (`subtree_lock` 참고용) |
| `--csv=파일` | | 한 줄 = 한 조건으로 추가 저장 |

결과 표의 주요 열

| 열 | 뜻 |
|---|---|
| `step ms` | 스텝 하나 시간. **작을수록 좋음**, 주 지표 |
| `ph:loc / glob / remove / spec` | 단계별 실제 걸린 시간 (개인 트리 갱신 / **전역 트리 갱신** / 삭제 / 추측) |
| `w:*` | 같은 일을 스레드들이 쓴 시간의 합. `ph`보다 많이 크면 기다리느라 낭비 |
| `lockw%` | 전체 시간 중 락 기다린 비율 |
| `retry` | 재시도 횟수 |
| `vllm%` | 실제 vLLM 순서(serial)와 추측이 같은 비율. phase면 원래 99%대 |
| `det` | 3번 반복했을 때 결과가 똑같았는지 |
| `amdahl:` 줄 | 1스레드 결과로 계산한 이론 상한 |

- 각자 PC 숫자는 **개발 확인용**입니다. 최종 숫자는 각자 방식을 main에 합친 뒤 한 PC에서 한꺼번에 잽니다 (node_lock 담당)
- 잴 때는 전원 옵션 "고성능", 브라우저·게임 런처 등 끄기

---

## 6. 경쟁 버그 찾기 (ThreadSanitizer, WSL2 또는 Colab)

ThreadSanitizer는 실행 중에 "두 스레드가 같은 메모리를 순서 보장 없이, 한쪽은 쓰면서" 건드린 순간을 잡습니다. 이번 실행 결과가 맞았어도 잡아 줍니다. 대신 5~15배 느려서 작은 설정으로 돌립니다.
Windows에서는 안 되고 Linux가 필요합니다. 둘 중 편한 쪽을 쓰세요.

| | WSL2 (내 PC 안의 Linux) | Colab |
|---|---|---|
| 준비 | 처음 한 번 설치 (관리자 권한, 재부팅 1번) | 없음 (브라우저) |
| 반복 실행 | 빠름. 코드 고치고 바로 다시 실행 | 매번 zip 업로드 |
| 추천 | 자주 돌릴 사람 (optimistic 담당 등) | 가끔 한 번 돌릴 사람 |

### WSL2
처음 한 번: **관리자 cmd**에서 아래를 실행하고 재부팅합니다. 재부팅 후 뜨는 Ubuntu 창에서 사용자 이름·비밀번호를 정합니다.
```
wsl --install -d Ubuntu-24.04
```
Ubuntu 창에서 (처음 한 번):
```
sudo apt update && sudo apt install -y g++ git
cd ~ && git clone https://github.com/scj7565/suffix-tree-sync.git && cd suffix-tree-sync
```
- 저장소는 Windows 폴더(`/mnt/c/...`)가 아니라 **WSL 안(`~`)에 따로 clone** 하세요. `/mnt/c`에서 빌드하면 많이 느립니다

검사할 때마다 (Ubuntu 창):
```
cd ~/suffix-tree-sync && git checkout <내 브랜치> && git pull
TSAN=1 bash scripts/build.sh
bin/tree_bench_tsan data/tokens_code.bin --mode=<이름> --sched=phase --threads=4 --b=32 --n_cached=200 --steps=10 --repeat=1 --ref=0
```

### Colab
1. 저장소 폴더를 zip으로 묶어 Colab에 업로드 (`data` 폴더 포함)
2. Colab 셀에서:
```
!unzip -q suffix-tree-sync.zip
!cd suffix-tree-sync && TSAN=1 bash scripts/build.sh
!cd suffix-tree-sync && bin/tree_bench_tsan data/tokens_code.bin --mode=<이름> --sched=phase --threads=4 --b=32 --n_cached=200 --steps=10 --repeat=1 --ref=0
```

### 결과 읽기
| 나온 것 | 뜻 |
|---|---|
| 아무 경고 없음 | 통과 |
| `WARNING: ThreadSanitizer: data race` | 경쟁 발견. 아래 `Read ... by thread T1` / `Previous write ... by thread T3` 두 묶음에서 **내 파일(`trees/<이름>/...`)이 나오는 가장 위 줄**이 원인 위치 |
| `FATAL: ThreadSanitizer: unexpected memory mapping` | WSL2에서 가끔 나옴. `sudo sysctl vm.mmap_rnd_bits=28` 실행 후 다시 |

- 노드만이 아니라 트리 전체가 같이 쓰는 표(`_seqs`, `_active_nodes`, `append` 첫 부분)도 경쟁 대상입니다. 락으로 보호하거나 병렬 구간 전에 미리 만들어 두세요
- `optimistic`·`lazy`는 처음엔 경고가 나오는 게 정상입니다. 락 없이 읽는 칸(자식 포인터, count, 삭제 표시 등)을 `std::atomic`으로 바꿔야 없어집니다
- 개발 중에는 주 1회 자기 방식만 돌려 보면 충분합니다. 전 방식 검사는 main에 합친 뒤 optimistic 담당이 합니다

---

## 7. vLLM 실제 경로 측정

`tree_bench`는 C++ 안에서만 잽니다. 실제 vLLM은 Python에서 **요청마다** C++ 트리를 부릅니다 (요청 256개면 스텝마다 갱신·추측 512번). 이때 GIL을 잡은 채 한 스레드로 돕니다.
그래서 "C++에서 빨라진 만큼 실제 vLLM 경로에서도 빨라지는가"를 따로 확인합니다.

### 비교하는 세 가지
| 이름 | 하는 일 | 보여 주는 것 |
|---|---|---|
| 요청별 호출 | 지금 vLLM 그대로. Python이 요청마다 C++ 호출 | 출발점 |
| 묶음 호출, 1스레드 | 스텝마다 C++ 함수 **한 번**에 전부 넘김. 안에서는 `serial` | Python 호출 비용을 없앤 효과 |
| 묶음 호출, N스레드 | 묶음 함수 안에서 GIL을 풀고 `std::thread`로 우리 방식(`--mode`) 실행 | **우리 기여** (위 둘과 분리해서 보여 줌) |

- Python 스레드는 쓰지 않습니다. 일 하나가 10~20µs로 작아서 GIL을 요청마다 풀고 잡으면 오히려 느려집니다
- vLLM의 답변·스케줄링·GPU는 그대로입니다. 묶어서 부르기만 하면 결과가 비트 단위로 같고, `phase` 병렬이면 추측이 99~100% 같습니다 (5장 `vllm%` 열)
- GPU 없이 CPU 시간만 잽니다. 전체 속도 향상 폭은 GPU 한 스텝 시간(Colab에서 잠깐 측정, 안 되면 개발사 보고치)을 더해 추정합니다

### 환경
| 항목 | 내용 |
|---|---|
| 환경 | Linux 필요 → PC의 **WSL2** (6장 설치와 같음, `tree_bench`와 같은 CPU) |
| 필요한 것 | vLLM이 쓰는 Arctic Inference 소스 빌드 (cmake, nanobind). 저장소에는 넣지 않음 |
| 코드 위치 | `vllm_path/` (준비 중, subtree_lock 담당만 수정) |
| 순서 | 요청별 호출·묶음 1스레드를 먼저 재고, 4방식을 main에 합친 뒤 N스레드를 잼 |

### 각 방식이 지킬 것 (내 방식이 그대로 쓰이도록)
| 규칙 | 이유 |
|---|---|
| `common/tree_iface.h`의 함수만으로 동작하게 | 묶음 함수가 인터페이스로만 부름 |
| `caps()`를 실제 상태와 맞게 | 묶음 함수도 `caps()`를 보고 병렬로 부를지 정함 |
| **Linux(g++)에서도 빌드되게** | WSL2에서 빌드함. Windows 전용 함수(`windows.h`, `_Interlocked...`) 대신 `std::atomic`, `std::mutex`, `common/locks.h` 사용 |
| 전역 변수 대신 트리 객체 안에 상태 두기 | 같은 프로그램에서 여러 번 만들고 지움 |
| 화면 출력(`printf` 등)은 디버깅 때만 | 측정 시간에 섞임 |

6장 ThreadSanitizer가 Linux에서 통과하면 위 규칙 대부분은 자동으로 지켜진 것입니다.

---

## 8. 문제 해결

| 증상 | 원인 / 해결 |
|---|---|
| `[ERROR] g++ not found` | WinLibs 설치 후 **cmd 창을 새로** 여세요 |
| `[ERROR] g++ version 6 found, need 13+` | 옛 MinGW가 먼저 잡힘. WinLibs를 설치하면 `build.bat`가 자동으로 그쪽을 씀 |
| `'python'은(는) ... 아닙니다` | Python 설치 후 새 cmd 창. 안 되면 `py tools\new_tree.py <이름>` |
| `cannot open data\tokens_code.bin` | 저장소 폴더가 아닌 곳에서 실행함 (`cd suffix-tree-sync` 먼저) |
| `need N records, have M` | `--n_cached`가 데이터보다 큼. 값을 줄이기 |
| `unknown --mode=...` | 새 방식을 만든 뒤 `build.bat`을 다시 안 함 |
| `BUILD FAILED` | 위쪽 첫 번째 `error:` 줄이 진짜 원인. 그 파일·줄을 확인 |
| 결과에 `skipped: mixed needs ...` | `caps()`가 아직 false. 정상 (해당 조합만 건너뜀) |
| 결과에 `global update 1 thread (caps)` | `par_update`가 아직 false라 갱신이 1스레드로 돌았음. 락을 넣은 뒤 true로 |
| 스레드를 늘렸더니 느려짐 | 락 경합 (`lockw%` 확인). 그 자체가 분석할 결과입니다 |

---

## 9. 폴더 구조

| 폴더 | 내용 | 누가 고치나 |
|---|---|---|
| `bench/tree_bench.cpp` | 측정 틀 (실행 순서, 시간 측정, 검사, CSV) | subtree_lock 담당 |
| `common/` | 인터페이스 `tree_iface.h`, 공통 락 `locks.h` | subtree_lock 담당 |
| `trees/tree_<이름>.cc`, `trees/<이름>/` | 방식별 구현 | **각자 자기 것만** |
| `third_party/arctic/` | vLLM이 쓰는 Arctic Inference 트리 원본 (Apache-2.0). 검사용 `content_digest()` 하나만 추가 | 고치지 않음 |
| `tools/` | `new_tree.py` (내 방식 뼈대), `prep_tokens.py` (데이터를 다시 만들 때만) | subtree_lock 담당 |
| `scripts/` | `build.bat`, `build.sh`, `baseline.bat` / `run_all.bat`, `plot.py` (lazy 담당) | subtree_lock 담당, lazy 담당 |
| `vllm_path/` | vLLM 실제 경로 측정 (7장, 준비 중) | subtree_lock 담당 |
| `premeasure/` | 사전 실험 1~4(문제 크기 확인) 재현용 프로그램. 사용법은 파일 맨 위 주석 | 고치지 않음 |
| `data/` | 측정용 토큰 데이터 2개 (`KTOK` 형식) | 고치지 않음 |
| `results/`, `bin/` | 결과, 실행 파일 (Git에 안 올라감) | |

데이터 파일 맨 앞의 `KTOK`는 파일 형식 표시입니다 (바꾸면 기존 데이터 파일을 못 읽음).
