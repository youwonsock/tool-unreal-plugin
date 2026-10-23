# Workspace 변경 검증 — 2026-10-24

기준 저장소: `tool-unreal-plugin`, 시작 커밋 `64f7def`.
이 기록은 현재 workspace 변경에 대한 검증입니다. [기존 검증 기록](VALIDATION.md)의 2026-10-16 Win64 실행 결과를 현재 코드의 실행 결과로 승계하지 않습니다.

아래 workspace 분리 기록은 `1168a18`까지의 변경입니다. 이후 사용자 요청으로 구형 MCP·설정·기록 호환도 제거했으며, 현재 API 2.0의 변경과 검증 범위는 문서 끝의 별도 항목에 기록합니다. 아래의 API 유지 설명과 Python 68개 결과는 workspace 분리 당시 기준입니다.

## 변경 내용

- 공통 `TModel`은 행별 독립 옵션, 기본 편집/행 편집, 체크 선택, 순서, 입력 세대, 유효성, 분리된 실행 snapshot을 관리합니다.
- 플러그인별 workspace controller와 UE 공통 어댑터가 입력 초안, 정규화, 프리셋, 요청 구성, 작업 선택과 이력 캐시를 담당합니다. Slate는 모듈이 소유한 controller를 주입받습니다.
- 옵션 적용은 새 기본값의 암묵적 전파 없이 명시적인 행 편집/프리셋/일괄 적용/초기화로 수행합니다. FBX JSON은 현재 행에 부분 적용하며 `{}`는 값 변경을 하지 않습니다.
- JSON/경로 입력을 변경 즉시 보관하고 검사/접수 전에 확정합니다. 이전 편집 세대의 입력은 다른 대상이나 초기화된 행에 적용하지 않습니다.
- CubeShot은 기본 편집 중 미리보기를 비웁니다. 미리보기 요청 번호는 ViewModel을 바꿔도 증가하며 배치 접수 조건은 미리보기 완료와 분리합니다. Auto Fit의 표시 계산과 보관 수동 값을 분리합니다.
- 공통 C++ 원본은 두 독립 플러그인으로 동기화합니다. MCP 재등록으로 CubeShot 서비스가 재생성되는 경우 controller가 이전 서비스 참조를 계속 사용하지 않도록 접근자를 통해 연결합니다.
- 검토 보완: CubeShot의 표시용 옵션에는 미확정 typed 초안도 반영해 행 전환·편집기 재생성 후 복원합니다. 마지막 유효 실행 옵션과 오류 초안은 계속 분리합니다.
- 입력 오류는 범위별 초안에서 투영하고 revision으로 캐시합니다. 초기화·프리셋·일괄 덮어쓰기로 복구한 범위의 오류만 사라지며 다른 범위의 오류는 유지합니다. FBX 출력 계획의 본문·요약·재검사 상태는 하나의 상태 객체로 갱신합니다.
- CubeShot controller가 snapshot 생성, 중복 접수 차단, 검사, UI 승인 콜백, checkout, 엄격한 재검사와 접수를 담당합니다. Slate는 확인창과 미리보기 중단·재개를 처리합니다. 두 툴의 프리셋 저장·삭제·가져오기·내보내기와 파일 읽기/쓰기도 controller로 이동했습니다. 취소한 승인은 저장이나 접수를 수행하지 않고, 저장 확인 중 편집 범위가 바뀌면 이전 저장을 거부합니다.
- FBX 회귀 보완: 결과 JSON 복사는 `Selected`를 직렬화하지 않고 이력 캐시 문자열을 그대로 사용합니다. 작업 이력이 없으면 빈 문자열을 안전하게 복사합니다. 개별 맵과 전체 맵 선택·해제는 controller 명령으로 처리하며, 마지막 유효 옵션 대신 현재 초안에 변경을 누적합니다. 유효한 JSON 초안은 후속 맵 명령 전에 확정합니다. 별도의 경로 오류 때문에 확정하지 못하면 유효한 JSON 필드를 읽고 후속 맵 명령을 그 위에 누적해, 경로 수정 시 이전 JSON이 맵 선택을 되돌리지 않게 합니다. 잘못된 JSON 원문과 구문·의미 오류는 보존하며 맵 명령으로 오류를 숨기지 않습니다.

