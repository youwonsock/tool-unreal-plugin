# FBX_Exporter

Unreal Engine 5.8용 Editor 전용 플러그인입니다. Content Browser에서 선택한 Static Mesh를 FBX와 PBR 텍스처 세트로 내보내 Unity 등 외부 툴에서 후처리할 수 있게 합니다.

## 주요 기능

- 여러 Static Mesh를 한 번에 내보냅니다.
- 선택한 메시마다 별도의 폴더와 FBX 파일을 생성합니다.
- 도킹 탭에서 `Base Color`, `Normal`, `Mask`, `Occlusion`, `Emission`, `Opacity/Opacity Mask`를 개별 선택할 수 있습니다. `Select All`/`Clear All`로 한 번에 변경할 수 있습니다.
- 선택한 맵만 베이크·직접 재사용·FBX 연결·JSON에 기록합니다. 모든 맵을 해제하면 텍스처 참조가 없는 geometry-only FBX만 생성합니다.
- `Embed selected textures in FBX`를 켜면 외부 PNG와 동일한 바이트를 FBX 미디어에 임베드합니다. 끄면 `Textures/...` 상대 경로로 연결합니다.
- PBR 베이크 해상도 `512`, `1024`, `2048`, `4096`을 지원하며 기본값은 `2048`입니다.
- 머티리얼에 직접 연결된 원본 Texture2D를 사용할 수 있는 경우 재사용할 수 있습니다.
- 마지막 출력 폴더를 프로젝트 설정에 저장하고 다음 실행 때 다시 사용합니다.
- FBX SDK 2020.2 후처리로 LOD0 polygon material index를 검증해 머티리얼과 텍스처를 연결합니다.
- 기존 메시 폴더를 덮어쓸 때 사용자 파일을 포함한 폴더 전체가 교체됨을 안내하며, 오류가 발생하면 기존 폴더와 백업을 보존하도록 임시 스테이징을 사용합니다.
- 플러그인 자체에 MCP 도구를 포함합니다. 다른 프로젝트의 `Plugins/FBX_Exporter`로 옮겨도 해당 프로젝트의 메시 검색·사전 검사·추출을 사용할 수 있습니다. Haven, CubeShot, 프로젝트별 Python 스크립트에 의존하지 않습니다.

Unreal 에셋 자체는 수정하지 않습니다. Unity 머티리얼, Unity 메타데이터, 머티리얼 설정 파일은 생성하지 않으므로 외부 툴에서 직접 구성해야 합니다.

## 출력 결과

출력 루트 아래에 메시 이름별 폴더가 생성됩니다. 파일명에 사용할 수 없는 문자는 `_`로 바뀝니다. 슬롯명이 대소문자 무시 기준으로 충돌하면 `_Slot<index>`가 추가됩니다.

```text
<출력 루트>/
└─ <메시 이름>/
   ├─ <메시 이름>.fbx
   ├─ <메시 이름>_Materials.json   # 하나 이상의 맵을 선택한 경우
   └─ Textures/                    # 하나 이상의 맵을 선택한 경우
      ├─ <머티리얼 슬롯>_BaseColor.png
      ├─ <머티리얼 슬롯>_Normal.png
      ├─ <머티리얼 슬롯>_Mask.png
      ├─ <머티리얼 슬롯>_Occlusion.png
      ├─ <머티리얼 슬롯>_Emission.png
      └─ <머티리얼 슬롯>_Opacity.png
```

메시가 Masked 블렌드 모드이면 마지막 파일 이름은 `OpacityMask.png`가 됩니다. `BaseColor`의 알파 채널에도 opacity 값이 기록됩니다.

임베드를 켜면 PNG 파일 데이터가 FBX 내부에도 저장되며, 외부 `Textures` 폴더도 함께 생성됩니다. 외부 PNG는 임베드를 읽지 못하는 프로그램에서 사용하거나 직접 재연결·편집할 때 쓸 수 있습니다. 임베드를 끈 경우에는 FBX와 `Textures` 폴더를 함께 옮겨야 합니다. JSON은 맵·색 공간·재질 규칙을 기록하는 메타데이터이며, 일반적인 3D 프로그램이 자동으로 읽는 텍스처 연결 파일은 아닙니다. 실제 연결 정보는 FBX 안에 있습니다. 여기서 DCC(Digital Content Creation)는 Blender, Maya, 3ds Max 같은 3D 제작 프로그램을 뜻합니다.

