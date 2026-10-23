# Unreal Tools MCP

CubeShot과 FBX_Exporter는 C++ 도구를 각 플러그인에서 등록하고 UE의 `ModelContextProtocol` HTTP 서버를 공유합니다. API와 Python 런타임 버전은 **2.0.0**입니다. 설치·검색·Python 호출 원본은 이 폴더이고 GUI/MCP 공통 C++ 원본은 `../Shared/Native`입니다. `../Scripts/sync_runtime.py`가 코드와 진입점을 각 플러그인에 포함하므로 플러그인 하나만 다른 프로젝트에 복사해도 사용할 수 있습니다. 별도 서버 프로세스나 공용 C++ 플러그인은 없습니다.

지원 기준: **UE 5.8 / Win64 Editor / Python 3.11 이상**. C++ 바이너리는 대상 엔진으로 빌드해야 합니다. 설치와 `--help`는 표준 라이브러리만 사용하며 실제 Python 호출은 `requirements.txt`의 MCP SDK가 필요합니다.

기존 1.1의 실제 출력·실패 복구·UI·독립 배포 결과는 [역사 검증 기록](VALIDATION.md)에 있습니다. 1.2 검증은 프로젝트 `Saved/UnrealTools/V12`의 요구사항·문제 목록·실행 보고서로 별도 관리합니다.

## 설치

Editor를 닫고 프로젝트 루트에서 실행합니다. 외부 공용 플러그인 폴더에서는 `--project`로 대상 `.uproject` 전체 경로를 지정합니다.

```powershell
python tool-unreal-plugin/MCP/setup_mcp.py --project Haven.uproject --dry-run
python tool-unreal-plugin/MCP/setup_mcp.py --project Haven.uproject
python -m pip install -r tool-unreal-plugin/MCP/requirements.txt
```

`Plugins` 및 `.uproject`의 `AdditionalPluginDirectories` 아래에서 `MCP/manifest.json`을 가진 활성 Editor 플러그인을 찾습니다. 명시적 비활성화와 플랫폼/타깃 제한을 먼저 적용하고, 제외된 플러그인의 manifest는 읽지 않습니다. 비활성 구버전이 함께 설치되어도 현재 플러그인의 설정·목록·호출을 막지 않으며, 활성 구버전은 API 2 검증에서 거부합니다. 현재 `PlatformAllowList/DenyList`, `TargetAllowList/DenyList`, `SupportedTargetPlatforms`를 사용하며 이전 `Whitelist*`/`Blacklist*` 키는 해석하지 않습니다. 활성 플러그인 사이의 중복 provider ID, 플러그인 ID, 도구 이름은 변경 전에 오류로 처리합니다. 플러그인 내부의 `MCP/setup_mcp.py`도 프로젝트 전체를 검색하므로 재실행해도 다른 활성 플러그인의 도구가 사라지지 않습니다.

| 활성 플러그인 | 연결 이름 | 허용 도구 수 |
| --- | --- | --- |
| CubeShot | `unreal_tools` | 8 |
| FBX_Exporter | `unreal_tools` | 7 |
| 둘 다 | `unreal_tools` | 15 |

다른 MCP 클라이언트는 출력된 `MCP_URL`을 Streamable HTTP 주소로 등록합니다. 전용 설정 생성은 Codex만 지원합니다. 허용 목록은 클라이언트의 제한이며 UE 서버의 엔진 도구 등록을 바꾸지 않습니다.

## 설정과 재로딩

현재 설치기는 `[mcp_servers.unreal_tools]` 연결 하나와 API 2.0 생성 상태만 관리합니다. `.codex/unreal-tools-state.json`의 `api_version`, 허용 목록과 현재 설정을 확인한 뒤 활성 플러그인 변경을 반영합니다. 사용자 제한·비활성화는 자동으로 풀지 않습니다.

개별 `cubeshot`/`fbx_exporter` 서버 테이블, 이전 manifest와 생성 상태는 자동 병합·변환하지 않습니다. 두 구형 서버 이름은 플러그인이 비활성화되거나 제거된 경우에도 설정 계층에서 별도로 감지합니다. 비활성 manifest를 읽지 않으며, 구형 테이블이 남아 있으면 setup은 파일 변경 전에, CLI는 서버 연결 전에 해당 이름을 표시하고 거부합니다. 이전 개별 서버 테이블은 직접 제거해야 하며 `--replace-owned-config`도 이 검사를 우회하지 않습니다. 기존 통합 설정이나 생성 상태를 다시 구성하려면 원하는 허용 목록을 확인한 뒤 `--replace-owned-config --dry-run`으로 검토하고 같은 옵션에서 `--dry-run`을 빼고 실행합니다. 이 명령은 통합 연결과 생성 상태만 교체하며 다른 서버 테이블을 삭제하지 않습니다. 포트·URL 경로와 타임아웃은 현재 통합 설정에서 보존합니다.