촬영/베이크/에셋 저장/FBX 게시 실행기, MCP 도구 구현·manifest·schema·버전, 프리셋과 작업 기록 형식은 변경하지 않았습니다. 미실행 workspace를 디스크에 저장하는 기능은 추가하지 않았습니다.

## 실제 실행한 검사

전체 변경 검토 보완: CubeShot의 프리셋 저장 모달 복귀와 편집기 재생성에서 종료 상태를 검사하고, 탭 종료 시 입력 위젯 연결을 해제합니다. 공통 이력 controller는 작업 ID·개별 작업 revision 기반의 20개 결과 캐시와 별도 결과 표시 revision을 사용합니다. 일반 작업 전환은 캐시를 재사용하고 명시적 새로고침은 상태와 외부 복구 정보를 다시 읽습니다. FBX 복구 진단 재조회·이력 전환을 controller 명령으로 이동했으며, 두 툴의 행 수정 표시는 revision으로 캐시한 텍스트를 읽습니다.

이력 표시에서는 기존 CubeShot UI 검사가 요구하는 메타데이터 검토 필요·확인 완료 안내와 보존 증거 경로를 유지합니다. 종료된 작업에는 취소 대기 안내를 표시하지 않습니다.

서비스 교체 보완: `FJobService`는 생성 시 현재 로드된 런타임의 단조 증가 인스턴스 세대 번호를 받습니다. Controller는 주소나 revision 감소 대신 이 세대로 캐시 소유자를 식별하고, 세대 변경 시 결과 캐시·이전 pin 식별자를 비운 뒤 현재 표시 작업을 새 서비스에 한 번 pin합니다. 갱신 전에 탭이나 controller가 종료돼도 이전 세대의 unpin을 새 서비스에 전달하지 않습니다. 세대 번호는 C++ 내부 상태이며 MCP 응답·작업 기록·프리셋 형식에는 추가하지 않습니다.

환경: macOS arm64, Python 3.14.6, Apple Clang 21. C++ 검사는 `-std=c++17 -Wall -Wextra -Werror`로 실행했습니다. Python 검사는 임시 가상환경에 `MCP/requirements.txt`를 설치해 실행했습니다.

| 검사 | 결과 |
|---|---|
| `python3 Scripts/test_workspace.py` | 공통 원본, CubeShot 배포본, FBX 배포본에서 각각 **상태 코어 25개 + 서비스 세대 9개 검사 통과**; 두 Slate 소스의 서비스 소유권 정적 검사 통과 |
| `python3 -m unittest discover -s MCP/tests -q` | **68개 통과** |
| `python3 -m unittest discover -s Unreal_CubeShot/MCP/tests -q` | **1개 통과** |
| `python3 Scripts/sync_runtime.py --check` | 통과, provider 2개·native 파일 5개 |
| `git diff --check` | 통과 |

독립 C++ 검사는 실제 제품에서 사용하는 `UnrealToolsWorkspace.h`를 컴파일합니다. 각 빌드에는 선택한 헤더만 별도 임시 include 디렉터리로 노출하며, Unreal 헤더나 상위 저장소 include 경로를 사용하지 않습니다. 옵션 payload는 독립 테스트용 중첩 데이터이며 UE의 `FJsonObject`는 아닙니다.

서비스 세대 검사는 제품의 `UnrealToolsServiceInstance.h`를 별도로 컴파일합니다. 동일 storage·동일 revision의 테스트 객체를 재생성해 세대 증가를 확인하고, 반복 생성·서로 다른 C++ translation unit·동시 할당에서 세대 번호가 중복되거나 초기화되지 않는지 검사합니다. 이 검사는 실제 할당기를 실행하지만 `FJobService`나 UE controller의 캐시·pin 동작을 실행한 결과는 아닙니다. 공통 Python 배포 검사는 새 헤더의 복사·drift 검출·복구도 확인합니다.

검사에는 기존 행의 기본값 독립성, 중첩 데이터 복사 격리, 선택 행 일괄 적용, 빈 선택, 조회/중복 추가의 무변경, 재정렬과 삭제 후 편집 대상, 잘못된 기본값/행의 구분, 지연 입력 세대 거부, 삭제 후 같은 경로 재추가, 실행 snapshot 격리, 세션 객체 재사용과 새 세션 초기화를 포함합니다.

