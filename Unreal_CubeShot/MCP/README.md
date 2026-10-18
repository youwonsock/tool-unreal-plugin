# CubeShot MCP 1.1

이 플러그인의 9개 C++ 도구는 UE 5.8의 ModelContextProtocol HTTP 서버에 등록됩니다. Python 설치·호출 코드와 공통 native runtime 배포본을 포함하므로 CubeShot 폴더만 다른 프로젝트에 복사해도 동작합니다. 지원 기준은 UE 5.8 / Win64 Editor / Python 3.11 이상입니다.

## 설치·설정 이전

Editor를 닫고 프로젝트 루트에서 실행합니다.

```powershell
python Plugins/Unreal_CubeShot/MCP/setup_mcp.py --dry-run
python Plugins/Unreal_CubeShot/MCP/setup_mcp.py
python -m pip install -r Plugins/Unreal_CubeShot/MCP/requirements.txt
```

`.uproject`가 여러 개이거나 외부 경로의 플러그인이면 `--project C:/Projects/MyProject/MyProject.uproject`를 지정합니다. 신규 주소는 `http://127.0.0.1:8000/mcp`이며 시작 20초·호출 180초를 사용합니다. 기존 포트·URL 경로·타임아웃을 보존합니다. 여러 Editor는 `--port`로 서로 다른 포트를 사용합니다.

설치 도구는 활성 Editor 플러그인 manifest를 검색해 `.codex/config.toml`의 `unreal_tools` 연결 하나를 생성합니다. CubeShot 1.1 단독은 9개, FBX Exporter 1.1과 함께 설치하면 17개입니다. 이 폴더의 CLI는 CubeShot 도구만 허용합니다.

설정 생성 기록은 `.codex/unreal-tools-state.json`에 저장합니다. 기존 1.0 전체 도구 목록 또는 마지막 관리 기록과 일치하는 목록만 자동 확장하며, 사용자 부분 제한·disabled_tools·비활성화는 자동으로 풀지 않습니다. 충돌은 변경 없이 설명합니다. 생성 설정을 명시적으로 교체할 때만 `--replace-owned-config`를 사용하고 먼저 dry-run으로 확인합니다. 설정 적용 전에 복구본과 임시 파일을 준비하며 실패 시 실행 직전 상태로 복원합니다.

플러그인 바이너리 업데이트에는 Editor 재시작이 필요합니다. 요청은 등록 세대와 실행 상태를 확인해 이전 등록의 지연 요청을 EDITOR_INTERRUPTED로 차단합니다.

Editor를 열고 **Codex의 프로젝트 MCP 설정을 다시 로드**해 `unreal_tools`를 확인합니다. 다른 MCP 클라이언트는 출력한 MCP_URL을 Streamable HTTP 연결에 사용합니다. 전용 설정 생성은 Codex만 지원합니다.

## 도구

모든 도구는 input/output schema를 게시하고 서버에서도 검증합니다. 알 수 없는 필드·잘못된 숫자·enum·경로를 거부합니다. `success`는 해당 요청의 성공 여부이며, 작업 조회의 `success=true`가 작업 자체의 성공을 의미하지는 않습니다.

| 이름 | 인수·기능 |
| --- | --- |
| cubeshot_status | 인수 없음. project_file, PIE/SIE/RHI 상태, api_version, capabilities, active_jobs |
| cubeshot_find_meshes | query 필수, root=/Game, limit=40(1~200). 등록된 엔진·플러그인 mount도 명시적으로 검색 |
| cubeshot_preflight | mesh_path 또는 mesh_paths 중 하나, 촬영 옵션. 단일 기존 응답 유지; 배치는 items와 전체 검사 결과 |
| cubeshot_generate | mesh_path, 촬영 옵션, overwrite=false. 기존 완료 응답·기본값 유지, 여섯 패키지 저장 결과 |
| cubeshot_export_png | texture_path 필수, trim_transparent=false, padding_pixels=8, quarter_turns=0. 기존 PNG 교체 동작 유지 |
| cubeshot_profiles | action=list/get/save/delete, get/delete는 id, save는 profile JSON 및 기존 사용자 프리셋 교체 시 overwrite=true |
| cubeshot_job_start | request_id, mesh_paths 필수. profile_id, options, overrides, overwrite=false |
| cubeshot_job_status | job_id 또는 request_id 중 하나 |
| cubeshot_job_control | action=cancel/acknowledge_recovery, job_id 또는 request_id 중 하나 |

공통 촬영 옵션은 resolution(256~4096), projection(orthographic/perspective), background(transparent/solid/texture), background_texture, solid_color(선형 RGBA 4개), auto_fit, camera_distance, field_of_view, ortho_width, export_png, png_directions(Front/Back/Left/Right/Top/Bottom), trim_transparent, padding_pixels, quarter_turns입니다. 구체적 범위와 최신 schema는 Editor의 `tools/list`로 확인합니다.

기존 generate 기본값은 512·Orthographic·Transparent·Auto Fit이며 PNG 자동 저장은 끕니다. 기존 export_png는 저장된 PNG를 교체합니다. trim/padding은 원본 범위에서 잘라낸 RGBA를 그대로 보존하며 알파 0 texel의 RGB도 유지합니다. 가장자리 padding은 원본 프레임에서 잘립니다. **새 큐는 Texture2D와 PNG 모두 overwrite=false가 기본값**입니다. 프리셋은 overwrite 승인을 저장하지 않습니다.

## CLI와 작업

```powershell
python Plugins/Unreal_CubeShot/MCP/call_mcp.py --list-tools
python Plugins/Unreal_CubeShot/MCP/call_mcp.py cubeshot_status
python Plugins/Unreal_CubeShot/MCP/call_mcp.py cubeshot_preflight --arguments-file mesh.json
python Plugins/Unreal_CubeShot/MCP/call_mcp.py cubeshot_job_start --arguments-file job.json --wait
```