모든 변경 파일의 임시본과 실행별 `*.unreal-tools.<실행 ID>.bak`을 준비한 뒤 같은 디렉터리에서 파일을 교체합니다. 최초 `*.unreal-tools.bak`도 유지합니다. 상태 파일은 마지막에 교체하며 중간 실패 시 이번 실행 직전 상태로 복구합니다. 복구도 실패하면 원래 오류·실패 경로·해당 실행의 복구본을 표시합니다. `--dry-run`은 파일·백업을 생성하지 않습니다. 같은 설정으로 재실행하면 변경하지 않습니다.

설치 후 Editor를 열고 Codex에서 프로젝트의 MCP 연결을 다시 로드해 `unreal_tools`와 총 15개 도구를 확인합니다. Editor 재시작과 Codex 설정 재로딩은 별개입니다.

## CLI

```powershell
python tool-unreal-plugin/MCP/call_mcp.py --list-tools
python tool-unreal-plugin/MCP/call_mcp.py cubeshot_status
python tool-unreal-plugin/MCP/call_mcp.py fbx_exporter_preflight --arguments-file mesh.json
python tool-unreal-plugin/MCP/call_mcp.py --batch-file batch.json
```

`mesh.json` 예제:

```json
{"mesh_paths":["/Engine/BasicShapes/Cube.Cube"],"options":{"resolution":512,"maps":[]}}
```

`batch.json`은 `[{"tool":"cubeshot_status","arguments":{}},{"tool":"fbx_exporter_status","arguments":{}}]` 형태입니다. 촬영/추출 전에는 각 도구의 preflight 결과를 확인합니다. 배치는 오류 응답이나 예외 이후 요청을 실행하지 않으며 CLI는 실패 종료 코드를 반환합니다.

전체 배치의 허용 목록을 먼저 검사하고 관련 플러그인 **모두**의 status 응답에 있는 `.uproject` 전체 경로를 확인한 후 실행합니다. `--url`을 지정해도 확인을 생략하지 않습니다. 주소는 로컬 HTTP만 지원합니다. 같은 이름의 다른 프로젝트와 잘못된 포트 연결도 차단합니다.

플러그인 내부 `MCP/call_mcp.py`는 자기 도구만 조회·호출합니다. 루트 CLI의 인자 생략은 목록 조회, 개별 CLI의 인자 생략은 해당 status 호출입니다. 구형 Python 호출 어댑터와 `default_project` 인자는 제거했으며 CLI의 `--project`로 대상을 지정합니다.

| 진입점 | 대상 프로젝트의 기록 경로 |
| --- | --- |
| 공통 CLI | `Saved/UnrealTools/MCPCalls` |
| CubeShot CLI | `Saved/CubeShot/MCPCalls` |
| FBX_Exporter CLI | `Saved/FBX_Exporter/MCPCalls` |

기록 저장 실패는 경고로 표시하고 실제 성공 결과나 원래 예외를 보존합니다. 작업 접수 인수는 같은 provider의 `MCPRequests`에도 기록합니다.

## 작업 접수·대기·재연결

CubeShot 8개와 FBX 7개 도구를 제공합니다. 실행은 `job_start`로 접수하고 `job_status`로 조회합니다. 구형 `cubeshot_generate`와 `fbx_exporter_export`는 등록하지 않습니다. preflight도 `mesh_paths`, `options`, `overrides`를 사용하며 `mesh_path`·최상위 옵션 형식은 지원하지 않습니다. 실제 도구 이름은 provider ID를 접두어로 붙입니다.

| 역할 | 동작 |
| --- | --- |
| `profiles` | `list/get/save/delete`; 기본 프리셋 수정 불가, 저장 옵션에 출력 덮어쓰기 승인 포함 불가 |
| `job_start` | request UUID, 순서가 있는 `mesh_paths`, 프리셋·공통 옵션·대상별 override를 접수하고 `job_id` 반환 |
| `job_status` | `job_id` 또는 `request_id` 중 하나로 조회; `success`는 조회 성공, `state`는 작업 상태 |
| `job_control` | `job_id` 또는 `request_id` 중 하나와 `action=cancel` 또는 `acknowledge_recovery` |

`job_status`와 `job_control`은 **정확히 하나의 ID**를 사용합니다. 둘 다 보내거나 둘 다 생략하면 오류입니다. 제어 인수의 두 표현은 다음과 같습니다.

```json
{"job_id":"<반환된 작업 ID>","action":"cancel"}
```

```json
{"request_id":"c6771378-2f46-4e92-8713-ff5801b3c73f","action":"acknowledge_recovery"}
```