Python 검사는 임시 프로젝트와 모의 전송을 사용하는 기존 설치·연결·업그레이드·문서 계약 검사입니다. 실제 Unreal MCP 서버와 통신한 결과가 아닙니다. macOS 임시 경로의 `/var`·`/private/var` 표현 차이는 테스트 fixture 정규화로 해결했고 제품의 경로 판정은 변경하지 않았습니다. 네이티브 배포 검사에는 새 workspace 헤더의 drift 감지와 복구도 포함했습니다.

Slate 소유권 검사는 production UI에서 provider service, profile/job store를 직접 호출하거나 접수·프리셋 요청을 구성하는 코드가 없는지 검사합니다. 테스트 fixture의 작업 조회는 이 검사에서 제외합니다. 이는 소스 경계에 대한 정적 검사이며 controller의 실행 결과를 입증하지 않습니다. 검토 보완 후 위 독립 검사와 Python 검사를 다시 실행했습니다.

FBX 회귀 수정에서는 Slate의 JSON 직렬화와 맵 옵션 배열 구성도 정적 검사에서 금지했습니다. 강화한 검사가 수정 전 JSON 복사 경로와 맵 배열 구성 경로를 각각 탐지하는 것을 확인한 뒤, 수정된 코드에서 통과시켰습니다. 이 검사는 null 포인터 경로나 맵 입력의 실제 UE 실행 결과를 대신하지 않습니다.

복구 서비스 직접 호출도 소유권 정적 검사에 추가했습니다. 수정 전 FBX 진단 버튼의 `RefreshRecoveryDiagnostics()` 호출에서 실패한 뒤, controller로 이동한 코드에서 통과하는 것을 확인했습니다. 이번에 추가한 캐시·종료·수정 표시 동작은 UE 어댑터와 Slate에 있으므로 위 25개 독립 C++ 검사에 포함되지 않습니다.

## 작성·갱신했지만 실행하지 않은 Unreal 검사

Unreal이 설치되어 있지 않아 Editor 타깃 빌드와 아래 Automation 검사는 실행하지 않았습니다. 독립 C++ 검사는 UE controller의 컴파일, Slate 바인딩, 실제 JSON 처리나 출력 결과를 입증하지 않습니다.