`Mask.png`의 채널은 다음과 같이 구성됩니다.

| 채널 | 값 |
| --- | --- |
| R | Metallic |
| G | Ambient Occlusion |
| B | `255` 고정값 |
| A | `1 - Roughness`로 계산한 Smoothness |

`Opacity`/`OpacityMask` PNG는 RGB와 A 모두 `1=불투명` 값입니다. `BaseColor`를 선택하면 standalone Opacity 맵을 선택하지 않아도 BaseColor 알파에 Opacity 또는 OpacityMask가 유지됩니다. Emission 베이크의 실제 스케일은 FBX `EmissiveFactor`, `UE_PBR_EmissionScale`, JSON의 `emissionScale`에 기록됩니다.

## Material JSON (schemaVersion 1)

맵을 하나 이상 선택하면 `<메시 이름>_Materials.json`을 생성합니다. 최상위에는 `sourceStaticMesh`, `fbxFile`, `bakeResolution`, `selectedMaps`, `texturesEmbedded`, `maskLayout`, `opacityConvention`, `materials`가 들어갑니다. 각 `materials` 항목은 `fbxMaterialIndex`, `sourceSlots[{slotIndex,slotName}]`, nullable `materialPath`, `blendMode`, `baseColorAlpha`, `emissionScale`, `maps[{map,file,colorSpace}]`를 기록합니다. DCC가 FBX 투명도 표준 속성을 다르게 해석할 때는 JSON의 `opacityConvention`과 FBX 머티리얼의 `UE_PBR_OpacityConvention`(`1=Opaque`)을 권위 정보로 사용하십시오.

## 설치 및 프로젝트 연결

호환되는 UE 5.8 환경에서 플러그인 폴더 전체를 다음처럼 배치합니다. MCP를 제공하는 엔진의 `ModelContextProtocol` 플러그인이 필요하며, 의존성은 `FBX_Exporter.uplugin`에 선언되어 자동으로 함께 활성화됩니다. 이 엔진 플러그인과 C++ API가 없는 버전에서는 먼저 호환 작업이 필요합니다.

```text
<다른 프로젝트>/Plugins/FBX_Exporter/
├─ FBX_Exporter.uplugin
├─ Source/
├─ Config/
├─ MCP/
└─ README.md
```

프로젝트 파일을 생성하고 대상 엔진으로 Editor를 빌드한 뒤 플러그인을 활성화합니다. 다른 엔진 버전에서 생성한 `Binaries`/`Intermediate`를 그대로 사용하는 것은 지원하지 않습니다. 배포용 패키지는 해당 엔진의 `RunUAT BuildPlugin`으로 만들 수 있으며, `Config/FilterPlugin.ini`가 README와 MCP 명세·설치 도구·공통 런타임·CLI를 패키지에 포함합니다.

이 저장소의 Haven 프로젝트는 `.uproject`의 `AdditionalPluginDirectories: ["tool-unreal-plugin"]`로 현재 폴더를 참조합니다. 다른 프로젝트의 `Plugins` 아래에 직접 설치할 때는 이 설정이 필요 없습니다.

## MCP 사용

MCP 도구 구현·등록·입력 검사는 이 플러그인 안에 있습니다. 통신 서버는 엔진의 `ModelContextProtocol`을 공유하므로 다른 플러그인을 복사할 필요가 없습니다. 여러 플러그인을 함께 설치하면 각자 자신의 이름으로 도구를 등록합니다. FBX 플러그인을 종료할 때는 해당 도구만 해제하고 공용 서버는 종료하지 않습니다.

| 도구 | 기능 |
| --- | --- |
| `fbx_exporter_status` | 현재 프로젝트, 출력 기본 경로, 실행 가능 여부 확인 |
| `fbx_exporter_find_meshes` | 메시 이름 검색 및 정확한 에셋 경로 반환 |
| `fbx_exporter_preflight` | LOD0/UV0·출력 경로·덮어쓰기 필요 여부 검사; 파일 생성 없음 |
| `fbx_exporter_job_start` | `request_id`와 `mesh_paths`로 FBX·선택 PNG·JSON 추출 접수 |
| `fbx_exporter_job_status` / `fbx_exporter_job_control` | 조회·취소·복구 확인 |
| `fbx_exporter_profiles` | 프리셋 조회·저장·삭제 |