옵션 우선순위는 **대상 override → 공통 options → profile → API 기본값**입니다. 출력 덮어쓰기는 접수마다 명시하며 기본 false입니다. 독립 `cubeshot_export_png`도 기본 false이며 파일 교체에는 명시적인 true가 필요합니다. 동일 request UUID와 동일 요청은 기존 작업을 반환하고 다른 요청은 충돌 오류로 처리합니다. 프리셋을 변경해도 이미 접수한 옵션은 바뀌지 않습니다.

CLI용 `job.json` 예제:

```json
{"mesh_paths":["/Engine/BasicShapes/Cube.Cube"],"profile_id":"item_icon","overwrite":false}
```

```powershell
python tool-unreal-plugin/MCP/call_mcp.py cubeshot_job_start --arguments-file job.json --wait
python tool-unreal-plugin/MCP/call_mcp.py cubeshot_job_status --arguments '{"job_id":"<반환된 ID>"}' --wait
```

`job_start`에서 request UUID를 생략하면 CLI가 생성해 **전송 전에 `MCP_REQUEST_ID`를 출력·기록**합니다. MCP를 직접 호출하는 클라이언트는 UUID를 직접 넣습니다. 접수 응답이 유실되면 Python 클라이언트는 새 연결에서 동일 request ID로 조회하며 자동으로 재접수하지 않습니다. 조회도 실패하면 해당 ID로 다시 상태를 확인합니다.

`--wait`는 단일 `job_start/job_status`만 지원하며 동기 도구·배치에는 사용할 수 없습니다. 기본 전체 대기는 1800초, 폴링 간격은 1초이며 `--wait-timeout`과 `--poll-interval`로 바꿉니다. 개별 HTTP 호출 제한은 프로젝트 설정의 `tool_timeout_sec`입니다. 새 연결마다 프로젝트 전체 경로·API·capabilities를 확인합니다. 일시적 조회 연결 실패는 전체 기한 안에서 재시도합니다. 프로젝트나 capabilities가 바뀌면 중단합니다.

종료 코드는 작업의 `state=succeeded`만 0입니다. failed/cancelled/interrupted·조회 실패·전체 대기 만료는 1입니다. 대기 만료·연결 단절은 작업 취소나 재제출을 의미하지 않으며 ID를 보존합니다. GPU·FBX 네이티브 호출 중에는 상태 응답과 취소 적용이 지연될 수 있습니다. 취소 요청 수용과 실제 취소 완료를 구분합니다.

`acknowledge_recovery`는 기록된 복구 경로를 사용자가 확인한 뒤 출력의 재사용 차단을 해제하는 동작입니다. **파일을 복원하거나 삭제하지 않습니다.** CubeShot의 dirty 출력 패키지는 먼저 해결해야 합니다. 재시작 후 interrupted 작업을 자동 재실행하지 않습니다.

모든 연결은 현재 Editor의 API 2.x 버전과 capabilities 배열을 검사합니다. API 버전 누락·1.x·다른 major 버전은 status/목록 조회에서도 거부합니다. manifest에 API·최소 런타임·job 역할을 명시해야 하며 이전 버전 기본값을 추정하지 않습니다. 오래된 Python 런타임이 이미 로드된 경우 새 프로세스에서 다시 시작합니다.

## 공통 코드 배포와 검증

공통 Python 코드는 이 폴더의 `unreal_tools_mcp/`, `setup_mcp.py`, `call_mcp.py`, `requirements.txt`에서 수정합니다. 하위 패키지와 런타임 데이터도 재귀 동기화합니다. 공통 C++ 코드의 원본은 저장소 `Shared/Native/`이며 `.h/.inl`을 각 `.uplugin`의 Editor 모듈 `Source/<모듈>/Private`에 포함합니다. 플러그인 내부 배포본을 직접 수정하지 않습니다. Python 캐시·가상환경은 동기화하지 않습니다.

```powershell
python tool-unreal-plugin/Scripts/sync_runtime.py
python tool-unreal-plugin/Scripts/sync_runtime.py --check
python -m unittest discover -s tool-unreal-plugin/MCP/tests -v
```

각 `MCP/manifest.json`은 schema 1의 식별자·status·tools와 필수 `api_version`, `minimum_runtime_version`, `capabilities`, `job_tools`를 선언합니다. `job_tools`에는 start/status/control이 모두 필요합니다. 이전 `compatible_tool_sets`는 제거했습니다. status는 `structuredContent.success=true`, 절대 `project_file`, API 버전과 capabilities 배열을 제공합니다.

`Config/FilterPlugin.ini`는 MCP 명세·런타임·의존성·문서를 UAT `BuildPlugin` 패키지에 포함하고 Python 캐시·가상환경을 제외합니다. 패키지를 단독 프로젝트에 설치해 상위 MCP 폴더 없이 실행하고, 엔진 Cube의 6면 촬영·PNG 저장과 FBX 추출을 검증합니다.