- `CubeShot.Editor.Workspace.OptionsAndSnapshots`: 실제 JSON 복사, 행별 기본값 독립성, 고정 snapshot, Auto Fit 해제, 잘못된 기본 입력과 정상 배치의 분리.
- `FBX_Exporter.Editor.Workspace.DraftsAndSnapshots`: 편집 열기의 무변경, 부분 JSON/빈 JSON, focus-loss 이전 입력 확정, 초기화 이전 토큰 거부, 표시 캐시.
- `CubeShot.Editor.Workspace.DraftRebind`: 잘못된 Texture 배경 입력 후 A/B 전환과 표시 옵션 재생성, 다른 typed 입력과의 공존, 명시적 초기화 후 오류 해제.
- `CubeShot.Editor.Workspace.SubmissionFlow`: 주입한 검사/checkout/접수 backend로 동일 request·snapshot·UUID 전달, 중복 접수 거부, 승인 취소·checkout 거부·엄격한 재검사 실패·승인 중 종료의 접수 차단과 상태 해제. 실제 촬영이나 저장을 수행하는 테스트는 아닙니다.
- `FBX_Exporter.Editor.Workspace.ErrorsAndPlanInvalidation`: 여러 범위의 오류 중 복구한 범위만 해제, 프리셋·일괄 덮어쓰기 후 오류 해제, 출력 계획 본문·요약의 동시 무효화.
- `FBX_Exporter.Editor.Workspace.ProfileOperations`: 고유 ID의 테스트 프리셋 저장·교체 취소·편집 범위 변경 시 거부·내보내기·삭제·가져오기와 옵션 보존. 생성한 프리셋과 내보낸 파일은 테스트 종료 시 정리합니다.
- `FBX_Exporter.Editor.Workspace.MapsAccumulateInDrafts`: 잘못된 출력 경로 아래 연속 맵 해제·전체 해제·개별 활성화의 누적, 중복 방지, 경로 수정 후 요청 옵션, 잘못된 JSON 원문 보존, focus-loss 이전의 유효한 JSON 확정, 경로 오류와 유효한 JSON이 동시에 있을 때의 맵 입력 순서, JSON 의미 오류를 맵 변경으로 숨기지 않음, 다른 행·기본 옵션의 독립성, 초기화 이전 맵 이벤트 거부.
- 기존 `FBX_Exporter.Editor.Integration.WorkspaceSelectionState`: 실제 production JSON 편집기에서 raw typing·focus-loss·초기화 후 지연 이벤트와 진단 표시 검사. 결과 복사 버튼이 사용하는 공통 함수에도 빈 이력과 캐시 JSON을 전달해 null 직렬화를 피하고 문자열을 그대로 복사하는지 검사합니다. 복사 대상 콜백을 주입하므로 테스트가 사용자의 실제 클립보드를 변경하지 않습니다.
- 기존 CubeShot 패널/애플리케이션 검사: controller 주입 및 동일 controller로 탭 재개방, 전체 preflight·checkout·기록 표시 검사.
- 기존 `CubeShot.Editor.MCP.RegistrationGeneration`: MCP 재등록 후 기존 workspace가 새 서비스에 연결되는지 검사 추가.
- 두 플러그인의 `Editor.Runtime.WorkspaceHistoryCache`: 별도 임시 저장소와 가짜 runner로 다른 작업 진행 시 선택 결과 재사용, 캐시된 작업 간 전환, 선택 작업 revision 변경, 강제 새로고침, 20개 캐시 한도를 검사합니다. 실제 촬영·추출이나 사용자 작업 기록을 변경하는 테스트가 아닙니다.
- 두 플러그인의 `Editor.Runtime.WorkspaceServiceGeneration`: 실제 `FJobService`를 placement new로 동일 storage에 재생성하고 글로벌·개별 작업 revision이 동일함을 먼저 확인합니다. 강제 갱신 없이 `queued` 캐시가 `interrupted`로 바뀌는지, 이전 snapshot은 유지되는지, 새 서비스에서 다른 관찰자의 pin을 보존하며 controller pin을 한 번 등록하는지 검사합니다. 반복 조회·탭 닫기/재열기와 갱신 전 서비스 재생성 후 detach·shutdown도 검사합니다. 별도 임시 메타데이터와 가짜 runner를 사용하며 사용자 작업이나 실제 촬영·추출을 실행하지 않습니다. **이 UE 검사는 작성했지만 미실행입니다.**
- `CubeShot.Editor.UI.ProfileDialogAfterShutdown`: 실제 프리셋 저장 핸들러의 확인 콜백에 탭 종료·모듈 종료·승인 취소를 주입하고, 복귀 후 해제한 ViewModel·미리보기 서비스·타이머가 되살아나지 않는지 검사합니다. 고유 ID의 테스트 프리셋은 종료 시 삭제합니다.
- `CubeShot.Editor.UI.LiveModifiedLabel` 및 FBX `Integration.WorkspaceSelectionState` 보완: 실제 행 텍스트 위젯을 유지한 채 옵션 변경과 초기화 후 Slate prepass로 수정 표시를 갱신하고, 같은 revision의 반복 조회가 표시 데이터를 재생성하지 않는지 검사합니다.
- `FBX_Exporter.Editor.Workspace.RecoveryCommand`: 주입한 재조회 콜백으로 controller의 스캔 호출·첫 진단 페이지 전환·종료 후 요청 거부를 검사합니다. 실제 복구 스캔 실행 결과를 대신하지 않습니다.

## Unreal 환경에서의 후속 확인

