@echo off
chcp 65001 >nul
rem 기준 측정 (subtree_lock 담당, 약 10~15분). 먼저 scripts\build.bat
rem 입력: data\tokens_code.bin, data\tokens_chat.bin   결과: results\baseline_*.txt (읽기용), baseline_*.csv (그래프용)
cd /d "%~dp0.."
set H=bin\tree_bench.exe
set R=results
if not exist %R% mkdir %R%

rem 1) 갱신 비용 나누기 (개인 / 전역), 1스레드 + 서브트리 쏠림 분석
for %%D in (code chat) do for %%B in (64 256) do for %%N in (1000 10000) do %H% data\tokens_%%D.bin --mode=serial --sched=serial,phase --threads=1 --b=%%B --n_cached=%%N --profile=1 --csv=%R%\baseline_split.csv >> %R%\baseline_split.txt

rem 2) 삭제(remove) 비용: 스텝마다 지우는 응답 0 / 1 / 2개
for %%D in (code chat) do for %%E in (0 1 2) do %H% data\tokens_%%D.bin --mode=serial --sched=phase --threads=1 --b=256 --n_cached=10000 --evict=%%E --csv=%R%\baseline_evict.csv >> %R%\baseline_evict.txt

rem 3) 개인 트리 + 추측 병렬, 전역 갱신은 1스레드 (개인 트리 병렬화 확인)
for %%D in (code chat) do %H% data\tokens_%%D.bin --mode=serial --sched=phase --threads=1,2,4,6,8,12 --b=256 --n_cached=10000 --csv=%R%\baseline_localpar.csv >> %R%\baseline_localpar.txt
for %%D in (code chat) do %H% data\tokens_%%D.bin --mode=serial --sched=phase --threads=4,12 --local=serial --b=256 --n_cached=10000 --csv=%R%\baseline_localpar.csv >> %R%\baseline_localpar.txt

rem 4) 전역 락 기준선: 여러 스레드가 락 하나로 전역 트리 갱신
for %%D in (code chat) do %H% data\tokens_%%D.bin --mode=global_lock --sched=phase,mixed --threads=1,2,4,6,12 --b=256 --n_cached=10000 --csv=%R%\baseline_glock.csv >> %R%\baseline_glock.txt

rem 5) 검사 (작게)
%H% data\tokens_code.bin --mode=serial,global_lock --sched=phase,mixed --threads=4 --b=64 --n_cached=1000 --repeat=1 --check=1 >> %R%\baseline_check.txt
echo done