## 실제 schema와 문서 검사

빌드·Editor 재시작 후 실제 `tools/list` JSON을 저장합니다. `generate_tool_docs.py`는 해당 응답에서 플러그인의 전체 도구를 확인하고 입력·출력 schema와 각 플러그인의 `examples/*.json` 예제를 검사합니다. 예제 파일은 `{"tool":"...","arguments":{...}}` 객체 또는 그 배열이며 예제 검증에도 `requirements.txt`가 필요합니다.

```powershell
python tool-unreal-plugin/MCP/generate_tool_docs.py --catalog-file Saved/UnrealTools/tools-list.json --write
python tool-unreal-plugin/MCP/generate_tool_docs.py --catalog-file Saved/UnrealTools/tools-list.json --check
```

플러그인마다 `MCP/tool-spec.json`과 `MCP/TOOL_REFERENCE.md`를 생성합니다. `--check`는 schema·설명·문서의 차이를 검출하며 파일을 수정하지 않습니다. manifest나 C++ 문자열에서 실제 catalogue를 추정하지 않습니다. 도구가 누락되거나 예제가 schema와 맞지 않으면 문서를 갱신하기 전에 실패합니다. 이전 API의 캡처 파일은 제거했으며 현재 Editor의 실제 catalogue로 다시 생성해야 합니다. 구형 도구가 섞인 catalogue는 현재 명세로 게시할 수 없습니다.

SDK의 `Session termination failed: 202` 메시지는 UE의 세션 종료 DELETE 응답 차이로 발생할 수 있습니다. 성공 여부는 도구의 `isError`, `structuredContent.success`, 종료 코드와 출력 파일로 판단합니다.

## 이력·진단과 오류 계약

플러그인별 최근 완료 작업 100건과 보관 작업 100건을 유지하며 두 영역 모두 완료 후 7일을 넘기면 정리합니다. 기간 또는 개수 제한을 넘긴 기록의 상세 JSON과 요청 UUID/hash 색인이 함께 삭제됩니다. 삭제된 ID 조회는 `NOT_FOUND`이고 같은 UUID 제출은 새 작업입니다. 보존 중인 동일 UUID 재접수 판정은 프로필 해석보다 먼저 처리하므로 이후 프리셋을 변경·삭제해도 원래 작업을 반환합니다. CLI는 조회 실패나 만료를 자동 재접수로 바꾸지 않습니다.

실행·대기·미확인 복구·손상 기록과 완료 결과 전달 중인 기록은 보호합니다. 생성 에셋·PNG·FBX, staging·backup, MCP 호출 증거와 검증 보고서는 자동 정리하지 않습니다. 진단 기록은 일반 작업/UUID 색인과 분리하고 확인 완료 메타데이터는 7일·100건으로 제한합니다. Quarantine 원본은 자동 정리하지 않습니다.

status의 `record_issues`에서 손상/미지원 기록의 진단 ID와 경로를 확인합니다. 기존 `job_status`의 `job_id`로 진단을 조회하고 `job_control`의 `action=acknowledge_recovery`로 확인합니다. 진단은 실행 작업이 아니므로 `--wait`의 대상이 아닙니다. 확인 절차는 원본을 JSON 스캔 밖 `.raw`로 보존·검증하고 prepared guard를 저장한 뒤 손상 메타데이터를 정리하며 acknowledged guard 저장 후에만 차단을 해제합니다. 출력·staging·backup은 변경하지 않습니다. 일부 확인·저장 실패나 중간 종료 뒤 같은 진단 ID로 재시도할 수 있습니다.

완료 기록 저장 실패는 `state=failed`, `error_code=IO_ERROR`, `recovery_required=true`입니다. 작업 조회도 오류를 표시하고 이미 생성된 출력은 보존합니다. 재시작 후에는 마지막 durable checkpoint와 실제 파일로 복구를 검토하며 작업을 자동 재실행하지 않습니다. `job_status.success`는 조회 성공을 뜻하며 실행 성공 여부는 `state`를 확인합니다. 완료 시각·record_kind·실행 모드가 누락된 이전 기록은 보정하지 않고 진단으로 보존합니다.

`cubeshot_export_png`의 `overwrite`는 기본 false이며 기존 파일을 보호합니다. 명시적인 true만 교체를 허용합니다. `overwrite_allowed`는 허용 정책, `existing_file_observed`는 게시 직전 관찰값이며 동시 파일 변경이 있을 때 실제 교체 여부를 확정하는 값이 아닙니다. 큐 PNG는 생성 당시 staging snapshot을 사용하고 독립 PNG 내보내기는 호출 시점의 Texture2D source snapshot을 사용합니다.