1. UE 5.8 Win64 Editor 타깃을 빌드해 C++ API·템플릿과 Slate 바인딩을 확인합니다.
2. 두 도구의 Workspace/Application/UI 자동화와 기존 capture/export 회귀를 실행합니다. 기존 실제 PIE·강제 종료 fixture는 해당 테스트가 요구하는 소유 QA 프로젝트와 명시적 조건을 따릅니다.
3. 행 A/B에서 기본 옵션 변경, 프리셋, 일괄 적용, 기본값 복귀와 부분/잘못된 JSON을 확인합니다. 기본 옵션 오류가 기존 유효 배치를 막지 않는지도 확인합니다.
4. A 편집 후 B 전환, 편집 중 삭제, 일괄 적용/초기화 이후 focus-loss가 다른 행이나 이전 값으로 되돌리지 않는지 확인합니다.
5. CubeShot 기본 편집의 빈 미리보기, 같은 옵션을 가진 다른 메시 전환, 배경만 재합성, Auto Fit on/off, 배치 검사 중 미리보기 중단·접수 실패 후 재예약을 확인합니다.
6. 탭 종료/재개방, 실행 중 탭 종료, 이력 선택·pin 해제·모듈 종료, 실제 Texture2D/PNG/FBX 출력과 독립 UAT 패키지를 확인합니다.
7. Texture 배경을 텍스처 없이 선택한 상태에서 행 전환·탭 재개방 후 같은 초안과 오류가 보이는지 확인합니다. 해당 행만 복구했을 때 다른 행의 오류는 남는지, 옵션 변경 후 FBX 계획 요약과 본문 모두 재검사 필요로 바뀌는지도 확인합니다.
8. FBX 작업 이력이 없는 상태에서 결과 JSON 복사 버튼을 눌러 안전하게 빈 문자열을 복사하는지 확인합니다. 출력 경로를 비운 상태에서 BaseColor와 Normal을 연속 해제한 뒤 경로만 수정해 두 해제가 모두 유지되는지도 확인합니다.
9. CubeShot 프리셋 교체 확인 중 탭·모듈 종료 후 UI 복귀가 차단되는지, 두 툴의 `*` 표시가 행 재생성 없이 갱신되는지, 다른 작업 진행 중 과거 결과를 보고 있을 때 결과 캐시가 재사용되는지 확인합니다. FBX 진단 버튼의 실제 스캔·진단 목록 갱신도 확인합니다.
10. 두 `Editor.Runtime.WorkspaceServiceGeneration` 검사를 실행해 동일 주소·동일 revision의 재생성에서 캐시와 pin이 새 세대로 연결되는지 확인합니다. 실제 MCP 재등록 전후의 탭 표시·pin 수명도 확인합니다.

이번 변경의 완료 범위는 구현, 독립 코어·Python·배포 검사와 소스 검토입니다. Unreal 빌드·GUI·렌더링·최종 출력 검증은 위 후속 항목으로 남습니다.

## API 2.0 레거시 제거 — 2026-10-24

사용자가 구형 MCP 호출·설정 업그레이드·기록 호환까지 제거하는 범위를 선택했습니다. 기준 커밋은 `1168a18`이며, 현재 촬영·추출 실행기와 workspace controller는 유지합니다.

- `cubeshot_generate`, `fbx_exporter_export`와 완료까지 대기하던 비동기 어댑터·완료 콜백 저장소를 제거했습니다. 촬영·추출은 `job_start`/`job_status`/`job_control`을 사용합니다. 내부 C++의 unattended 출력 경로는 현재 네이티브 출력 검사에도 사용하므로 유지합니다.
- preflight는 `mesh_paths`·`options`·`profile_id`·`overrides` 형태의 배치 요청만 받습니다. 한 대상도 배열로 전달하며 단일 결과를 최상위로 펼치지 않습니다. 구형 `mesh_path` 및 최상위 옵션 입력은 거부합니다.
- API·런타임·두 플러그인 버전은 `2.0.0`입니다. 현재 등록 도구는 CubeShot 8개·FBX 7개입니다. 독립 PNG는 기본 `overwrite=false`이며 기존 PNG 교체에는 명시적인 true가 필요합니다.
- provider별 MCP 설정 병합, 이전 도구 목록 추정, manifest 기본 버전 추정, capabilities 객체 변환, 이전 작업 응답 envelope/state 이름 변환을 제거했습니다. 현재 생성 상태에는 API 버전을 명시합니다. 이전 개별 서버 테이블은 자동 변경하지 않으며, 통합 설정·상태의 재구성에는 명시적인 `--replace-owned-config`가 필요합니다.
- `MCP/sync_runtime.py`, `cubeshot_common.py`, CLI의 이전 `run_calls`/`default_project` 어댑터, 인자만 담는 export 예제를 제거했습니다. 테스트는 `Scripts/sync_runtime.py`를 직접 가져옵니다. 새로운 요청 예제는 `examples/job-start.json`입니다.
- Haven/AssetImportProject 출력 경로 이관과 전용 테스트 접근자를 제거했습니다. 현재 FBX 설정만 읽으며 읽기 과정에서 설정을 저장하지 않습니다. 현재 설정 조회 테스트로 대체했습니다.
- 완료 시각을 파일 수정 시각에서 생성하는 기록 보정을 제거했습니다. 현재 필드·완료 시각·실행 모드가 맞지 않는 기록은 진단으로 남기고 원문을 보존합니다. 이전 변환 메타데이터도 지원하지 않습니다. 보존·정리 단계에서 다시 읽고 검사하는 보호는 유지합니다.
- 미사용 UI/촬영 wrapper와 FBX 대화상자 wrapper, ViewModel의 별도 revision·변경 종류·실행 허용 상태와 중복 초기값을 제거했습니다. ViewModel은 표시 검증을 수행하며 실행 판단과 미리보기 예약 번호는 기존 controller/Slate 경로를 사용합니다. runner의 snapshot revision 제공은 필수 계약으로 바꿨습니다.
- API 1.x의 `tool-spec.json`/`TOOL_REFERENCE.md` 캡처를 제거했습니다. 실제 API 2.0 Editor catalogue가 생긴 뒤 다시 생성해야 합니다. 문서 생성기는 manifest에 없는 provider 도구가 섞인 구형 catalogue를 거부합니다.