`mesh.json`:

```json
{"mesh_path":"/Engine/BasicShapes/Cube.Cube","resolution":512,"projection":"orthographic"}
```

`job.json`:

```json
{
  "request_id": "c6771378-2f46-4e92-8713-ff5801b3c73f",
  "mesh_paths": ["/Engine/BasicShapes/Cube.Cube"],
  "profile_id": "item_icon",
  "options": {"resolution": 512},
  "overrides": {},
  "overwrite": false
}
```

옵션 우선순위는 대상 override → 공통 options → 선택 프리셋 → API 기본값입니다. 같은 request_id·같은 요청은 원래 작업을 반환하며, 같은 ID·다른 요청은 오류입니다. 접수는 값과 기록을 고정하고 에셋 로드·컴파일·실제 검사는 큐에서 수행합니다. 접수 응답이 유실되면 request_id로 조회하고 자동 재제출하지 않습니다.

작업 상태는 queued/running/succeeded/failed/cancelled/interrupted이며 대상에는 skipped가 추가됩니다. 첫 오류 이후 후속 대상을 중단합니다. 완료 출력은 유지합니다. 취소는 안전한 경계에서 적용합니다. pending staging PNG worker가 완료된 후 취소/정리하고, 현재 메시의 package save/PNG 게시를 시작했다면 마칩니다. worker는 소유한 픽셀·파일만 처리하며 게임 스레드 callback 없이 종료할 수 있습니다. Editor 재시작은 interrupted로 표시하며 자동 재개하지 않습니다.

`--wait`는 단일 job_start/job_status에만 적용합니다. 기본 전체 대기는 30분, 폴링 1초이며 `--wait-timeout`/`--poll-interval`로 조정합니다. succeeded만 종료 코드 0입니다. timeout이나 연결 단절은 취소를 뜻하지 않으며 접수된 ID를 보존합니다. 일반 batch는 JSON 요청 배열을 순차 실행하고 오류 이후 중단합니다.

매 연결/재연결에서 `cubeshot_status.project_file`과 지정 프로젝트의 전체 경로를 비교해 다른 Editor에서 출력 작업을 하지 않습니다. 기존 `run_calls` 및 Haven `CallCubeShotMcp.run_calls` 인터페이스를 유지합니다. 요청 기록은 `Saved/CubeShot/MCPCalls`, 작업 journal은 `Saved/CubeShot/Jobs`, PNG는 `Saved/CubeShot/MCPExports`입니다.

## 프리셋 JSON·복구

```json
{
  "schema_version": 1,
  "provider_id": "cubeshot",
  "id": "my_icon",
  "display_name": "My Icon",
  "options": {"resolution":512,"projection":"orthographic","background":"transparent","export_png":true,"png_directions":["Front"],"trim_transparent":true,"padding_pixels":8,"quarter_turns":0}
}
```

profiles save는 위 JSON을 profile에 넣습니다. builtin editor_default/item_icon은 변경할 수 없습니다. UI에서 export한 JSON은 조회용 success/builtin 필드를 제거해 import/save에 사용할 수 있습니다.

부분 패키지 실패는 saved/failed/dirty_packages/errors/recovery_staging을 제공합니다. dirty 패키지를 해결하고 staging과 저장 결과를 확인한 뒤 `acknowledge_recovery`로 출력 예약을 해제합니다. 이 작업은 복원·삭제를 하지 않습니다. 미완료 저장 중 Editor가 종료된 경우도 같은 확인이 필요합니다.

## 검증·공통 코드 유지

```powershell
python -m unittest discover -s Plugins/Unreal_CubeShot/MCP/tests -v
```

원본 저장소에서는 MCP Python 및 Shared/Native C++ 원본을 수정하고 `python Scripts/sync_runtime.py`로 배포합니다. `--check`가 재귀 런타임 불일치를 검출합니다. Unreal Automation `CubeShot.Editor`에는 writer 실패 주입·atomic PNG·옵션·등록 및 공통 job/profile 테스트가 포함됩니다. UAT BuildPlugin은 standalone runtime과 MCP 폴더를 함께 패키징합니다.

2026-10-13 UE 5.8 / Win64 / Python 3.12에서 네이티브 36개가 통과했습니다(의도적인 실패 입력·fixture 정리 경고 3개, 실패 0). 실제 MCP로 단독 9개·통합 17개를 확인하고 4096 및 NPOT 257에서 각각 6면과 PNG를 저장했습니다. 실제 PIE/SIE·NullRHI 제한, 외부 함수/property 변경, 등록 세대, 6개 UObject 참조 보존, 두 화면 크기와 탭 재개방도 확인했습니다. 응답 유실·조회 timeout은 Python transport 장애 주입으로 검사했습니다.

Epic 서버의 세션 종료 DELETE 202를 SDK가 경고로 출력할 수 있습니다. 성공 판정은 도구의 isError/structuredContent.success와 작업 state·실제 출력 검증을 기준으로 합니다.

## 1.2 보존·진단

최근·보관 완료 작업은 각각 100건·7일로 제한합니다. 만료된 기록과 UUID/hash 색인은 함께 삭제되어 같은 UUID가 새 작업으로 접수될 수 있습니다. 실행·복구·손상 및 결과 전달 중인 기록은 보호하고 출력 파일을 자동 삭제하지 않습니다. status의 진단 ID를 기존 job_status/job_control로 조회·확인하며 진단에는 CLI --wait를 사용하지 않습니다. 자세한 계약은 저장소 MCP 통합 안내에 있습니다.
