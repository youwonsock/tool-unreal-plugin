# FBX Exporter MCP 1.1

UE 5.8 / Win64 Editor / Python 3.11 이상을 지원합니다. 엔진 `ModelContextProtocol` HTTP 서버를 공유하며 별도 서버를 실행하지 않습니다. 이 플러그인 폴더 전체를 복사하면 C++ 도구·공통 Python 런타임·명세·설치/호출 진입점이 함께 이동합니다.

## 설치와 연결

1. `<프로젝트>/Plugins/FBX_Exporter`로 플러그인을 복사하고 해당 UE 5.8 Editor를 빌드합니다.
2. Editor를 닫고 `python Plugins/FBX_Exporter/MCP/setup_mcp.py --dry-run`으로 설정을 확인한 뒤 dry-run 없이 실행합니다. 외부 위치에서 실행하면 `--project <전체경로.uproject>`를 지정합니다.
3. Editor를 다시 열고 Codex 프로젝트 MCP 설정을 다시 불러옵니다. 연결 이름은 `unreal_tools`입니다.
4. `fbx_exporter_status`의 전체 `project_file`과 `api_version`, `capabilities`를 확인합니다.

설치 명령은 프로젝트 `Plugins`와 `AdditionalPluginDirectories`의 활성 manifest를 발견합니다. 최신 FBX 단독은 8개, 최신 CubeShot과 함께 설치하면 17개 도구를 설정합니다. 구버전 플러그인을 함께 설치하면 그 manifest에 선언된 도구만 설정합니다. 단독 CLI는 FBX 도구만 호출하며 통합 설치는 다른 활성 도구를 함께 유지합니다.

기본 HTTP 주소는 `http://127.0.0.1:8000/mcp`, 시작 제한 20초, 도구 호출 제한 180초입니다. 기존 주소·타임아웃을 보존합니다. `--port`, `--url-path`, `--dry-run`을 지원하며 각 프로젝트는 다른 포트를 사용합니다. 전체 구형 관리 허용 목록(4개/9개)은 신규 명세로 확장합니다. 사용자 부분 제한·거부·비활성화는 자동 변경하지 않습니다. 생성 기록과 목록이 다르면 원인을 표시하고 검토 후 명시적인 설정 교체를 사용합니다.

```powershell
python -m pip install -r Plugins/FBX_Exporter/MCP/requirements.txt
python Plugins/FBX_Exporter/MCP/call_mcp.py --list-tools
python Plugins/FBX_Exporter/MCP/call_mcp.py fbx_exporter_status
python Plugins/FBX_Exporter/MCP/call_mcp.py fbx_exporter_preflight --arguments-file export-arguments.json
```

매 연결마다 status로 대상 `.uproject` 전체 경로를 확인하고 Codex 허용/거부 목록을 적용합니다. 일반 `--batch-file`은 순차 호출이며 오류 후 중단합니다. 기록 위치는 `Saved/FBX_Exporter/MCPCalls`입니다. 기록 저장 오류가 원래 도구 결과나 실패를 덮지 않습니다.

## 도구

| 도구 | 입력과 결과 |
| --- | --- |
| `fbx_exporter_status` | 프로젝트, 버전/capabilities, 실제 renderer 가능 여부, 차단 이유, 활성 작업 |
| `fbx_exporter_find_meshes` | `query`, 기본 `/Game`인 `root`, `limit`(1–200) → 정확한 object path |
| `fbx_exporter_preflight` | `mesh_path` 또는 `mesh_paths`와 추출 옵션 → 단일 기존 응답 또는 대상 배열·전체 검사 결과, planned_files/replacement_files와 전체 폴더 교체 범위 |
| `fbx_exporter_export` | `mesh_path`, 옵션, 기본 false인 `overwrite` → 완료 시 기존 files/paths/warnings/errors 응답 |
| `fbx_exporter_profiles` | `action=list/get/save/delete`, `id` 또는 `profile`; save 기존 사용자 ID 교체에는 명시적인 `overwrite` 필요 |
| `fbx_exporter_job_start` | `request_id`, 순서 있는 `mesh_paths`, 선택 `profile_id`, `options`, 경로별 `overrides`, `overwrite` → 접수된 job ID |
| `fbx_exporter_job_status` | `job_id` 또는 `request_id` → 현재 상태·대상별 단계/결과/오류·진행 |
| `fbx_exporter_job_control` | `job_id` 또는 `request_id` 중 하나, `action=cancel/acknowledge_recovery` → 제어 후 현재 상태 |

