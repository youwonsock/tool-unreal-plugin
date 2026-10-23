# CubeShot 1.2

UE 5.8 / Win64 Editor용 Static Mesh 6면 촬영 도구입니다. 독립 도킹 탭과 MCP가 같은 단계 실행 서비스를 사용하며, 폴더 전체를 다른 프로젝트에 복사해 단독으로 사용할 수 있습니다.

## 설치

`Unreal_CubeShot` 폴더 전체를 프로젝트의 `Plugins/Unreal_CubeShot`에 복사하거나 상위 폴더를 `.uproject`의 `AdditionalPluginDirectories`에 등록합니다. 플러그인 바이너리 갱신은 Editor 재시작을 요구하며 동적 모듈 reload를 지원하지 않습니다. Editor 타깃을 빌드한 뒤 `Tools > CubeShot`을 엽니다. 엔진의 `ModelContextProtocol` 플러그인은 `.uplugin` 의존성으로 활성화됩니다.

[MCP 설치·도구·CLI 안내](MCP/README.md)를 참고하세요. 별도 MCP 서버 프로세스나 다른 프로젝트의 Scripts 폴더는 필요하지 않습니다.

## 도킹 탭 사용

1. Content Browser에서 여러 Static Mesh를 선택하고 `Use Content Browser Selection`을 누르거나 탭으로 드래그합니다. 정확한 object path로 중복을 제거합니다.
2. 대상 이름을 눌러 해당 행을 미리 보고 편집합니다. 체크 박스로 여러 행을 선택하며 Select All/Clear Selection/Remove Selected를 제공합니다. ↑/↓와 ×도 사용할 수 있고 제거 후 남은 실행 순서를 유지합니다.
3. 프리셋을 선택하고 배경·카메라·해상도·PNG 옵션을 조정합니다. 옵션을 편집하면 미리보기 중인 행에 저장합니다. `Apply current options to checked rows`는 여러 체크 행에 값을 복사하며 다른 행의 옵션을 유지합니다. 체크 선택과 미리보기 행은 독립적이고 실행은 대상 목록의 순서대로 수행합니다. 별표는 변경한 행을 나타냅니다.
4. `Inspect all outputs`으로 전체 대상의 출력, 경고, 오류와 덮어쓰기 여부를 확인합니다. 실행 전 검사도 첫 오류 이후의 모든 행을 끝까지 확인하며 오류가 하나라도 있으면 전체 접수를 거부합니다.
5. `Start Capture Queue`로 실행합니다. 기존 Texture2D 또는 선택한 PNG가 있으면 실행마다 명시적으로 교체를 확인합니다.
6. 결과 영역에서 진행 단계, 대상별 결과, 저장·실패·dirty 패키지·staging 경로를 확인합니다. 출력 폴더 열기와 결과 복사를 지원합니다.

작업은 탭을 닫아도 계속됩니다. 재개방하면 서비스의 작업 기록과 결과를 표시합니다. 결과 위의 기록 행을 선택하면 이전 작업도 확인할 수 있고, 생성 Texture2D를 Content Browser에서 찾아볼 수 있습니다. `Retry failed`와 `Run skipped`는 원래 접수한 옵션을 유지하는 새 작업이며, preflight와 덮어쓰기 승인을 다시 수행합니다.

기본 프리셋은 수정하거나 삭제할 수 없습니다. 사용자 프리셋은 `Saved/CubeShot/Profiles`의 버전 있는 JSON이며 저장·삭제·가져오기·내보내기를 지원합니다. 출력 덮어쓰기 승인은 프리셋에 포함하지 않습니다.

| 프리셋 | 기본값 |
| --- | --- |
| Editor Default | 1024, Perspective, Transparent, Auto Fit, PNG 자동 저장 끔 |
| Item Icon | 512, Orthographic, Transparent, Auto Fit, PNG Front, trim=true, padding=8, rotation=0 |
| 기존 MCP 기본값 | 512, Orthographic, Transparent, Auto Fit, PNG 자동 저장 끔 |

Solid Color는 **선형 RGBA**로 해석하며 UI의 초기 색상은 sRGB `#202020`입니다. 미리보기는 방향당 최대 512px이고 약 150ms debounce를 적용합니다. 여섯 방향이 모두 준비된 뒤 함께 교체합니다. 방향 제목을 클릭하면 큰 창으로 확인할 수 있습니다. 큐 작업 및 PIE/SIE 중에는 미리보기를 정지합니다.