현재 코드에서 실행한 검사:

| 검사 | 결과 |
|---|---|
| `python3 Scripts/test_workspace.py` | 원본·두 배포본에서 각각 상태 코어 25개 + 세대 할당기 9개 통과, Slate 2개 소유권 정적 검사 통과 |
| `python3 -m unittest discover -s MCP/tests -q` | 75개 통과 |
| `python3 -m unittest discover -s Unreal_CubeShot/MCP/tests -q` | 1개 통과 |
| `python3 Scripts/sync_runtime.py --check` | provider 2개·native 파일 5개 일치 |
| `git diff --check` | 통과 |

Python에는 구형 도구 접수 전 차단, 구형 설정·상태·manifest의 자동 이전 거부, API·capabilities 검증, 현재 설정의 재설치·백업·rollback, 독립 설치와 현재 예제 검사를 포함합니다. C++ 도구 등록 목록·manifest·버전 일치는 정적으로 확인합니다. Python 호출은 모의 전송이며 실제 Editor API 2.0을 실행한 결과가 아닙니다.

Editor에서 남은 검사: 두 플러그인 빌드, 8개/7개 실제 등록과 새 preflight schema, 현재 job_start 기반 geometry 내보내기, PNG 기본 보호/명시적 덮어쓰기, 현재 FBX 설정 읽기, 미지원 기록의 진단·원문 보존·수동 확인, 변경한 runner revision·프레임 구동·저장 실패 검사를 실행해야 합니다. 앞서 작성한 동일 주소·동일 revision 서비스 교체의 캐시·pin 검사와 실제 Slate·GPU·Texture2D/PNG/FBX·UAT 검증도 계속 미실행입니다. 실제 catalogue 문서 재생성도 이 단계에서 수행합니다.

### 비활성 구버전 검색 회귀 수정 — 2026-10-24

- 검색은 `.uproject`·플러그인·모듈의 활성 여부와 현재 플랫폼/Editor 타깃 조건을 먼저 검사합니다. 제외된 플러그인의 manifest는 읽지 않으며 활성 플러그인만 중복·API 검증과 도구 목록에 참여합니다. 활성 구버전에 대한 엄격한 API 2 거부는 유지합니다.
- descriptor의 `WhitelistPlatforms/Targets`, `BlacklistPlatforms/Targets` fallback을 제거했습니다. 현재 `PlatformAllowList/DenyList`, `TargetAllowList/DenyList`, `SupportedTargetPlatforms` 판정은 유지합니다. 원본과 두 플러그인 배포본에 동기화했습니다.
- 새 Python 테스트 7개를 추가했습니다. CubeShot/FBX를 번갈아 활성화해 비활성 1.2 및 API 버전 누락 manifest와 함께 설정·재설치·목록·모의 호출이 되는지, 활성 구버전은 파일 변경·연결 전에 거부되는지 확인합니다. 플랫폼·타깃·기본 비활성화로 제외한 읽을 수 없는 manifest도 검사합니다. 현재 descriptor 필터와 구형 키 무시를 검증하며, 복사한 두 플러그인의 setup을 저장소 밖 별도 `python -S` 프로세스에서 실행합니다.
- 수정 전 새 설정/descriptor 테스트가 실패하는 것을 확인했고, 수정 후 공통 Python **82개**, CubeShot 독립 설치 smoke **1개**가 통과했습니다. MCP 호출은 모의 전송이며 실제 Editor 서버에 연결하지 않았습니다.
- `python3 Scripts/test_workspace.py`는 원본·두 배포본 각각 상태 코어 25개와 서비스 세대 9개, Slate 소유권 정적 검사 2개를 통과했습니다. `python3 Scripts/sync_runtime.py --check`와 `git diff --check`도 통과했습니다.
- Editor 빌드·Automation·실제 MCP catalogue·Slate/GPU/출력 검증은 위 목록대로 미실행입니다. 이번 수정은 Python 검색과 descriptor 판정에 한정하며 이 결과를 Unreal 어댑터 검증으로 간주하지 않습니다.