모든 도구는 input/output JSON schema를 게시하고 서버에서도 인수를 검사합니다. 오류는 MCP `isError=true`, `structuredContent.success=false`와 오류 코드로 반환합니다. Job 조회/제어의 `success`는 조회·제어 성공이며 실제 작업 성공은 `state=succeeded`로 확인합니다.

## 추출 옵션과 기본값

| 옵션 | 기본값 / 의미 |
| --- | --- |
| `mesh_path` | 단일 기존 도구의 정확한 Static Mesh object path |
| `output_root` | MCP 기본 `<현재프로젝트>/Saved/FBX_Exporter/Exports`; 지정 시 절대 디렉터리 |
| `output_layout` | `mesh_name`; `asset_path`는 `/Game/A/SM_X`를 `<루트>/Game/A/SM_X`로 매핑 |
| `resolution` | 2048; 512/1024/2048/4096 |
| `maps` | 생략 시 BaseColor/Normal/Mask/Occlusion/Emission/Opacity 모두, `[]`는 geometry-only |
| `embed_textures` | false; 선택 PNG와 같은 바이트를 FBX 안에도 포함하고 외부 PNG 유지 |
| `reuse_direct_textures` | true; 조건에 맞는 직접 소스를 float 선형 처리로 재사용 |
| `overwrite` | export/job_start만 허용, false; true는 메시 출력 폴더 전체를 사용자 파일까지 교체 |

`mesh_path`와 `mesh_paths`를 동시에 지정하지 않습니다. 단일 preflight의 기존 응답은 유지됩니다. 배치는 `items`, `can_export`를 반환하고 모든 항목을 검사합니다. preflight는 출력을 만들지 않습니다. 실제 베이크·권한·게시 가능 여부는 실행에서 다시 검사합니다.

`PBR` 기본 프리셋 ID는 `pbr`, geometry-only ID는 `geometry_only`입니다. 사용자 프리셋은 아래 형태이며 overwrite 승인을 옵션 안에 넣을 수 없습니다.

```json
{"schema_version":1,"provider_id":"fbx_exporter","id":"fast_geometry","display_name":"Fast geometry","options":{"maps":[],"output_layout":"asset_path"}}
```

`profiles.save`에는 이 객체를 `profile`로 전달합니다. `get/delete`에는 `id`를 전달하며 기본 프리셋은 수정할 수 없습니다. 프리셋 JSON은 프로젝트의 `Saved/FBX_Exporter/Profiles`에 저장됩니다.

## 작업 접수·대기·취소·복구

```json
{
  "request_id": "a1447d7d-591f-48c8-aad7-15e83d0d79c1",
  "mesh_paths": ["/Engine/BasicShapes/Cube.Cube"],
  "profile_id": "geometry_only",
  "options": {"output_layout":"asset_path"},
  "overrides": {},
  "overwrite": false
}
```

옵션 우선순위는 대상 override → 명시적 공통 옵션 → 프리셋 → API 기본값입니다. 목록 밖 override와 중복 대상은 거부합니다. 같은 UUID·같은 요청은 기존 작업을 반환하며 같은 UUID·다른 요청은 오류입니다. 접수 후 프리셋 변경은 이미 고정한 옵션에 영향을 주지 않습니다. 신규 접수 시 PIE/SIE를 거부하며 선택 맵이 있으면 렌더링 가능 여부도 확인합니다. geometry-only는 NullRHI에서 사용할 수 있습니다. 이미 접수된 request ID의 재조회는 실행 정책이 달라져도 같은 기록을 반환합니다.

```powershell
python Plugins/FBX_Exporter/MCP/call_mcp.py fbx_exporter_job_start --arguments-file job.json --wait
python Plugins/FBX_Exporter/MCP/call_mcp.py fbx_exporter_job_status --arguments '{"job_id":"<returned-id>"}' --wait
```