새 프로젝트에서는 Editor를 닫고 `python Plugins/FBX_Exporter/MCP/setup_mcp.py`를 실행합니다. 활성 MCP 플러그인을 검색해 `unreal_tools` 연결을 구성하며 FBX 2.0만 설치하면 7개, CubeShot 2.0도 설치하면 15개 도구를 제공합니다. 기본 주소는 `http://127.0.0.1:8000/mcp`입니다. Editor 재시작 후 Codex의 프로젝트 MCP 설정을 다시 로드합니다. [MCP 설치·호출 안내](MCP/README.md)에 설정·백업·CLI와 요청 예제가 있습니다.

MCP 기본 출력은 **현재 프로젝트**의 `Saved/FBX_Exporter/Exports`입니다. `output_root`에 절대경로를 주면 변경할 수 있습니다. 해상도 기본값은 2048, 모든 맵 선택, 임베드 해제입니다. `maps: []`는 FBX만 생성합니다. 기존 메시 출력 폴더는 `overwrite: true`가 명시된 경우에만 사용자 파일까지 포함해 전체 교체됩니다. MCP는 확인창을 열지 않으며 결과의 `success`, `files`, `warnings`, `errors`를 반환합니다.

## 도킹 탭과 작업 흐름

`Tools → FBX Exporter`에서 독립 도킹 탭을 엽니다. Content Browser의 기존 `Export Static Meshes + Textures` 메뉴는 선택 메시를 이 탭에 추가합니다. 탭을 닫아도 접수한 작업은 계속되며 다시 열면 서비스의 작업 기록과 결과를 표시합니다.

1. Content Browser 선택 또는 다중 드래그 앤 드롭으로 메시를 추가합니다. 정확한 에셋 경로로 중복을 제거하고 행의 Up/Down/Remove로 순서를 변경합니다. 행 체크박스로 여러 대상을 선택하고 전역 옵션을 선택 대상에 적용하거나 선택 대상만 제거할 수 있습니다. 중복 추가는 선택과 순서를 유지합니다.
2. `PBR`(2048, 모든 맵, 직접 재사용 켬, 임베드 끔) 또는 `Geometry Only`를 선택합니다. geometry-only에서는 해상도와 텍스처 관련 옵션이 비활성화됩니다.
3. 출력 루트, 맵, 임베드와 경로 레이아웃을 정합니다. 행의 `Edit options`를 선택하면 같은 옵션 컨트롤에서 그 대상만 편집합니다. JSON override는 접힌 Advanced 영역에서 사용할 수 있습니다. `Preserve asset path`는 `/Game/A/SM_X.SM_X`를 `<출력 루트>/Game/A/SM_X`로 매핑합니다.
4. `Inspect output plan`에서 모든 대상의 구체적인 FBX·metadata·슬롯 PNG 파일명, 교체될 전체 폴더와 기존 파일, 경고와 충돌을 확인합니다. 파일 상세는 접힌 스크롤 영역에 표시합니다. 실행 전 검사는 모든 대상을 처리한 뒤 오류를 모아 표시합니다. 검사 결과는 실행 결과와 별도로 유지됩니다.
5. 기존 폴더를 교체할 때만 승인 체크박스를 선택하고 `Queue export`를 누릅니다. 이 승인은 프리셋에 저장하지 않으며 접수 후 해제됩니다.
6. 결과에서 출력 열기, 경로 복사, 첫 PNG 미리보기, 결과 JSON 복사, 실패 재시도와 미실행 항목 실행을 사용할 수 있습니다. 기록 선택에서 이전 작업을 열 수 있으며 기본 결과 영역에 대상 상태·파일·경고·오류를 표시합니다. 재시도는 새 작업이며 덮어쓰기를 다시 확인합니다.

UI의 출력 루트 기본값은 기존 프로젝트 설정 또는 `<프로젝트>/Exports/StaticMeshTextures`입니다. MCP 기본값 `Saved/FBX_Exporter/Exports`와 구분되며 UI가 자신의 경로를 서비스에 명시적으로 전달합니다.

프로젝트별 사용자 프리셋은 `Saved/FBX_Exporter/Profiles`에 JSON으로 저장합니다. 기본 프리셋은 변경할 수 없습니다. 새 ID·표시 이름으로 저장하거나 사용자 프리셋을 삭제하고, JSON 파일 대화상자로 가져오기/내보내기를 지원합니다. 같은 사용자 ID를 저장하거나 가져올 때 교체 승인을 확인합니다. 출력 덮어쓰기 승인은 저장하지 않습니다. 선택한 사용자 프리셋이 삭제되면 새로고침·탭 재개방 시 선택 ID만 PBR로 돌아가며 편집한 옵션과 출력 경로는 유지합니다.