### 비활성 provider의 구형 서버 설정 감지 분리 — 2026-10-24

- 비활성·필터 제외 플러그인의 manifest를 읽거나 검증하지 않는 검색 규칙을 유지했습니다. 설정 계층에 과거 서버 이름 `cubeshot`·`fbx_exporter`의 금지 목록을 두어 활성 provider 목록에 의존하지 않고 구형 테이블을 감지합니다. 현재 도구·provider 검색은 기존 manifest 규칙을 계속 사용합니다.
- 플러그인이 비활성화되거나 제거되어도 구형 서버 테이블이 있으면 setup은 파일 변경 전에, 공통/플러그인 CLI는 연결 전에 이름을 표시하고 거부합니다. 자동 삭제·병합·이관은 없으며 `--dry-run`·`--replace-owned-config`도 감지를 우회하지 않습니다. 구형 테이블을 직접 제거한 뒤 현재 통합 설정의 재실행과 다른 서버 보존은 정상 동작합니다.
- 회귀 테스트 5개를 추가했습니다. 두 플러그인을 번갈아 활성화해 비활성 1.2·버전 누락·손상 manifest와 구형 서버 테이블의 조합, 기존 통합 설정·상태·INI·백업 보존, 플러그인 제거 후 감지, 공통/개별 CLI의 연결 전 거부를 검사합니다. 두 독립 복사본의 setup도 저장소 밖 별도 `python -S` 프로세스로 확인합니다.
- 수정 전 새 설정 테스트가 실패하는 것을 확인했습니다. 수정 후 공통 Python **87개**와 CubeShot 독립 설치 smoke **1개**가 통과했습니다. MCP 전송은 모의 세션이며 실제 Editor 연결 결과가 아닙니다.
- 독립 C++ 상태 코어 25개·서비스 세대 9개는 원본과 두 배포본 각각 통과했고 Slate 소유권 정적 검사 2개도 통과했습니다. `python3 Scripts/sync_runtime.py --check`와 `git diff --check`를 통과했으며, 두 배포본의 설정 감지 코드가 원본과 일치합니다.
- 이번 수정은 Python 설정 감지에 한정합니다. Editor 빌드·Native Automation·실제 MCP 등록/호출·Slate·캐시/pin 수명·GPU·Texture2D/PNG/FBX·UAT 검증은 계속 미실행입니다.

### 프리셋 검증·손상 파일 보호·FBX 복구 표시 캐시 수정 — 2026-10-24

- 공통 `FProfileStore::ValidateUserProfile`로 읽기·저장·controller의 저장/가져오기 검증을 통일했습니다. 현재 schema/version/provider/ID 타입과 옵션을 검사하고 공백뿐인 표시 이름을 거부합니다. 정상 이름의 앞뒤 공백은 보존합니다. 저장 후 재조회 실패는 `IO_ERROR`로 반환하며 controller도 누락된 반환 옵션을 빈 기본 옵션으로 적용하지 않습니다.
- 덮어쓰기 판정은 프리셋 파싱 성공 여부 대신 실제 사용자 파일 존재 여부를 사용합니다. 손상 JSON·미지원 schema·잘못된 옵션의 파일도 GUI 저장/가져오기 확인과 MCP의 명시적인 boolean `overwrite=true`가 필요합니다. 유효하지 않은 새 입력은 확인창 전에 거부하며, 승인 중 편집 범위가 바뀌면 저장하지 않는 기존 보호도 유지합니다.
- 공통 이력 캐시에 외부 표시 상태 확인 콜백을 추가했습니다. FBX controller는 journal 존재 여부와 캐시의 누락 표시를 비교합니다. 삭제·복원 시 service/job revision이 같아도 새 서비스 snapshot으로 표시를 재생성하므로 누락 경고가 추가·제거됩니다. 상태 불변 시 JSON 재조회·직렬화를 하지 않으며, 다른 캐시 항목으로 전환할 때도 확인합니다. 서비스 세대 구분·최대 20개 결과·pin 수명은 유지합니다. 파일 내용의 유효성은 기존 복구 진단 서비스가 담당합니다.