## 출력·촬영 규칙

`/Game/Props/SM_Chair.SM_Chair`는 `/Game/CubeShot/Props/SM_Chair/T_SM_Chair_<Direction>`으로 출력합니다. 엔진·플러그인 메시도 지원하며 `/Game/CubeShot/_Mounts/<MountName>/...`으로 미러링합니다. 기존 Texture2D를 같은 UObject로 갱신해 참조를 보존합니다.

| 방향 | 카메라 위치 | 보는 축 | 이미지 위쪽 |
| --- | --- | --- | --- |
| Front | +X | -X | +Z |
| Back | -X | +X | +Z |
| Left | -Y | +Y | +Z |
| Right | +Y | -Y | +Z |
| Top | +Z | -Z | +X |
| Bottom | -Z | +Z | +X |

촬영은 레벨과 분리된 Preview Scene을 사용합니다. `SCS_SceneColorHDR` RGB와 inverse-opacity alpha를 함께 읽습니다. 2048 이하 출력은 2배 캡처 후 premultiplied RGBA 다운샘플을 사용하고, 더 큰 출력은 목표 해상도로 촬영합니다. Transparent는 straight alpha와 투명 texel RGB dilation을 사용하며 Solid/Texture 배경은 선형 공간에서 합성합니다. Texture 배경은 중앙 cover/crop입니다.

256~4096의 정사각 해상도를 지원합니다. NPOT는 정확한 크기를 유지하며 mip을 생성하지 않습니다. 최종 Texture2D는 BGRA8/sRGB, Default compression, World texture group, Clamp, Virtual Texture 비활성입니다. Opaque와 Masked를 지원하며 Translucent/Additive/Refraction에는 결과 제한 경고를 표시합니다.

선택 PNG는 `Saved/CubeShot/MCPExports`에 저장합니다. trim은 알파 0 외곽만 제거하고 padding은 0~64, rotation은 시계 방향 90도 단위 0~3입니다. 재샘플링 없이 RGBA 픽셀을 보존합니다. padding은 원본 프레임 안에서 경계를 확장하며 이미지 가장자리에서 잘립니다. 알파 0 픽셀의 dilated RGB도 유지하며 인위적인 검은 padding을 추가하지 않습니다. PNG를 같은 폴더의 임시 파일에 완성한 뒤 Win64 atomic rename으로 교체합니다.

## 진행·취소·복구

작업은 검사·리소스 준비, 방향별 capture/staging, 재검사·checkout, 6개 패키지 변경·저장, 선택 PNG 게시 순서로 실행합니다. 각 capture가 별도 단계이며 단계 사이에 Editor tick을 양보합니다. GPU 캡처·UObject 접근은 게임 스레드에서 수행하고, 소유권을 분리한 raw 픽셀의 합성·staging PNG 인코딩·검증과 최종 PNG framing/게시를 worker에서 수행합니다. Worker는 UObject나 게임 스레드 callback에 의존하지 않습니다. 직접 `Run`은 동일 단계를 inline으로 끝내고 MCP의 `RunAsync`는 완료 시 응답합니다. 플러그인당 작업은 하나씩 실행합니다.

PIE/SIE 또는 NullRHI에서는 촬영하지 않습니다. 소스 메시·재질 부모·사용 텍스처·외부 material function의 수정 및 property 변경을 감지하면 패키지 변경 전에 `source_changed`로 중단합니다. 접수한 경로·해석된 값 옵션은 고정되며 소스 UObject의 깊은 복제는 수행하지 않습니다.

GPU 호출은 강제 중단하지 않습니다. 취소 요청은 안전한 단계 경계에서 적용합니다. staging worker가 진행 중이면 파일을 쓰는 worker가 끝난 뒤 staging을 정리하고 취소하며, worker 아래에서 폴더를 삭제하지 않습니다. 여섯 패키지 저장을 시작하면 현재 메시의 저장과 선택 PNG 게시를 마친 뒤 후속 대상을 중단합니다. 마지막 메시가 완료됐다면 성공으로 끝날 수 있습니다. PIE/SIE 진입도 다음 안전한 경계에서 확인하며 자동으로 플레이를 종료하지 않습니다.

여러 Unreal 패키지 저장은 원자적이지 않습니다. 부분 실패는 `saved`, `failed`, `dirty_packages`, `errors`, `recovery_staging`으로 보고합니다. 저장된 출력은 유지하며 자동 복원을 약속하지 않습니다. staging 정리 실패도 경로와 경고를 보존합니다.