`--wait`는 단일 작업 접수·조회에만 허용합니다. 기본 전체 대기는 30분, 폴링은 1초이며 `--wait-timeout`, `--poll-interval`로 변경합니다. 종료 코드 0은 실제 `succeeded`만 의미합니다. 접수 전에 request ID를 보존하며 응답 유실 시 해당 ID로 조회하고 자동 재제출하지 않습니다. 전체 대기 만료·연결 실패가 작업을 취소하지 않습니다.

상태는 queued/running/succeeded/failed/cancelled/interrupted입니다. 첫 오류 이후 대상은 skipped가 되며 완료 출력은 보존됩니다. GPU 베이크/FBX SDK 호출 내부에서는 조회·취소가 지연될 수 있습니다. 취소는 안전한 경계에서 적용하고 게시 중인 메시를 마무리합니다. 마지막 메시 완료와 취소 요청이 겹치면 실제 작업은 succeeded일 수 있습니다. PIE/SIE 진입 시 다음 안전한 경계에서 중단합니다.

작업·프리셋·복구 기록은 각각 `Saved/FBX_Exporter/Jobs`, `Profiles`, `Recovery`입니다. Editor 재시작 후 미완료 작업은 interrupted이며 자동 재실행하지 않습니다. 게시 전 source_changed를 검사합니다. commit 이전 실패는 완전한 백업으로 복원을 시도하고, commit 후 백업 정리 실패는 신규 출력과 warning을 보존합니다. 미완료 게시 output은 복구 검사·명시적인 acknowledge_recovery 전까지 재사용을 막습니다. 제어 도구는 자기 작업에 기록된 journal만 승인하며 파일 복원·삭제를 수행하지 않습니다.

기존 `fbx_exporter_export`는 동일한 단계 서비스를 완료 후 응답하는 어댑터입니다. `RunAsync`는 단계를 tick으로 실행하고 직접 `Run`은 inline으로 실행합니다. 대기/실행 작업이 있으면 busy를 반환합니다. MCP는 확인 대화상자를 열지 않습니다.

## 배포와 검증

플러그인의 `MCP/manifest.json`과 공통 runtime 배포본, `Source/.../Private/UnrealToolsRuntime.h`도 함께 패키징합니다. 상위 폴더가 없는 배포에서도 설치·호출할 수 있습니다. 원본 저장소에서는 공통 코드를 수정하고 `python Scripts/sync_runtime.py`로 동기화하며 `--check`로 불일치를 검출합니다.

Python 설정/호출 테스트는 `python -m unittest discover -s MCP/tests -v`, native 테스트는 `FBX_Exporter.Editor`입니다. 도구 수만으로 실제 출력·복구·호환성 검증을 대신하지 않으며 실제 GPU 베이크/임베드/재import와 단독 패키지는 별도 검증 프로젝트에서 수행한 결과를 기록합니다. [UI와 출력·복구 상세](../README.md)를 참고하십시오.

2026-10-13 UE 5.8 / Win64 / Python 3.12에서 네이티브 25개가 통과했습니다. 실제 GPU 베이크·HDR·2슬롯·선택 맵·4096, FBX SDK 재가져오기의 상대 링크·임베드 PNG·머티리얼 연결을 확인했습니다. 단독 8개·통합 17개 MCP와 geometry-only 추출, NullRHI 제한도 통과했습니다. 별도 검증 프로세스를 첫 원본 백업 이동 직후 종료하고 두 번 재시작해 복구 차단·명시적 해제·원본/staging/backup hash 보존을 확인했습니다. 네트워크 조회 timeout은 transport 장애 주입으로 검사하며 네이티브 호출 내부를 강제로 중단하지 않습니다.

## 1.2 보존·진단

최근·보관 완료 작업은 각각 100건·7일로 제한합니다. 만료된 기록과 UUID/hash 색인은 함께 삭제되어 같은 UUID가 새 작업으로 접수될 수 있습니다. 실행·복구·손상 및 결과 전달 중인 기록은 보호하고 출력 파일을 자동 삭제하지 않습니다. status의 진단 ID를 기존 job_status/job_control로 조회·확인하며 진단에는 CLI --wait를 사용하지 않습니다. 자세한 계약은 저장소 MCP 통합 안내에 있습니다.
