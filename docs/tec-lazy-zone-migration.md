# TEC / PWL 기반 RAID0 lazy zone migration

이 브랜치는 `experiment/tec-swans-src-only`의 `aba1b9d`를 기준으로 교수님이 제안한 RAID0 정책의 남은 경로를 구현한다. `professor/greedy-circular-wrap-error`의 별도 오류 수정 커밋은 합치지 않았다. 기본 `ssdconfig.xml`, `workload.xml`과 EAGER 경로는 유지하며 새 정책은 `SWANS_Migration_Mode=ON_DEMAND`로 선택한다.

## 요구사항과 구현 범위

| 요구사항 | 기존 기준 브랜치 | 이 브랜치 |
|---|---|---|
| TEC와 PWL 형태 트리거 | 구현됨 | 재사용: `Wear_i > (Wear_avg + 1) / 2` |
| zone 기반 RAID0 striping | stripe/zone 크기가 달라도 허용 | ON_DEMAND에서 stripe = zone을 검증 |
| 고정 migration 주기, 주기당 한 존 | 상태에 따른 평가 주기, 설정 가능한 동시 개수 | 고정 시간축, 한 hot 존 작업만 허용, redirect 비활성화 |
| 가장 old SSD의 hottest zone -> youngest SSD의 empty zone | 구현됨 | 재사용, heat 동점에서는 유효 데이터량/복사량이 작은 후보 우선 |
| 빈 zone이 없으면 SSD별 숨겨진 OP zone | 구현됨 | 재사용: SSD당 사용자 zone 하나 제외 |
| cold -> old SSD의 OP 역방향 | 일괄 복사 | 유효 sector만 즉시 복사하고 무효화 완료를 기다림 |
| hot -> young SSD 순방향 | 전체를 grab/swap/restore | 쓰기 도착 시 page 단위 on-demand 이동 |
| 전체 page 덮어쓰기 | 복사 후 처리 | 이전 page read/program 없이 실제 host write를 목적지에 offload |
| 부분 page 쓰기 | 전체 task 종료 후 처리 | 기존 유효 sector만 보존 복사한 후 host write |
| migration 중 읽기 | task 종료까지 대기 | 미이동 page는 원본, 이동한 page는 목적지; 복사/갱신 중에는 보수적으로 대기 |
| cold 존에 새 쓰기 도착 | task 종료 후 replay | 남은 hot 복사를 강제 완료한 뒤 쓰기 replay |
| 다음 주기에서 이전 작업 완료 | 별도 deadline 없음 | 다음 tick에서 잔여 복사 강제 시작; 완료 전 새 migration 금지 |
| 인기도 | migration 최종 완료 시 HALF_DECAY, 또는 LAST_EPOCH | HALF_EPOCH 추가: 고정 주기별 반감 + 현재 주기 write sectors |
| 페이지 이동 추적 | grab/dirty bitmap | 현재 한 존의 page bitmap, copy 상태와 host 완료 추적 |

트리거 수식의 `Wear_max`는 관측된 SSD 중 최대값이 아니라 고정 수명 한도다. TEC를 `E_tail/E_max`로 정규화하므로 `Wear_max=1`이다. 원시 erase 단위를 선택하는 별도 대안은 이번에 추가하지 않았다. 기존 TEC 계산은 retired block을 포함한 전체 erase count에서 사용자 블록 수 다음 OP 경계 항목을 고르는 방식이며 제공 원고의 식 (6), (7)을 따른다.

## 논문에서 가져온 부분과 적용상의 차이