Editor 재시작으로 미완료 작업이 발견되면 `interrupted`로 기록하고 자동 재실행하지 않습니다. 저장·게시 도중 중단된 출력은 복구 확인 전까지 예약이 유지됩니다. `Acknowledge reviewed recovery`는 파일을 복원·삭제하지 않으며, 아직 dirty 상태인 패키지가 있으면 차단을 해제하지 않습니다. 출력과 staging을 확인하고 패키지 저장/변경 취소를 해결한 뒤 재시도하세요. 생성은 Undo 대상이 아닙니다.

## 검증

Automation의 `CubeShot.Editor`에서 기존 capture math/RHI/image/writer/view-model/MCP 테스트와 새 staged cancellation, cleanup failure, partial save, atomic PNG, 옵션 호환성 테스트를 실행합니다. `Writer.CancelPendingPixelWorker`는 실제 worker를 gate로 정지시켜 pending future 중 취소·파일 수명·게임 스레드 callback 없는 종료를 검증합니다. 공통 runtime 테스트도 플러그인에 포함됩니다. `Capture.RHI.OutputSweep`는 NPOT 257, Solid/Texture 배경 256, 최대 4096의 실제 GPU/worker 출력을 검사하고 `Saved/UnrealTools/V12/Images`에 PNG·메모리 기록을 남깁니다. `MCP.SourceChangeBeforeCommit`은 별도 메모리 fixture를 실제로 변경해 저장 전 중단을 검사합니다. `MCP.RealPlaySessionPolicy`는 `Saved/UnrealTools/V12/Projects` 아래의 유휴 검증 Editor에서만 실제 PIE/SIE를 실행하고 status/preflight/접수 차단 후 자체 세션을 종료합니다. 사용자 프로젝트에서는 이 테스트가 실행 상태를 변경하지 않습니다. 실제 RHI 촬영 검증은 DX12 렌더링 Editor에서 진행합니다.

`CubeShot.Editor.UI.LayoutScreenshots`는 실제 Slate 패널에 Engine Cube를 표시하고 1280×720 및 1920×1080 PNG를 `Saved/UnrealTools/V12/UI`에 저장합니다. 여섯 preview 버퍼의 유효 픽셀과 실제 Slate 이미지 영역에 메시가 그려지는지 검사하며, 결과가 있는 탭을 닫고 다시 열어 기록 보존도 확인합니다.

`UI.MultiSelectionAndWholePreflight`는 여러 행의 편집·제거·순서·전체 오류 검사와 접수 거부를 검증합니다. `MCP.RegistrationGeneration`은 off-thread에서 접수한 요청을 등록 해제/재등록 후 안전하게 차단하는지 검사합니다. `MCP.ExternalFunctionPropertyChangeBeforeCommit`은 별도 패키지의 실제 material function 변경을 저장 전에 검출합니다.

검증 결과는 프로젝트의 실행 기록을 기준으로 확인합니다. 고해상도 캡처·shader compilation·package save 중에는 UI/MCP 응답이 늦어질 수 있습니다. 4096 출력은 방향별 버퍼를 저장 후 해제하며 여섯 방향 HDR 버퍼를 동시에 유지하지 않습니다.

## 1.2 공통 서비스와 출력 snapshot

GUI와 MCP는 동일한 옵션·배치 계획·예약 검사 서비스를 사용합니다. GUI는 전체 검사와 덮어쓰기 확인, 필요한 패키지 checkout 및 엄격한 재검사 후에만 큐를 접수합니다. MCP와 실행 큐는 확인창을 열지 않습니다. Nanite는 fallback/LOD0 촬영을 유지하며 관련 경고 문구를 표시합니다. 자동 PNG는 저장 후 Texture2D를 다시 읽지 않고 해당 작업의 staging 이미지를 사용합니다.


## 공통 옵션 적용 규칙과 workspace

대상별 옵션은 독립적으로 보관합니다. `Edit defaults for new targets`에서 설정한 기본 옵션은 **이후 추가하는 대상에만** 복사됩니다. 기존 행을 선택하거나 편집 화면을 여는 동작은 옵션을 변경하지 않습니다.