## 단계 실행과 취소

작업은 검사 → geometry → 슬롯/맵 의존 그룹 → 파일 구성 → 검증 → 게시 → 정리로 실행합니다. 각 단계 사이에 Editor tick을 양보하며 PNG 쓰기는 소유권이 분리된 픽셀과 경로만 작업 스레드로 넘깁니다. UObject·렌더링·FBX 호출은 게임 스레드에 유지하며 동시에 하나의 FBX 작업만 실행합니다. 준비된 PNG를 저장한 뒤 픽셀 버퍼를 해제해 전체 슬롯·맵의 float 버퍼가 누적되지 않게 합니다.

GPU 베이크와 FBX SDK 호출은 게임 스레드에서 수행되며 그 호출 내부에서는 응답과 취소 처리가 지연될 수 있습니다. `Request cancel`은 안전한 경계에 적용합니다. 게시를 시작한 현재 메시의 출력은 마무리하며 마지막 메시가 완료됐다면 취소 요청 이후에도 작업이 성공할 수 있습니다. 첫 실패는 후속 대상을 `skipped`로 처리합니다. PIE/SIE가 시작되면 다음 안전한 경계에서 중단합니다.

에셋 경로와 해석된 옵션은 접수 시 고정합니다. 메시·머티리얼·사용 텍스처는 실행 중 강하게 참조하고 변경 감지 시 게시 전 `source_changed`로 실패합니다. Editor 재시작 후 미완료 작업은 `interrupted`로 표시하고 자동으로 재실행하지 않습니다. 작업 기록은 `Saved/FBX_Exporter/Jobs`입니다.

## 출력 보호와 복구

기존 폴더 전체를 교체할 때는 신규 staging을 만든 뒤 journal에 원본·staging·backup 경로와 파일 목록을 기록합니다. 기존 파일 이동과 신규 파일 게시 후 모든 신규 바이트를 staging과 비교하고 commit을 기록합니다.

- commit 이전 실패는 완전한 백업으로 복원을 시도합니다. 복원 실패는 staging·backup·journal을 보존하고 해당 출력의 재사용을 차단합니다.
- commit 이후 백업 정리에 실패하면 성공과 별도 경고로 처리하고 신규 출력을 보존합니다. 일부 삭제된 백업으로 되돌리지 않습니다.
- 강제 종료 중 게시가 미완료라면 재시작 후 복구 필요 상태로 유지됩니다. 남은 파일을 확인한 뒤 `Acknowledge inspected recovery` 또는 `fbx_exporter_job_control`의 `acknowledge_recovery`로 해당 작업의 출력 차단을 해제합니다. 이 동작은 파일을 복원·삭제하지 않습니다.

복구 journal은 `Saved/FBX_Exporter/Recovery`에 저장됩니다. 실제 백업은 출력 루트의 고유 staging 옆 `_Previous` 디렉터리에 유지됩니다. 경고·결과의 경로를 확인하고 자동 삭제를 기대하지 마십시오.

## 텍스처와 FBX 동작

- 메시의 유효한 LOD0 렌더 데이터가 필요하며 텍스처에는 UV0와 렌더링 가능한 Editor가 필요합니다. `-nullrhi`에서는 geometry-only만 허용합니다.
- 직접 재사용은 해당 머티리얼 속성이 엔진에서 활성 상태일 때만 허용합니다. 예를 들어 기본 volumetric translucent 머티리얼의 Metallic/Roughness는 엔진 기본값으로 베이크되며, 비활성 입력에 연결된 원본 텍스처가 이 규칙을 우회하지 않습니다. 활성 직접 재사용 소스는 Texture2D의 SRGB/legacy gamma 설정을 반영해 RGBA32F 선형 데이터로 읽습니다. 리사이즈·채널 선택과 HDR normalization 이후 최종 PNG에서만 8-bit로 변환합니다.
- 직접 HDR Emission도 최대 RGB와 1 이상인 `emissionScale`로 정규화합니다. 복원은 sRGB PNG의 RGB를 선형으로 decode한 뒤 scale을 곱합니다. 베이크와 직접 경로 모두 FBX/JSON에 scale을 기록합니다.
- FBX는 바이너리 형식, LOD0, Vertex Color를 포함하며 Collision/Source Mesh는 포함하지 않습니다. 누락·미사용 머티리얼 슬롯은 경고를 반환합니다.
- Runtime Virtual Texture, Substrate Front Material, Custom HLSL은 경고를 반환할 수 있으며 외부 툴의 원본 셰이더로 변환하지 않습니다.
- 파일명은 Windows 예약 이름과 끝의 dot/space를 처리합니다. 정규화된 대소문자 경로가 겹치면 배치를 거부합니다.

