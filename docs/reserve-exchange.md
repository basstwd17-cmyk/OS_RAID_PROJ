# CASE 1 예약 zone 교환

실행 예:

```sh
./MQSim -i ssdconfig.xml -w swansworkload.xml -s swansconfig.xml
```

SSD 설정을 먼저 읽고 `-s` 파일의 항목으로 SWANS 설정을 덮어쓴 뒤 장치를 만든다.
`-s`를 생략하면 기존 SSD XML 설정과 기본값을 사용한다. 별도 파일의 루트는
`SWANS_Parameter_Set`이다. 알 수 없는 항목, 중복 항목, 잘못된 숫자·불리언,
없는 파일은 실행 전에 오류로 처리한다. 저장소의 기본 `workload.xml`은 여러 flow이므로
예약 교환 예제에는 단일 flow workload를 사용해야 한다. `swansworkload.xml`은 저장소의
SSD geometry에 맞춘 단일 flow 실행 예제다. 균등 쓰기이므로 마모 차이를 강제로 만들지
않으며, CASE 1 재현 워크로드는 `tests/reserve_exchange_smoke.py`에서 생성한다.

## 선택과 교환

`DYNAMIC_WEAR` 트리거는 `(Wear_avg + 1) / 2 < Wear_i`이다. TEC 설정에서는
`Wear_i=TEC/Emax`, BAD_BLOCK_COUNT 설정에서는 불량 블록 기반 정규화값을 사용한다.
`IMBALANCE`는 선택 지표의 표준편차와 critical 임계값을 사용한다.
source zone은 설정한 CUMULATIVE/SSD_POPULARITY/GLOBAL_POPULARITY 방식으로 선택한다.
목적지의 empty zone을 우선 선택한다. 없으면 source SSD의 비어 있고 사용 중이지 않은
예약 zone을 확보한 경우에만 목적지의 coldest zone과 교환한다.
같은 평가에서 hot/cold/reserve zone을 중복 선택하지 않는다. GLOBAL_POPULARITY에서는
예약 공간이 없는 후보를 건너뛰고 다른 SSD의 후보도 확인한다.
인기도가 같으면 실제 복사할 페이지가 적은 zone을 선택한다.

S0의 hot zone H, S3의 cold zone C, S0의 예약 zone R을 선택한 경우:

1. H·C·R을 잠그고 이미 제출된 사용자 I/O가 끝나기를 기다린다.
2. C의 사용 중인 페이지를 R에 복사한다. 쓰기가 완료된 페이지부터 C의 원본 매핑을
   무효화한다. GC 때문에 무효화가 지연되면 실제 완료를 기다린다.
3. C의 논리 주소는 기존 R 위치를 가리키고, 숨겨진 R은 비워진 기존 C 위치를 가리킨다.
4. H의 사용 중인 페이지를 기존 C 위치로 복사한다. 쓰기와 원본 무효화 완료 후
   H는 S3에, 숨겨진 R은 S0의 기존 H 위치에 놓인다.
5. 전체 교환이 끝나면 잠금을 풀고 대기한 사용자 요청을 새 매핑으로 재실행한다.
   S3에 원래 있던 예약 zone은 그대로다.

두 단계 동안 H 또는 C에 접근하는 읽기·쓰기는 보류한다. 교환 도중 쓰기를 조기에
완료 처리하지 않는다. 대기열이 꽉 차면 기존 controller backpressure 경로로 보류한다.
MQSim은 실제 payload를 저장하지 않는 시뮬레이터다. 복사는 read/write I/O와 매핑으로
모델링하며, 단위 테스트에서는 데이터 토큰을 추가해 두 데이터 집합의 보존도 검사한다.

## 용량

`SWANS_Reserve_Zone_Enabled=true`일 때 SSD마다 완전한 논리 zone 하나를 숨긴다.

```
SSD별 export sectors = (floor(backend sectors / zone sectors) - 1) * zone sectors
배열 export sectors = SSD별 export sectors * SSD 수
```

정렬되지 않은 마지막 조각도 export하지 않는다. zone은 페이지와 stripe 크기의 배수여야
하며, SSD마다 완전한 zone이 최소 두 개 있어야 한다. 이 예약 공간은 RAID의 논리 주소
공간이다. NAND의 `Overprovisioning_Ratio`, bad-block 예산, EOL 기준을 바꾸지 않는다.

## 설정 가능한 중단·재개 조건

| 항목 | 기본값 | 의미 |
|---|---:|---|
| `SWANS_Reserve_Zone_Enabled` | false | 라이브러리 기본값. 제공한 별도 XML은 true |
| `SWANS_Zone_Size_LBA` | 0 | 0이면 stripe × `Zone_Stripe_Multiplier` |
| `SWANS_Zone_Popularity_Mode` | HALF_DECAY | HALF_DECAY 또는 LAST_EPOCH |
| `SWANS_Zone_Popularity_Period` | 40000000000 | LAST_EPOCH에서만 사용(ns). HALF_DECAY는 시간 대신 교환 전체 완료 후 전체 zone 인기도를 1/2로 줄임 |
| `SWANS_Migration_Slice_Time` | 0 | 한 구간의 경과 시간(ns). 0이면 제한 없음 |
| `SWANS_Migration_Slice_Sectors` | 0 | 한 구간에서 목적지 쓰기가 완료된 sector 수 |
| `SWANS_Migration_Pause_Duration` | 1000000 | 일시정지 후 재개까지 ns |
| `SWANS_Migration_Pause_On_Read` | false | 잠긴 zone과 무관한 host read 도착 시 일시정지 |
| `SWANS_Migration_Working_Queue_Limit` | 64 | 작업별 보류 요청 수 |
| `SWANS_Max_Concurrent_Migrations` | 1 | 동시에 선택 가능한 작업 수. 예약 zone 중복 사용 금지 |