작성·확장한 Unreal 회귀 검사 (**미실행**):

- 두 플러그인의 `Editor.Runtime.Profiles`: 읽기와 저장의 동일 판정, 빈 이름/공백 이름/정상 이름 보존, 잘못된 타입·provider·알 수 없는 필드 거부, 잘못된 입력의 원문 보존, 손상/미지원/옵션 오류 파일의 승인 없거나 false일 때 거부 및 true일 때 복구, 저장 후 읽기 실패의 오류 응답, 공통 controller에서 실패한 저장의 옵션·편집 범위 보존.
- `FBX_Exporter.Editor.Workspace.ProfileOperations`: 실제 FBX 옵션으로 공백 이름 저장 거부, 손상·미지원 파일에 대한 저장과 가져오기 양쪽 확인창·거절·승인, 확인 중 편집 범위 변경 거부, 옵션 및 기존 파일 바이트 보존.
- `FBX_Exporter.Editor.Workspace.RecoveryFileCache`: 임시 파일과 별도 `FJobService`·가짜 runner에 실제 FBX 표시 함수를 연결합니다. 파일 삭제·복원, service/job revision 불변, 경고와 복사 JSON 갱신, 원래 runner 경고·이전 snapshot 보존, 상태 불변 시 결과 재사용, 캐시된 다른 작업 재선택, 기록 비변경·pin 해제를 검사합니다. 실제 추출을 수행하는 테스트는 아닙니다.
- 기존 공통 Automation 등록 검사에 누락된 이력 캐시·서비스 세대 두 케이스를 포함해 기대 개수를 16에서 실제 18로 수정했습니다. 새 Python 정적 계약 검사가 기존 기대값에서 `18 != 16`으로 실패한 뒤 수정 후 통과했습니다.

현재 변경분에서 Unreal 없이 실행한 검사:

| 검사 | 결과 |
|---|---|
| `python3 Scripts/test_workspace.py` | 원본·두 배포본 각각 상태 코어 25개 + 서비스 세대 9개 통과, Slate 소유권 정적 검사 2개 통과 |
| 테스트 venv의 `python -m unittest discover -s MCP/tests -q` | 88개 통과 |
| 테스트 venv의 `python -m unittest discover -s Unreal_CubeShot/MCP/tests -q` | 독립 설치 smoke 1개 통과 |
| `python3 Scripts/sync_runtime.py --check` | provider 2개, MCP 파일 7개씩·native 파일 5개씩 일치 |
| `git diff --check` | 통과 |

변경한 원본/FBX C++ 소스 7개는 UE 매크로를 임시 치환한 Tree-sitter 구문 비교에서 기존 대비 새 진단이 없었습니다. 최초 파서 0.26.0 실행은 비정상 종료했으며 임시 검증 환경의 0.25.2로 다시 비교했습니다. 이 보조 검사는 Unreal 타입 검사나 컴파일 결과가 아닙니다. 독립 C++ 테스트도 이번에 바꾼 UE 프리셋 I/O·controller 캐시를 실행하지 않으므로 위 동작은 소스 검토와 미실행 회귀 테스트의 범위입니다.

Editor 후속 검증: Win64 Editor 빌드와 위 Automation 및 기존 `WorkspaceHistoryCache`·`WorkspaceServiceGeneration`을 실행해야 합니다. 두 GUI에서 정상/공백 이름 저장, 손상 파일 교체 취소·승인과 MCP의 승인 없는 거부를 확인하고, FBX 복구 journal 삭제·복원 후 열린 이력의 경고·복사 JSON·탭 재개방·pin 수명을 확인해야 합니다. 기존 Slate·GPU·Texture2D/PNG/FBX 출력·UAT 검증도 계속 미실행입니다. API 버전·요청 schema·프리셋 및 작업 기록 저장 형식은 변경하지 않았습니다.