## 검증

`FBX_Exporter.Editor` Automation Tests는 파일명·Mask·맵 의존성·현재 출력 설정 조회·게시 복원·부분 백업 삭제 후 commit 보존·복원 실패 journal 차단/해제·sRGB ramp와 float HDR source·비활성 속성의 직접 재사용 거부·MCP 등록/스키마·엔진 Cube geometry 출력·덮어쓰기 보호를 검증합니다. 공통 작업 런타임 테스트는 접수 멱등성·취소·중단 복구·프리셋과 단계 실행 계약을 검증합니다. UI 상태 검증은 다중 선택·중복 유지·선택 대상 옵션 스냅샷·선택 제거 순서를 검증하며, 지연 MCP 등록 세대 검증은 unregister/re-register 이후 이전 요청이 EDITOR_INTERRUPTED로 한 번만 완료되는지 확인합니다. `Integration.GPUAndEmbeddedRoundTrip`은 임시 두 슬롯 메시에서 실제 GPU·Normal/Opacity/Mask/HDR 픽셀·선택 맵·FBX SDK 재import·임베드 PNG 바이트와 4096 처리를 검증하고 메모리 수치를 기록합니다. `Integration.WorkspaceScreenshots`는 실제 작업 공간을 사용하는 테스트 소유 창에서 두 화면 크기의 내용 PNG를 저장하며 최소 내용 크기를 검증합니다. 사용자 Editor 창의 크기를 바꾸지 않습니다.

Session Frontend의 Automation Tests에서 이 접두사를 실행할 수 있습니다. 렌더링 테스트와 GPU 베이크, 재import·임베드, 실제 독립 패키지 검증은 별도 테스트 프로젝트에서 수행하며, 수행 결과는 프로젝트 검증 기록에 남깁니다. 기능 설명 자체를 검증 실행 결과로 간주하지 않습니다.

`Integration.SourceChanged`는 Editor 프레임마다 실제 단계 runner를 전진시키고 geometry 이후 머티리얼 의존성을 변경하여 `source_changed`와 게시 차단을 검증합니다. `Integration.GuardedPublishingCrash`는 일반 테스트 실행에서 비활성입니다. 실제 프로세스 중단 검증은 별도 소유 fixture 프로젝트와 두 명시적 테스트 플래그가 있을 때만 원본 첫 파일을 백업으로 옮긴 직후 journal/checkpoint를 저장하고 종료 코드 86으로 프로세스를 중단합니다. 재시작 시 차단과 파일을 바꾸지 않는 명시적 복구 승인을 외부 검증으로 확인합니다.

## 1.2 공통 서비스

GUI와 MCP가 동일한 옵션 정규화·전체 배치 계획·출력 예약 검사를 사용하며 실행기는 기존 작업 큐를 공유합니다. 단일·비동기 MCP 호출도 원래 오류 코드를 전달합니다. MaterialBaking의 기존 linear 모드를 베이크 종료 후 복원하며 HDR 모드를 변경하지 않습니다. Material JSON 및 FBX 연결 규칙은 유지합니다.


## 공통 옵션 적용 규칙과 workspace

대상별 옵션은 독립적으로 보관합니다. `Edit defaults for new targets`에서 설정한 기본 옵션은 **이후 추가하는 대상에만** 복사됩니다. 기존 행을 선택하거나 편집 화면을 여는 동작은 옵션을 변경하지 않습니다.

