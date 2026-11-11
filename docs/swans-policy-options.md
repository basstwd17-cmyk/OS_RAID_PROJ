# SWANS 상태 평가 / extra zone / 이동 방식의 독립 설정

`-s`로 읽는 정책 XML에서 세 가지를 독립적으로 선택한다. `WRITE_STDDEV`, `TEC_PWL` 모두 NORMAL / REDIRECT / MIGRATION을 사용한다. ON_DEMAND도 REDIRECT를 허용한다.

```xml
<SWANS_Parameter_Set>
  <SWANS_State_Evaluation>TEC_PWL</SWANS_State_Evaluation>
  <op_zone_migration>true</op_zone_migration>
  <SWANS_Migration_Mode>ON_DEMAND</SWANS_Migration_Mode>
</SWANS_Parameter_Set>
```

위는 세 옵션의 발췌이며, 바로 사용할 전체 설정은 `examples/policy-options/`의 XML 8개에 있다. 실행 예:

```powershell
mingw32-make -j4
.\MQSim.exe -i ssdconfig.xml -w examples/lazy/workload.xml -s examples/policy-options/tec-pwl-extra-on-lazy.xml
```

`examples/lazy/workload.xml`의 trace 경로를 먼저 바꾼다. 결과는 workload XML 옆에 생기므로 실험마다 workload 파일을 별도 폴더에 복사한다. Linux에서는 실행 파일을 `./MQSim`으로 바꾼다.

## 상태 평가

| 옵션 | 지표 | NORMAL | REDIRECT | MIGRATION 시도 |
|---|---|---|---|---|
| `WRITE_STDDEV` | SSD별 누적 논리 쓰기 바이트 / `SWANS_Balance_Unit_Bytes`의 표준편차 μ | μ < 예방 임계값 | 예방 ≤ μ < critical 임계값 | μ ≥ critical 임계값 |
| `TEC_PWL` | TEC / 최대 erase count의 마모도 w, TEC 퍼센트포인트 표준편차 μ | PWL 미충족 및 μ < 예방 임계값 | PWL 미충족 및 μ ≥ 예방 임계값 | max(w) > (avg(w) + 1) / 2 |

PWL의 `1`은 고정 수명 한도다. 관측된 SSD 중 최대 마모도를 다시 대입하지 않는다. TEC에서는 PWL 이동 조건을 먼저 평가하고, 그 조건이 미충족이면 TEC 표준편차로 REDIRECT/NORMAL을 나눈다. REDIRECT의 SSD 선택도 TEC 값으로 한다. 즉 TEC 모드가 모든 상태 경계에 같은 PWL 수식을 사용하는 것은 아니다.

- WRITE_STDDEV 임계값: `SWANS_TH_Precautionary`, `SWANS_TH_Critical`. 누적 값에는 관측한 host 쓰기와 완료된 migration 목적지 쓰기가 들어간다. NAND program 합계가 아니다.
- TEC REDIRECT 임계값: `SWANS_TEC_TH_Precautionary_PctPoint`, 단위 %p. 기존 형식의 `SWANS_TEC_TH_Critical_PctPoint`는 유효한 큰 값으로 둔다. TEC_PWL에서 이 critical 값은 이동 판단에 쓰지 않는다.
- 이동 조건을 만족해도 옮길 존이나 공간이 없으면 기존 동작대로 REDIRECT/NORMAL 평가로 돌아간다. REDIRECT 상태여도 실제 재배치는 원본 존과 목적지 존이 비어 있는 등 기존 admission 조건을 만족해야 일어난다.

`SWANS_State_Evaluation`을 생략하면 `LEGACY`로 기존 `SWANS_Wear_Metric` / `SWANS_Migration_Trigger` 설정을 유지한다. 명시적으로 WRITE_STDDEV 또는 TEC_PWL을 선택하면 두 하위 설정보다 우선하며 XML 순서와 무관하다. 기존 `TEC + IMBALANCE` 조합도 LEGACY에서 계속 사용할 수 있다.

## 공간과 실행 방식

| 옵션 | 동작 |
|---|---|
| `op_zone_migration=false` | 일반 빈 존만 사용. 빈 목적지가 없으면 해당 migration을 시작하지 못함 |
| `op_zone_migration=true` | 실행 시작부터 SSD마다 존 하나를 사용자 용량에서 제외해 예약. 일반 빈 존을 먼저 쓰고, 없을 때만 cold → source 예약 존 → hot 순서로 교환 |
| `SWANS_Migration_Mode=EAGER` | 기존 grab/swap/restore 방식으로 요청 갱신을 기다리지 않고 이동. NAND read/write와 완료 대기는 실제로 발생 |
| `SWANS_Migration_Mode=ON_DEMAND` | 한 hot 존에 대해 전체 페이지 덮어쓰기를 목적지로 보냄. 부분 쓰기는 기존 유효 데이터를 보존. 다음 고정 경계 또는 cold write에서 잔여 복사를 강제 |