- 행을 편집하면 해당 행만 변경합니다. 프리셋도 현재 편집 범위인 기본 옵션 또는 해당 행에만 적용합니다.
- `Apply current options to selected/checked rows`는 현재 편집 값을 체크한 행들에 복사합니다. 체크 선택과 편집·미리보기 대상은 별개이며, 실행은 전체 목록의 순서를 따릅니다.
- 행의 `Reset` 또는 `Reset to defaults`는 현재 기본 옵션을 명시적으로 복사합니다. 기본 옵션을 바꿨다는 이유로 기존 행이 바뀌지 않습니다.
- 검사·실행·일괄 적용·프리셋 저장 전에 필요한 입력을 확정합니다. 잘못된 입력을 마지막 유효 값으로 조용히 대체하지 않습니다.
- 기본 옵션에 오류가 있으면 새 대상 추가·기본값 복사를 차단하지만, 이미 유효하게 구성한 행은 실행할 수 있습니다. 대상 행에 오류가 있으면 전체 접수를 거부합니다.
- 프리셋이 삭제돼도 이미 복사된 옵션은 유지합니다. 재시도는 작업 기록의 원래 옵션을 사용하며 덮어쓰기를 다시 승인해야 합니다.

Workspace는 플러그인 모듈이 현재 Editor 세션 동안 소유합니다. 탭을 닫았다 열면 대상·옵션·편집 범위·이력 선택을 복원합니다. Editor를 재시작하면 미실행 workspace는 초기화하며, 기존 작업 기록과 저장한 사용자 프리셋은 그대로 유지합니다. UI의 미리보기·타이머·작업 관찰 연결은 탭 종료 시 정리합니다.

기본 옵션 편집 중에는 메시 미리보기를 비웁니다. 미리보기 완료 여부와 별개로 전체 대상 검사 결과에 따라 배치를 접수합니다. Auto Fit 계산 자체는 보관 옵션을 변경하지 않으며, 체크를 끌 때 현재 대상의 유효한 계산값을 수동 카메라 값으로 승계합니다. 프리셋에서 지정한 수동 값은 유지합니다.

오류가 있는 입력도 행 전환이나 편집기 재생성 후 표시용 초안으로 복원합니다. 예를 들어 텍스처를 아직 지정하지 않은 Texture 배경은 해당 선택과 오류를 함께 유지하며, 다른 컨트롤의 변경으로 이전 유효 배경으로 돌아가지 않습니다. 초기화·프리셋·일괄 적용으로 복구한 범위의 입력 오류만 해제합니다.

접수 순서와 프리셋 저장·삭제·가져오기·내보내기는 controller가 담당합니다. Slate는 확인창·파일 대화상자와 미리보기 리소스를 관리하며, 접수 중에는 같은 snapshot을 검사·checkout·접수에 전달합니다.

프리셋 확인창이 열린 동안 탭이나 모듈이 종료되면 복귀한 콜백은 닫힌 UI와 미리보기 리소스를 다시 만들지 않습니다. 행의 수정 표시 `*`는 controller의 표시 revision에 따라 갱신합니다. 이력 결과는 작업 ID와 작업 revision으로 최대 20개를 캐시하며, 다른 작업의 진행만으로 선택한 과거 결과를 다시 조회·직렬화하지 않습니다. 명시적인 새로고침과 탭 재개방은 상태를 다시 확인합니다.

작업 서비스가 재생성되면 주소·revision이 같아도 인스턴스 세대로 구분해 캐시와 표시 작업 pin을 새로 연결합니다. 갱신 전에 탭이나 모듈을 닫아도 이전 세대의 pin 해제가 새 서비스의 다른 관찰자 pin을 감소시키지 않습니다. 이 세대 번호는 작업 기록이나 MCP 응답에 저장하지 않습니다.

현재 변경의 검증 범위와 Unreal 후속 확인 항목은 [workspace 검증 기록](../MCP/WORKSPACE_VALIDATION.md)에 있습니다.

## 2.0 레거시 제거

MCP 도구는 8개이며 촬영은 `cubeshot_job_start`로 접수합니다. 검사는 `mesh_paths`, `options`, `overrides`를 사용합니다. 단독 PNG 내보내기도 기본 `overwrite=false`이며 기존 파일 교체에는 명시적인 `true`가 필요합니다. 구형 단일 촬영 호출, 이전 Python 호출 어댑터와 설정·기록 변환 경로를 제거했습니다. ViewModel은 미리보기·표시 검증만 담당하며 저장 실행 판단은 controller에서 수행합니다. 현재 Editor 빌드·GPU 출력 검증은 아직 실행하지 않았습니다.
