# TEC 기반 마모도 계산 및 EOL 선택 구현 보고서

작성일: 2026-10-28

## 1. TEC의 기본 개념

TEC(Tail Erase Count)는 SSD의 전체 NAND 블록을 erase count 오름차순으로 나열했을 때, 사용자 영역 다음의 첫 블록이 가진 erase count다. 평균이나 최댓값 대신 OP 시작 경계의 마모를 관측한다.

논문의 사용자 블록 수를 N_user, 최대 허용 erase count를 E_max라고 하면 다음과 같다. 순위는 1부터 센다.

$$B_{ref}=B_{(N_{user}+1)}$$

$$Wear_{TEC}(t)=\frac{E_{tail}(t)}{E_{max}}\times100$$

예를 들어 전체 100블록 중 사용자 영역이 80블록이면 81번째 블록의 erase count를 사용한다. 그 값이 40이고 E_max가 100이면 TEC 마모도는 40%다.

근거: [TEC 원문](<C:/CODEX/Obsidian/LiamObsidian/MQSim SSD 연구/References/TEC 원문.pdf>) PDF 3쪽(인쇄 329쪽), 식 (6)·(7). [옵시디언 정리](<C:/CODEX/Obsidian/LiamObsidian/MQSim SSD 연구/References/TEC - Tail Erase Count 논문 정리.md>)와도 대조했다. 위 식은 논문의 정의이고, 아래 반올림·퇴역 처리·종료 기준 선택은 MQSim의 구현 사항이다.

## 2. 코드에서 TEC를 계산하는 방법

1. `Flash_Block_Manager_Base::Get_wear_snapshot()`이 channel/chip/die/plane/block 전체를 순회해 erase count를 수집한다. 퇴역 블록도 포함한다.
2. 초기 OP 블록 수를 `floor(N × OP 비율)`로 계산하고, `N_user = N - 초기 OP 블록 수`로 경계를 정한다.
3. `Calculate_TEC()`가 복사된 배열에 `std::nth_element`를 적용해 `counts[N_user]`를 구한다. 0부터 세는 배열 인덱스 N_user는 논문의 N_user+1번째 순위와 같다. 원본 블록 위치는 바꾸지 않는다.
4. 해당 erase count를 E_max로 나누고 100을 곱해 마모율을 만든다. SWANS 정책에는 이를 다시 100으로 나눈 0~1 비율을 전달한다.
5. erase 완료 직후 기존 계산 결과를 무효화한다. 이후 조회에서 재계산하고, 추가 erase가 없으면 저장된 값을 재사용한다.

OP가 0블록이거나 전체 블록 수 이상인 경우, 또는 E_max가 0인 경우에는 유효한 TEC를 계산할 수 없으므로 오류로 처리한다.

**사용자 영역 경계에 대한 가정:** 논문은 사용자에게 보이는 블록 수를 정의하지만 부분 블록의 반올림 방법은 지정하지 않는다. 현재 코드는 초기 OP 예산으로 정수화한다. 예를 들어 검증 구성에서는 실제 논리 용량이 89.59375블록이고 TEC 경계는 90블록이다. 이 차이는 XML에 기록한다. RAID 예약 zone의 유무나 워크로드 접근 범위로 TEC 경계를 변경하지 않는다.

## 3. OP 기준과 TEC 기준 EOL 분리

SSD 설정의 `End_of_Life_Mode`로 종료 기준을 선택한다. 두 조건을 OR로 결합하지 않으며, 선택한 기준만 EOL을 발생시킨다. 모드를 생략하면 기존 `OP_RATIO`가 적용된다.

| 설정 | 종료 조건 | 임계값 단위 |
|---|---|---|
| OP_RATIO | 잔여 OP 비율 ≤ End_of_Life_Threshold | 비율: 0.05 = 전체 물리 블록 대비 5% |
| TEC | TEC 마모율 ≥ End_of_Life_TEC_Threshold_Percent | 백분율: 100 = 마모율 100% |

OP 모드의 잔여 OP 계산은 다음과 같다.

$$RemainingOP=\frac{\max(B_{OP}-B_{bad},0)}{N}$$

기존 OP 5% 기준 실행:

```xml
<End_of_Life_Mode>OP_RATIO</End_of_Life_Mode>
<End_of_Life_Threshold>0.05</End_of_Life_Threshold>
```

TEC 100% 기준 실행:

```xml
<End_of_Life_Mode>TEC</End_of_Life_Mode>
<End_of_Life_TEC_Threshold_Percent>100</End_of_Life_TEC_Threshold_Percent>
```

위 항목은 `-i`로 지정하는 SSD 설정 XML의 `Device_Parameter_Set` 안에 넣는다. TEC 임계값은 `0 < 값 ≤ 100` 범위의 유한한 수를 허용하며 기본값은 100이다. OP 임계값은 `0 ≤ 값 < 초기 OP 비율`이어야 한다.

TEC 모드는 블록이 퇴역하지 않은 erase에서도 검사한다. 따라서 임계값을 30% 등으로 지정하면 불량 블록 발생 전에도 종료할 수 있다. 마모율은 erase count 단위로 증가하므로 정확히 지정값이 아니라 처음으로 지정값 이상이 된 시점에 종료한다. OP 모드의 EOL 반복에는 퇴역 기능이 필요하고, TEC 모드는 퇴역 기능을 끈 상태에서도 사용할 수 있다.

첫 SSD가 선택된 조건에 도달하면 기존 lifecycle monitor가 시뮬레이션 전체를 중단한다. 결과 snapshot XML에는 EOL 모드, 활성 임계값과 단위를 기록한다.

**마모 정책과 종료 기준은 독립적이다.** `SWANS_Wear_Metric=TEC`는 migration 판단에 사용할 지표를 정하고, `End_of_Life_Mode=TEC`는 실행 종료 조건을 정한다. 한쪽 설정이 다른 쪽을 자동으로 바꾸지 않는다. `BAD_BLOCK_COUNT` migration 지표의 정규화는 기존 OP 예산 기준이며, TEC EOL 선택으로 자동 변경되지 않는다.

## 4. 주요 코드 위치

| 파일 | 담당 기능 |
|---|---|
| `src/ssd/TEC_Wear.h` | TEC 순위값 및 마모율 계산 |
| `src/ssd/Flash_Block_Manager_Base.cpp` | 블록 수집·캐시, EOL 설정 검사 및 모드별 판정 |
| `src/ssd/Flash_Block_Manager.cpp` | erase 후 캐시 무효화, 퇴역 여부와 무관한 EOL 검사 |
| `src/exec/Device_Parameter_Set.h/.cpp` | XML 설정 기본값·읽기·출력 |
| `src/exec/SSD_Device.cpp` | 선택한 EOL 설정을 블록 관리자에 전달 |
| `src/exec/RAID_Telemetry.cpp` | TEC·논리 용량 경계·EOL 기준 출력 |
| `tests/eol_modes_smoke.py` | 실제 NAND 경로의 OP/TEC 독립 종료 및 잘못된 설정 검증 |

## 5. 검증 결과와 범위

Windows 전체 빌드와 실제 NAND 경로 테스트를 수행했다. 검증 구성은 SSD 4개, SSD당 128블록, 블록당 8페이지, E_max=3, 초기 OP=0.3이며, SWANS를 꺼 EOL 기준의 차이를 확인했다.

| 실행 | 종료 시 관측값 |
|---|---|
| 모드 생략 / OP_RATIO 명시, OP 임계값 0.20 | 동일 완료 쓰기량과 종료 상태: TEC 66.666667%, 잔여 OP 약 0.195312 |
| TEC 임계값 100% | TEC 100%, 잔여 OP 0%. 중간의 OP 0.20 기준을 통과해 계속 실행 |
| TEC 임계값 30%, 퇴역 비활성 | TEC 33.333333%, 잔여 OP 0.296875, 불량 블록 0개 |
| 알 수 없는 모드 및 TEC 임계값 0·음수·101·NaN | 실행 오류로 거부 |

기존 TEC·트레이스 풀·예약 교환·마모 트리거 테스트도 통과했다. 이전 TEC 순위 검증에서는 전체 정렬 기준값과의 대조 및 EOL 시점 히스토그램 대조를 수행했다.

이는 기능 검증이며 논문 실험 전체의 재현이나 수명 개선 효과 검증은 아니다. 논문 식 (8)의 마모 증가율 추정은 구현 범위에 포함하지 않는다. TEC EOL에서는 매 erase마다 새 순위 계산이 필요하므로 큰 SSD 구성의 실행 시간 비용은 별도로 측정해야 한다. Linux 빌드와 대규모 워크로드 검증은 수행하지 않았다.