extra zone ON은 중간에 용량을 줄이는 기능이 아니다. 예약 공간은 처음부터 확보하고 실제 교환에 사용하는 시점만 늦춘다. cold는 쓰기 빈도가 낮다는 뜻이며 데이터량이 작다는 보장은 없다. 역방향 완료 시간과 요청 대기 시간은 복사량에 따라 달라진다. 비워진 존은 RAID/FTL의 논리 매핑 관점에서 재사용 가능해진 것이며, 모든 NAND 블록이 즉시 erase되었다는 뜻은 아니다.

ON_DEMAND는 한 주기 한 hot 존 작업을 대상으로 한다. 역방향이 필요한 경우 그 시간도 같은 작업/기한에 포함되므로, 역방향 완료 뒤부터 정확히 한 주기를 추가로 주지는 않는다. 기한에는 잔여 복사를 시작하며 NAND I/O가 끝나야 완료된다. 완료가 다음 경계를 넘으면 새 작업은 이후 고정 경계까지 기다린다.

## 비교 실험 조건

8개 예제는 상태 평가 × 예약 존 × 실행 방식의 조합이며, 다른 공통 설정은 같다. 상태 평가는 WRITE_STDDEV / TEC_PWL, 예약 존은 on / off, 이동은 eager / lazy다. 모든 예제의 epoch 세 값과 인기도 기간은 40초, 최대 동시 migration은 1, 인기도는 HALF_EPOCH, stripe = zone = 32768 LBA이다. 임계값은 기능 예시이며 trace별 튜닝이 필요하다.

EAGER는 기존 상태별 평가 간격을 유지한다. ON_DEMAND는 `SWANS_Epoch_Migration`의 고정 시간축을 쓴다. 따라서 공정한 비교에서는 세 epoch 값을 예제처럼 동일하게 둔다. 실행 중 작업은 완료 전 겹쳐 시작하지 않는다. 예약 존 ON/OFF는 사용자 용량도 바꾸므로 같은 SSD 수/geometry에서 ON이 허용하는 공통 LBA 범위의 trace를 사용한다.

ON_DEMAND의 구조적 제한은 유지한다: SSD 2개 이상, 단일 flow, PAGE_LEVEL, device cache OFF, preconditioning OFF, page 정렬된 stripe=zone, concurrency=1, 양수 migration period. TEC나 DYNAMIC_WEAR는 더 이상 필수 조건이 아니다. extra zone 사용에도 단일 flow / PAGE_LEVEL / cache OFF 조건이 적용된다.

## 검증과 결과

결과 XML의 `SWANS_State_Evaluation`, `SWANS_Migration_Mode`, `SWANS_Extra_Zone_Enabled`, `SWANS_Redirect_Enabled`, `SWANS_Mu_Unit`으로 적용된 옵션을 확인한다. NORMAL/REDIRECT/MIGRATION epoch 수와 실제 redirect/migration 횟수는 별도로 본다. 상태 진입 횟수가 실제 이동 성공 횟수를 뜻하지 않는다.

```powershell
g++ -std=c++11 -O0 validation/lazy_zone_test.cpp src/policy/lazy_migration.cpp src/policy/migration_executor.cpp src/policy/zone_directory.cpp src/policy/wear_leveling_policy.cpp src/ssd/User_Request.cpp -o build/lazy_zone_test.exe
.\build\lazy_zone_test.exe
python validation/validate_policy_options.py --output build/policy-options-check
python validation/validate_lazy.py --output build/lazy-regression
```

단위 검사는 두 평가 방식의 임계값 경계, lazy 상태의 REDIRECT 유지, 일반 빈 존 우선/예약 교환/공간 부족, 부분 쓰기 보존과 완료 순서를 검사한다. 조합 검사는 실제 simulator에서 8개 조합을 실행하고, 별도 입력에서 REDIRECT의 실제 수행도 확인한다. 기능 검증을 수명 개선이나 성능 우위의 증거로 해석하지 않는다.

2026-11-11 Windows LLVM-MinGW 검증: 전체 빌드, 확장 단위 검사, 8개 조합의 실제 migration 완료, 추가 8개 조합의 실제 REDIRECT 수행, 새 selector 오류 거부/SSD XML 직접 설정, 기존 lazy/EOL/cold-write 검증, 최초 read 및 GC 회귀 6개가 통과했다. 별도 REDIRECT 입력에서 WRITE_STDDEV는 조합별 8회, TEC_PWL은 EAGER 6회 / ON_DEMAND 1회 실제 재배치가 관측됐다. 횟수 차이는 이 기능 확인 입력의 결과이며 성능 비교 결론이 아니다.