기본 종료 조건은 **전체 유효 데이터 이동과 원본 무효화 완료**다. 시간·이동량·읽기 조건은
부분 완료로 선언하는 조건이 아니라 **일시정지 조건**이다. 진행 중인 복사 I/O는 완료시키고
다음 I/O 제출 전에 멈춘다. 따라서 설정한 시간이나 sector 예산은 I/O 하나만큼 초과할 수 있다.
정지 중에는 매핑·버퍼·잠금을 유지하고 같은 단계에서 이어간다. 재개마다 최소 I/O 하나는
진행해 읽기가 계속 들어와도 영구적으로 정지하지 않게 한다. 잠긴 zone의 읽기는 교환 완료를
기다려야 하므로 일시정지 트리거에서 제외한다. 마모 격차 해소 조건은 아직 구현하지 않았다.

EOL은 기존 시뮬레이터의 즉시 종료 조건을 유지한다. EOL 전에 교환이 끝나지 않으면
재사용 가능한 예약 공간으로 선언하지 않는다. 실행 중 교환의 결과 XML 상태는 `ACTIVE`로
남을 수 있으며, 명시적 abort 처리 시에는 `ABORTED_EOL`이다. 복구·rollback은 구현하지 않았다.

## 현재 지원 범위와 결과 확인

- 예약 교환은 단일 I/O flow, FLASH, PAGE_LEVEL mapping,
  `Device_Level_Data_Caching_Mode=TURNED_OFF`를 요구한다. 여러 flow의 backend 주소 영역
  재배치와 cache 무효화는 별도 구현이 필요하므로 해당 조합은 실행 전에 거부한다.
- 예약 모드에서는 migration unit이 페이지다. 페이지 내 일부 sector만 유효해도 페이지
  전체를 복사한다. 기존 `*_Blocks` 통계는 migration unit 개수이며 실제 NAND erase block
  수와 구별해야 한다. `SWANS_Copy_Unit_LBA`로 단위를 확인한다.
- synthetic workload의 지원되는 preconditioning 데이터는 FTL에서 수집한다.
  기존 FTL은 trace workload preconditioning을 지원하지 않는다.
- 지원 지표는 `TEC`, `BAD_BLOCK_COUNT`, `WRITE_BYTES`다.
  `TAIL_ERASE_COUNT` 문자열은 지원하지 않으므로 TEC 설정에는 `TEC`를 사용한다.
- 결과 XML: `SWANS_User_Zone_Count`, `SWANS_Reserve_Zone_Count`, `SWANS_Exported_Sectors`,
  `SWANS_Exchange_Cold_Stages_Completed`, 세 종류의 `*_Pauses` 카운터를 확인한다.
  교환 기록은 `Mode=op_zone_migration`, `Reserve_Zone`, `Cold_Copy_Sectors`를 포함한다.

`make test`는 마모 트리거·인기도·예약 교환 단위 테스트, 실제 NAND 교환/일시정지,
사전 채움 등록, 별도 XML 파싱과 기존 CLI 호환성을 검사한다.

## 2026-10-28 예약 교환 정책 연결 수정

e6fb9df에서는 executor에 예약 교환 코드가 있으나 정책이 empty destination만 선택해
`reserve_exchange.cpp:43`이 실패했다. `wear_leveling_policy.cpp`에서 빈 목적지를
찾지 못하면 source reserve와 cold destination을 선택하고 `MigrationOp.Reserve_zone`을
전달하도록 수정했다. executor의 복사·매핑 로직은 변경하지 않았다.

Windows MSVC 전체 빌드 및 4개 단위 테스트 통과. 정책 테스트는 3개 지표 × 2개 트리거 ×
3개 zone 선택 방식, 예약 공간 중복 방지, 사용 중인 reserve, reserve 미설정 조건을 확인한다.
기존 executor 테스트는 데이터 토큰 보존, 예약 공간 순환, pause/resume, I/O 재실행,
discard 재시도와 EOL abort를 확인한다.

실제 NAND 경로의 reserve smoke 결과: 기본 모드 교환 완료 2회, 일시정지 모드 1회,
TEC 동적 모드 223회. 사전 채움 등록과 외부 XML 설정 검증도 통과했다.
기존 advanced TEC EOL/트레이스 풀 및 wear-trigger smoke도 통과했다.
이 결과는 소규모 기능 검증이며 대규모 워크로드의 수명 개선이나 성능 보장은 아니다.
Linux 빌드는 이번에 실행하지 않았다.
