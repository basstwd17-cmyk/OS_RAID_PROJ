# Advanced SWANS 구현 — 2026-10-28

> 후속 수정: 예약 교환 선택 경로를 연결해 아래의 과거 `reserve_exchange.cpp:43`
> 실패를 해결했다. Windows 전체 빌드, 4개 단위 테스트 및 TEC 예약 교환 NAND 검증이
> 통과했다. 변경과 검증 범위는 [reserve-exchange.md](reserve-exchange.md)의 후속 수정 절을 참고한다.

작업 경로: `D:/D_Drive_Codex/OS_RAID_PROJ-tail-erase`.
기존 미커밋 변경을 유지한 상태에 추가했다. `MQsimDT`와 `OS_RAID_PROJ` 디렉터리는 수정하지 않았다.

## 구현

- **TEC**: 전체 물리 블록(퇴역 블록 포함)의 erase count에서 `N - floor(N × OP)` 인덱스 선택. `std::nth_element`를 사용하며 실제 블록 배열은 변경하지 않는다. 최초 조회/erase 이후에만 계산한다.
- erase 완료 직후, retirement·EOL 관측 전에 캐시 무효화. 현재 코드의 초기 erase count는 0이다. 향후 초기 마모 주입 시 `Invalidate_wear_snapshot()`도 호출해야 한다.
- SSD 선택은 `TEC / Emax`, 불균형은 `100 × TEC / Emax`의 모집단 표준편차(%p). redirect도 매 요청의 최신 TEC 캐시를 사용한다. 동률은 SSD ID 순이며 모두 같은 마모도면 이동하지 않는다.
- TEC의 사용자 블록 경계와 장치 전체 논리 용량의 블록 환산값·차이를 결과 XML에 기록한다. workload 접근 범위나 reserve zone 제외 후 용량을 TEC 경계로 사용하지 않는다.
- **트레이스 풀**: 폴더의 확장자가 일치하는 일반 파일을 정렬하고, seed를 지정한 MT19937로 균등·복원 추출한다. 파일을 끝까지 읽은 뒤 다음 파일을 선택한다. 한 시뮬레이터 안에서 NAND 상태를 유지하며 첫 SSD EOL까지 반복한다.
- 각 파일의 첫 timestamp를 0으로 정규화하고 이전 파일 마지막 요청 시각에 `Trace_Inter_File_Gap_NS`를 더해 연결한다. ns/us/ps를 ns로 변환한다. 파일 내용은 스트리밍하며 시작 전에 모든 파일의 형식·시간 순서·빈 파일·쓰기 존재 여부를 검사한다.
- **인기도 (후속 변경)**: HALF_DECAY에서는 호스트 쓰기 섹터를 즉시 P에 더하고, 마이그레이션 1건이 정상 완료될 때 전체 zone에 `P=P/2`를 적용한다. 40초 주기 감쇠는 하지 않는다. 예약 교환은 두 단계가 모두 완료된 뒤 한 번 적용하며, 일시정지·EOL 중단 때는 적용하지 않는다. migration 복사 자체는 인기도를 늘리지 않고 실제 누적 쓰기 통계도 감쇠하지 않는다. 명시적 LAST_EPOCH 모드는 기존 시간 창 방식을 유지한다.
- **zone 선택**: `CUMULATIVE`(선택 SSD의 누적 쓰기 섹터 최댓값), `SSD_POPULARITY`(선택 SSD의 인기도 최댓값), `GLOBAL_POPULARITY`(전체 SSD의 인기도 최댓값). 비어 있거나 이동 중이거나 이번 평가에서 이미 선택한 zone은 제외한다. 점수 동률은 zone ID 순이다.
- 전체 인기도 모드에서도 선택 zone의 소유 SSD가 해당 migration 조건을 만족해야 한다. cold SSD 소유 zone이나 가변 TH 미달 source는 제외하고 다음 후보를 검사한다. 목적지는 선택 지표가 가장 낮은 SSD의 기존 empty zone이다.

## 정책 설정

| 설정 | 값 / 의미 |
|---|---|
| `SWANS_Wear_Metric` | `WRITE_BYTES`(새 기본값), `TEC`, 기존 호환용 `BAD_BLOCK_COUNT` |
| `SWANS_Migration_Trigger` | `IMBALANCE`(새 기본값), `DYNAMIC_WEAR` |
| `SWANS_TEC_TH_Precautionary_PctPoint` | TEC 모드에서 반드시 명시, 0 이상 |
| `SWANS_TEC_TH_Critical_PctPoint` | TEC 모드에서 반드시 명시, precautionary보다 커야 함 |
| `SWANS_Zone_Selection` | `CUMULATIVE`, `SSD_POPULARITY`(기본), `GLOBAL_POPULARITY` |
| `SWANS_Zone_Popularity_Period` | LAST_EPOCH에서만 사용, 기본 `40000000000` ns. HALF_DECAY에서는 무시 |

`IMBALANCE`: Mu가 precautionary 미만이면 NORMAL, critical 미만이면 REDIRECT, critical 이상이면 MIGRATION을 시도한다. 이동 가능한 source/destination이 없으면 placement 상태로 복귀한다.

`DYNAMIC_WEAR`: 기존 `(Wear_avg + 1) / 2 < Wear_i`를 엄격한 부등호로 유지한다. 최대값 1은 수명 한계이며 현재 SSD 중 최댓값이 아니다. TEC 모드에서는 `Wear_i = TEC/Emax`를 공급한다. critical 값은 이 모드의 migration 판정에는 사용하지 않는다. 기존 `ssdconfig.xml`·`swansconfig.xml`에는 `DYNAMIC_WEAR`를 명시해 기존 BAD_BLOCK_COUNT 실행 의미를 보존했다.

