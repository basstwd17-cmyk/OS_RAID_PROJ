# 설정 파서 결함 2개 수정

확인일: 2026-10-31. 대상: `C:/CODEX/output/mqsim-compare-20261028`, 기준 HEAD `e6fb9df` 위 기존 미커밋 작업본. 기존 변경은 보존했다. 이번 변경은 아직 커밋·원격 반영하지 않았다.

## 변경

`src/exec/Device_Parameter_Set.cpp`:

- SSD XML에서 `op_zone_migration`과 `SWANS_Reserve_Zone_Enabled`는 각각 단독 사용 가능하다. 두 이름이 같은 bool 값이면 기존 직렬화 파일과의 호환을 위해 허용한다. 서로 다른 값이거나 같은 이름이 반복되면 순서와 관계없이 설정 오류로 종료한다. 별도 SWANS XML의 기존 중복 거절 정책은 유지한다.
- 직렬화는 `op_zone_migration`만 출력한다. 레거시 이름의 읽기 호환은 유지한다.
- TEC EOL 임계값은 `std::stod`가 소비한 위치 이후에 공백만 있는지 검사한다. 유한값과 `(0,100]` 범위도 검사한다. 소수·지수·양수 부호·앞뒤 공백은 허용하고 `30garbage`, `30%`, `30,5`, `3e1junk`, `30 40`은 거절한다. 설정이 존재하면 숫자 검사를 수행하며, 실제 종료 기준 선택은 기존 `End_of_Life_Mode`를 따른다.

`tests/reserve_exchange_smoke.py`: 충돌 순서 2개, 동일 태그 중복, 동일 값 별칭 호환의 회귀 사례를 추가했다.

`tests/eol_modes_smoke.py`: 잘못된 숫자 접미부 5개와 정상 숫자 표현 6개를 추가하고 실제 출력 임계값도 비교한다.

## 검증

수정 전 실행 파일에서 새 EOL/OP 별칭 테스트가 각각 실패하는 것을 확인한 뒤 구현을 변경했다.

다음 명령의 종료 코드가 모두 0이었다. Python은 `C:/Users/kevin/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`를 사용했다.

```powershell
./tests/build_windows.ps1
python tests/eol_modes_smoke.py build/windows/MQSim.exe
python tests/reserve_exchange_smoke.py build/windows/MQSim.exe
python tests/advanced_swans_smoke.py build/windows/MQSim.exe
python tests/wear_trigger_smoke.py build/windows/MQSim.exe
python build/validate_parser_fixed.py
git diff --check
```

기존 35개 입력 검증은 역사적 원본 스크립트/결과를 보존하고, 별도 출력 디렉터리와 불일치 0개 기대값으로 실행했다. 결과는 35/35 일치, 불일치 0개다. `build/validation-findings-1-2-fixed/results.json`에 실행 파일 및 소스 SHA-256과 사례별 결과가 있다.

로그: `build/parser-red-eol.log`, `build/parser-red-reserve.log`, `build/parser-fix-build.log`, `build/parser-fix-eol_modes.log`, `build/parser-fix-reserve_exchange.log`, `build/parser-fix-advanced_swans.log`, `build/parser-fix-wear_trigger.log`, `build/parser-fix-35-cases.log`.

OP 교환은 기본 2회·분할 1회·TEC 218회·WRITE_BYTES 1459회 완료를 확인했다. 대규모 수명·성능 및 Linux 검증은 이번 범위에 포함하지 않았다.