- 제공된 Ilhoon Shin의 원고 **Tail Erase Count: A Wear Metric for Lifetime-Aware Management in Heterogeneous SSD Clusters**: OP 경계 erase count와 TEC 정규화 정의를 적용한다. 해당 원고 자체가 이 RAID0 lazy 정책을 완성하거나 그 수명 효과를 검증한 것은 아니다.
- [Chameleon: An Adaptive Wear Balancer for Flash Clusters, IPDPS 2018](https://people.cs.vt.edu/~butta/docs/ipdps2018-chameleon.pdf), III-B/III-C: 새 write/update에 맞춰 목적지를 바꾸고 중간 상태의 최신 위치로 읽기를 보내는 아이디어, 주기별 exponential heat를 차용했다. REP/EC 전환이나 논문 전체를 재현하지 않는다. 교수님 요구에 따라 RAID0의 zone을 대상으로 page별로 적용한다.
- 다음 주기에 강제 drain하는 규칙과 cold write 시 전체 작업을 끝내는 규칙은 교수님이 제안한 이 실험 정책의 규칙이다. Chameleon의 epoch-log compaction과 동일한 알고리즘이라고 주장하지 않는다.

## 실행

Windows MinGW/LLVM-MinGW에서 저장소 루트 기준:

```powershell
mingw32-make -j4
.\MQSim.exe -i ssdconfig.xml -w examples/lazy/workload.xml -s examples/lazy/swans.xml
```

Linux에서는 같은 인수로 `make -j4` 후 `./MQSim`을 사용한다. Linux는 이번에 실행 검증하지 않았다. 실행 전 workload의 `File_Path`를 실제 변환된 trace로 수정해야 하며, trace 파일은 포함하지 않는다. 상대 경로는 저장소 루트에서 실행하는 것을 기준으로 한다. Root workload의 다중 synthetic flow 대신 제공한 단일 flow 예제를 사용한다.

기본 overlay는 16 MiB stripe = zone, TEC/DYNAMIC_WEAR, 40초 고정 주기, 한 존, OP zone 사용, HALF_EPOCH를 선택한다. root SSD의 PE25는 가속 실험값이며 상용 SSD 내구성을 뜻하지 않는다. 다수의 짧은 trace는 마모가 트리거까지 진행되지 않으므로 migration이 0회일 수 있다.

ON_DEMAND는 SSD 2개 이상, 단일 flow, PAGE_LEVEL, preconditioning OFF, device cache OFF, page 정렬된 동일 stripe/zone, concurrency=1, 양수 고정 주기, TEC/DYNAMIC_WEAR/SSD_POPULARITY를 요구한다. 이번 경로의 초기 데이터는 trace의 실제 read/write로 등록한다. 잘못된 조합은 시작 전에 오류로 종료한다. 기본 XML과 EAGER 모드를 바꾸지 않았으므로 기존 실행은 기존 경로를 쓴다.

설정:

- `SWANS_Migration_Mode`: `EAGER`(기존 기본값) / `ON_DEMAND`.
- `SWANS_Epoch_Migration`: ON_DEMAND 고정 주기(ns). NORMAL/REDIRECT/MIGRATION 상태별 가변 주기를 쓰지 않는다.
- `SWANS_Zone_Popularity_Mode=HALF_EPOCH`: 주기 경계에서 기존 heat를 반감하고 현재 write sectors를 누적한다. migration 완료 횟수와 무관하다. `LAST_EPOCH`도 기존처럼 사용할 수 있다. `HALF_DECAY`는 호환용 기존 의미를 유지한다.
- `op_zone_migration`: `true`이면 빈 목적지가 없을 때 OP 존 교환을 허용한다. `false`이면 일반 빈 목적지만 사용한다.

## 데이터 이동과 직렬화

역방향 cold -> source OP가 실제 NAND read/write 및 source discard 완료를 기다린 후에만 비워진 cold 위치를 hot 목적지로 사용한다. 이전 hot 물리 위치는 최종 commit 후 source SSD의 새 OP 역할이 된다.

순방향에서 전체 page를 덮어쓰는 실제 host write는 직접 목적지로 가며, 원본은 host 완료 뒤에 무효화한다. 부분 쓰기는 그 page의 기존 유효 sector만 보존한다. 아직 없는 sector를 읽어 MQSim의 read 초기화로 가짜 데이터를 만드는 것을 피한다. 한 번에 copy I/O 하나, hot host write 하나를 진행하며 요청이 여러 page를 덮는 경우도 모두 준비한 뒤 replay한다. 일반 빈 목적지에 속한 논리 존을 읽는 요청 역시 commit 전에는 대기시켜 이동 중 hot 데이터를 잘못 읽지 않게 한다.

다음 고정 tick에서 잔여 page 복사를 강제한다. NAND I/O에는 시간이 필요하므로 tick 순간에 완료를 보장하지는 않는다. 완료가 늦으면 놓친 tick에서는 새 작업을 시작하지 않고 다음 고정 tick을 기다린다. cold write 역시 그 요청을 막고 같은 drain 경로를 시작한다. EOL에서는 진행 중 작업을 `ABORTED_EOL`로 기록하고 부분 위치를 재사용 가능한 빈 존으로 공개하지 않는다.

## 결과에서 확인할 값

XML의 `SWANS_Migration_Mode`, `SWANS_Migration_Fixed_Period`, migration record의 `Execution_Mode`, `Start_Time`, `Status`를 확인한다.

- `SWANS_Lazy_Offloaded_Pages`: 이전 데이터 복사 없이 전체 page 쓰기를 목적지로 보낸 수.
- `SWANS_Lazy_Copied_Pages`: 보존 복사를 완료한 hot page 수(역방향 cold는 별도 stage 통계).
- `SWANS_Lazy_Deadline_Drains`: 다음 tick에서 강제 drain한 작업 수.
- `SWANS_Lazy_Cold_Write_Drains`: cold write가 강제 drain을 일으킨 수.
- `SWANS_Lazy_Remaining_Pages`: 아직 최종 정리가 남은 page 수.
- 기존 `SWANS_Exchange_Cold_Stages_Completed`, source discard, backpressure 및 host/NAND 통계도 유지된다.

host write, migration write, NAND program은 서로 다른 통계다. 새 예제의 실행 결과는 workload XML 옆에 기록되므로 실험별 XML 경로를 분리한다.

## 재현 가능한 검증

단위 테스트는 mock sector payload를 사용해 부분 쓰기 보존, 전체 덮어쓰기, 최신 위치 읽기, cold 교환/예약 역할 회전, 무효화 재시도, page 경계 요청, 복사 도중 deadline, backpressure와 EOL 잠금을 확인한다.

```powershell
g++ -std=c++11 -O0 validation/lazy_zone_test.cpp src/policy/lazy_migration.cpp src/policy/migration_executor.cpp src/policy/zone_directory.cpp src/policy/wear_leveling_policy.cpp src/ssd/User_Request.cpp -o build/lazy_zone_test.exe
.\build\lazy_zone_test.exe
python validation/validate_lazy.py --output build/my-lazy-validation
```

출력 폴더는 기존 결과를 덮지 않도록 새 이름을 사용한다. Python 검사는 작은 실제 NAND 설정에서 TEC/PWL 트리거, 고정 tick, 단일 존, 일반 이동, 예약 교환, 의도적으로 주입한 cold write 충돌, EOL, 잘못된 설정 거부를 확인한다. EAGER 대조도 포함한다.

2026-11-09 Windows LLVM-MinGW 검증:

- 전체 Makefile 빌드, 새 lazy 단위 검사 통과.
- 일반 빈 목적지 및 OP 교환에서 각각 migration 4회 완료. 원본 복사를 생략한 page는 각각 6개/7개, deadline drain은 각각 4회. 이는 작은 입력의 기능 확인이며 성능 우열을 뜻하지 않는다.
- cold write 주입 시 강제 drain 1회, 첫 교환을 다음 tick 전에 완료.
- 기존 read/GC 회귀 6개, 기존 정책 단위 4종, 기존 policy smoke 4종 통과.
- mock 값 보존 검사는 controller 정책의 위치/상태 검사다. MQSim 전체가 실제 사용자 payload를 byte-by-byte 저장해 무결성을 검증하는 것은 아니다. 대규모 실제 trace, 여러 flow/동시 migration, Linux 및 수명 개선 효과는 별도 검증 범위다.