TEC 계산을 관측만 하려면 `WRITE_BYTES`를 유지하고 telemetry를 활성화한다. TEC 오류는 valid/error 필드로 표시하고, TEC 정책을 선택한 경우 잘못된 snapshot은 실행 오류로 처리한다.

## 실행

Windows 전체 빌드:

```powershell
cd D:/D_Drive_Codex/OS_RAID_PROJ-tail-erase
./tests/build_windows.ps1
```

`examples/advanced-swans/trace-pool-workload.xml`의 `Trace_Directory`를 실제 폴더로 변경한다. 경로는 실행 작업 디렉터리 기준이며 절대경로를 권장한다. `.txt` 파일이면 `Trace_Extension`도 변경한다. 하위 폴더는 탐색하지 않는다. `File_Path`는 비워두고 `Relay_Count=0`, 실행 비율 100을 사용한다.

SSD 설정은 `Enabled_Preconditioning=false`, `Bad_Block_Retirement_Enabled=true`로 지정한다. 예제 workload의 채널·칩·die·plane ID는 SSD 설정에 맞춰 조정한다.

```powershell
./build/windows/MQSim.exe -i ssdconfig.xml -w examples/advanced-swans/trace-pool-workload.xml -s examples/advanced-swans/tec-dynamic.xml
```

고정 임계값 비교는 `-s examples/advanced-swans/tec-imbalance.xml`을 사용한다. 예제의 5·10%p는 기능 확인용 값이며 최적화된 연구 임계값이 아니다. 결과는 workload 경로의 `_scenario_1.xml`과 `.telemetry.csv`에 저장된다. 다른 실험은 workload 복사본 또는 별도 실행 디렉터리를 사용한다.

`TRACE_POOL` 실행 로그에 파일 선택 순서·시각을 남기고 결과 XML의 `TracePool`에는 seed·파일 목록·현재 파일·재생 회차를 기록한다. 동일 재현에는 파일 내용·목록·seed·SSD 설정이 모두 같아야 한다.

## 검증

- MSVC 전체 빌드 성공. 이 환경의 WSL은 `HCS_E_HYPERV_NOT_INSTALLED`로 실행되지 않아 Linux 빌드는 실행하지 않았다.
- `advanced_swans.cpp`: TEC 정렬 기준값·순위 경계·퇴역 tail·무효 설정, 가변 TH 경계, TEC의 write-byte 우선순위 대체, 세 zone 선택 방식, 고정 시간 감쇠, deterministic pool 추출·파일 시간 연결·잘못된 레코드 거부.
- 기존 `wear_trigger.cpp`, `zone_popularity.cpp` 통과.
- 기존 `wear_trigger_smoke.py` 통과: BAD_BLOCK_COUNT 실제 migration, retirement 비활성 시 wear migration 방지, 미지원 지표 거부. 기본값 변경에 맞춰 legacy BAD_BLOCK_COUNT 테스트는 지표를 명시한다.
- `advanced_swans_smoke.py`: 4 SSD × 128 blocks, 8 pages/block, Emax 3, OP 0.3, EOL 잔여 OP 0.05. 동적 TEC·고정 TEC·WRITE_BYTES 관측·SWANS OFF를 실제 NAND 경로로 EOL까지 실행한다. 두 동적 TEC 실행은 SSD XML과 `-s` overlay를 각각 사용하며 seed·파일 순서·완료 쓰기량이 일치한다.
- 종료 snapshot의 TEC를 별도 erase 히스토그램을 정렬한 값과 대조한다. TEC·Emax·마모도·OP·사용자 블록 경계·XML/CSV를 검증한다. 이 실험은 기능 검증이며 수명 개선을 입증하는 비교 실험은 아니다.

## 기존 문제와 지원 범위

- `reserve_exchange.cpp:43` 실패는 변경 전 백업에서도 동일하게 재현된다. 본 변경의 migration은 명세의 empty destination 조건을 유지한다. reserve-assisted exchange 복구는 포함하지 않았다. 예제에서는 reserve zone을 비활성화한다. 따라서 기존 전체 `make test` / Windows unit suite는 이 기존 테스트에서 실패한다.
- trace preconditioning 검사에서 기존 FTL의 `Unhandled address distribution type` 오류가 발생했다. 트레이스 풀과 preconditioning 조합은 시작 전에 명시적으로 거부한다. 기존 `Get_statistics`·FTL preconditioning·mapping 구현은 이 작업에서 수정하지 않았다.
- EOL은 이제 `End_of_Life_Mode=OP_RATIO`(기존 기본값) 또는 `TEC`로 선택한다. OP 기준과 TEC 100%는 서로 다른 종료 시점이 될 수 있다. [TEC 구현 보고서](TEC-implementation-report-ko.md)에 설정과 검증 결과를 정리했다. 동일 구성 SSD에서 검증했으며 이종 SSD의 설정·용량 지원 확장은 하지 않았다.
- 02 Mapping·05 Preconditioning Notion 페이지는 읽거나 참고하지 않았다.
- 작업 전 사본: `D:/D_Drive_Codex/artifacts/advanced-swans-20261028/before.zip`. Git에는 기존 미커밋 작업과 함께 남겨두었고 commit/push는 하지 않았다.

명세: [TEC](https://app.notion.com/p/TEC-4053d40307ff80b09e4ff2726dc83cd4), [인기도](https://app.notion.com/p/4063d40307ff80eaaef8f10b066c5e29), [가변 TH](https://app.notion.com/p/Wear_avg-t-Wear_max-2-Weari-t-4053d40307ff80058f45ecea60f59ed7).