- 행을 편집하면 해당 행만 변경합니다. 프리셋도 현재 편집 범위인 기본 옵션 또는 해당 행에만 적용합니다.
- `Apply current options to selected/checked rows`는 현재 편집 값을 체크한 행들에 복사합니다. 체크 선택과 편집·미리보기 대상은 별개이며, 실행은 전체 목록의 순서를 따릅니다.
- 행의 `Reset` 또는 `Reset to defaults`는 현재 기본 옵션을 명시적으로 복사합니다. 기본 옵션을 바꿨다는 이유로 기존 행이 바뀌지 않습니다.
- 검사·실행·일괄 적용·프리셋 저장 전에 필요한 입력을 확정합니다. 잘못된 입력을 마지막 유효 값으로 조용히 대체하지 않습니다.
- 기본 옵션에 오류가 있으면 새 대상 추가·기본값 복사를 차단하지만, 이미 유효하게 구성한 행은 실행할 수 있습니다. 대상 행에 오류가 있으면 전체 접수를 거부합니다.
- 프리셋이 삭제돼도 이미 복사된 옵션은 유지합니다. 재시도는 작업 기록의 원래 옵션을 사용하며 덮어쓰기를 다시 승인해야 합니다.

Workspace는 플러그인 모듈이 현재 Editor 세션 동안 소유합니다. 탭을 닫았다 열면 대상·옵션·편집 범위·이력 선택을 복원합니다. Editor를 재시작하면 미실행 workspace는 초기화하며, 기존 작업 기록과 저장한 사용자 프리셋은 그대로 유지합니다. UI의 미리보기·타이머·작업 관찰 연결은 탭 종료 시 정리합니다.

고급 JSON은 현재 행의 **부분 수정**입니다. 예를 들어 `{"maps":[]}`는 해상도·출력 경로를 유지하고 맵만 해제합니다. `{}`는 기존 값을 유지하며 기본값 복귀는 `Reset to defaults`를 사용합니다. 잘못된 JSON 원문은 수정하거나 명시적으로 덮어쓸 때까지 보존하고, 포커스를 옮기거나 다른 컨트롤을 조작해도 오류를 숨기지 않습니다. FBX 프리셋 적용 시 현재 편집 범위의 출력 루트는 유지합니다.

초기화·프리셋·일괄 적용으로 복구한 범위의 입력 오류는 해제하고, 다른 범위의 오류는 유지합니다. 대상이나 옵션을 바꾸면 출력 계획의 요약과 상세 본문을 함께 재검사 필요로 표시합니다. 프리셋 저장·삭제·가져오기·내보내기는 controller가 수행하며 Slate는 확인창과 파일 대화상자를 표시합니다.

잘못된 출력 경로나 JSON 초안이 남아 있어도 맵의 연속 선택·해제는 현재 초안에 누적합니다. 출력 경로만 수정해도 누적한 맵 선택을 유지하며, 먼저 입력한 유효한 JSON이 나중 맵 선택을 되돌리지 않습니다. 잘못된 JSON 원문과 오류는 별도로 수정할 때까지 보존합니다. `Copy result JSON`은 현재 이력의 캐시를 그대로 복사하고, 작업 이력이 없으면 빈 문자열을 복사합니다.

현재 변경의 검증 범위와 Unreal 후속 확인 항목은 [workspace 검증 기록](../MCP/WORKSPACE_VALIDATION.md)에 있습니다.

복구 진단 재조회와 진단 이력 페이지 전환은 controller 명령 하나로 처리합니다. 행의 수정 표시 `*`는 옵션 변경·초기화 후 기존 위젯에서 갱신합니다. 결과는 작업 ID와 작업 revision으로 최대 20개를 캐시하며, 다른 작업이 진행해도 선택한 과거 결과를 다시 조회·직렬화하지 않습니다. 명시적인 새로고침과 탭 재개방은 상태를 다시 확인합니다.

작업 서비스가 재생성되면 주소·revision이 같아도 인스턴스 세대로 구분해 캐시와 표시 작업 pin을 새로 연결합니다. 갱신 전에 탭이나 모듈을 닫아도 이전 세대의 pin 해제가 새 서비스의 다른 관찰자 pin을 감소시키지 않습니다. 이 세대 번호는 작업 기록이나 MCP 응답에 저장하지 않습니다.

## 2.0 레거시 제거

MCP 검사와 접수는 `mesh_paths`, `options`, `overrides` 형식입니다. 이전 프로젝트 설정에서 출력 경로를 가져오지 않으며 현재 `/Script/FBX_Exporter.FBX_ExporterSettings`만 읽습니다. 구형 단일 추출 호출과 Python·설정·기록 변환 경로를 제거했습니다. 이전 완료 기록은 자동 보정하지 않고 미지원 형식이면 원본을 보존한 진단으로 표시합니다. 현재 Editor 빌드와 실제 출력 검증은 아직 실행하지 않았습니다.
