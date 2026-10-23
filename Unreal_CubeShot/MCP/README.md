# CubeShot MCP 2.0

독립 설치 가능한 8개 도구를 제공합니다. 구형 단일 실행 호출은 제거했으며 촬영·추출은 작업 큐를 사용합니다.

| 도구 | 기능 |
| --- | --- |
| `cubeshot_status` | 프로젝트·API·실행 가능 상태 |
| `cubeshot_find_meshes` | query/root/limit로 메시 검색 |
| `cubeshot_preflight` | 배치 출력 검사; 파일 생성 없음 |
| `cubeshot_profiles` | 프리셋 list/get/save/delete |
| `cubeshot_job_start` | 순서가 고정된 배치 접수 |
| `cubeshot_job_status` | job_id 또는 request_id로 조회 |
| `cubeshot_job_control` | 취소 또는 검토한 복구 확인 |
| `cubeshot_export_png` | 기존 Texture2D source의 PNG 내보내기; overwrite=false |

촬영 기본값은 512·Orthographic·Transparent·Auto Fit이며 배치 PNG 자동 저장은 기본 해제입니다. 옵션에는 resolution, projection, background, background_texture, solid_color, auto_fit, camera_distance, field_of_view, ortho_width, export_png, png_directions, trim_transparent, padding_pixels, quarter_turns가 있습니다. Texture2D·PNG 교체에는 명시적인 overwrite=true가 필요합니다. 독립 PNG의 trim/padding/rotation은 원본 RGBA를 보존합니다.

## 설치와 실행

UE 5.8 Win64 Editor와 Python 3.11 이상을 사용합니다. 플러그인 폴더 전체를 프로젝트 `Plugins`에 복사하고 Editor를 빌드합니다. Editor를 닫고 아래 명령을 실행한 뒤 다시 열어 Codex의 프로젝트 MCP 설정을 다시 로드합니다.

```powershell
python Plugins/Unreal_CubeShot/MCP/setup_mcp.py --dry-run
python Plugins/Unreal_CubeShot/MCP/setup_mcp.py
python -m pip install -r Plugins/Unreal_CubeShot/MCP/requirements.txt
python Plugins/Unreal_CubeShot/MCP/call_mcp.py --list-tools
python Plugins/Unreal_CubeShot/MCP/call_mcp.py cubeshot_job_start --arguments-file Plugins/Unreal_CubeShot/MCP/examples/job-start.json --wait
```

외부 경로에서는 `--project <전체경로.uproject>`를 지정합니다. 기본 주소는 `http://127.0.0.1:8000/mcp`이며 `--port`, `--url-path`로 변경합니다. 현재 설정의 주소·타임아웃과 다른 서버는 보존합니다. 이 CLI는 해당 플러그인의 도구만 호출합니다. 두 플러그인을 설치하면 통합 `unreal_tools` 연결에서 15개 도구를 사용합니다.

개별 provider 서버 테이블과 API 1.x manifest·생성 상태를 자동 이전하지 않습니다. 이전 개별 테이블은 직접 제거합니다. 통합 설정과 생성 상태의 명시적 교체에는 `--replace-owned-config --dry-run`을 먼저 확인한 뒤 `--dry-run` 없이 실행합니다. 현재 생성 상태와 다른 사용자 제한은 자동 해제하지 않습니다.

구형 `cubeshot`/`fbx_exporter` 서버 테이블은 플러그인의 활성·설치 여부와 별도로 감지합니다. 해당 테이블이 남아 있으면 setup은 파일 변경 전에, CLI는 연결 전에 거부하며 이름을 표시합니다. 비활성 manifest는 읽지 않고 `--replace-owned-config`도 구형 테이블 검사를 우회하지 않습니다.

명시적 비활성화나 현재 플랫폼/Editor 타깃 제한으로 제외된 플러그인의 manifest는 읽지 않습니다. 비활성 구버전이 함께 설치되어도 현재 도구를 사용할 수 있으며, 활성 구버전은 API 2 검증에서 거부합니다. descriptor의 현재 AllowList/DenyList와 SupportedTargetPlatforms만 사용하며 이전 Whitelist/Blacklist 키는 해석하지 않습니다.

## 요청과 결과

검사와 접수는 `mesh_paths` 배열, 공통 `options`, 선택적 `profile_id`, 경로별 `overrides`를 사용합니다. 하나의 대상도 배열로 지정합니다. preflight는 동일한 형식을 사용하되 `request_id`를 생략할 수 있습니다. 프리셋과 출력 승인은 구분하며 옵션 우선순위는 override → options → profile → 기본값입니다.

```json
{"mesh_paths":["/Engine/BasicShapes/Cube.Cube"],"options":{"resolution":512},"overrides":{}}
```

접수에는 UUID `request_id`가 필요하며 CLI는 생략 시 생성해 출력합니다. 같은 ID와 요청은 기존 작업을 반환하고 다른 요청은 거부합니다. 응답 유실 후 같은 ID로 조회하며 자동 재접수하지 않습니다. 성공적인 조회의 `success=true`와 실제 작업의 `state`를 구분합니다.

`--wait`는 단일 job_start/job_status에만 사용합니다. 기본 전체 대기는 1800초, 폴링 간격은 1초입니다. `succeeded`만 종료 코드 0이며 대기 만료는 취소나 재접수를 뜻하지 않습니다. 작업은 queued/running/succeeded/failed/cancelled/interrupted이고 후속 미실행 대상은 skipped로 표시합니다. 취소는 안전 경계에서 적용하며 Editor 재시작 후 자동 재개하지 않습니다.

매 연결에서 status의 프로젝트 전체 경로, API 2.x와 capabilities 배열을 검사합니다. 호출 기록은 `Saved/CubeShot/MCPCalls`, 작업 기록은 `Saved/CubeShot/Jobs`, 프리셋은 `Saved/CubeShot/Profiles`에 저장합니다. 최근/보관 완료 기록은 각각 100건·7일입니다. 출력·staging·backup·미해결 복구·손상 원본은 자동 삭제하지 않습니다. 누락된 완료 시각 등 이전 형식은 자동 보정하지 않고 진단으로 남깁니다. `acknowledge_recovery`는 검토한 기록의 출력 예약을 해제하며 출력 파일을 복원하거나 삭제하지 않습니다.

## 검증과 명세

현재 변경은 Unreal 없이 Python·독립 C++·배포 검사까지 검증합니다. API 2.0의 Editor 빌드·Automation·실제 출력 검증은 미실행입니다. 이전 실행 결과는 [저장소 검증 기록](https://github.com/youwonsock/tool-unreal-plugin/blob/1168a18/MCP/VALIDATION.md)의 해당 버전 기록입니다.

현재 Editor의 실제 `tools/list`를 캡처한 뒤 저장소 `MCP/generate_tool_docs.py`로 tool-spec.json과 TOOL_REFERENCE.md를 생성합니다. API 1.x 캡처와 구형 인자 전용 예제는 제거했습니다. 현재 예제는 `examples/*.json`의 tool/arguments 형식입니다. 원본 코드는 저장소에서 수정하고 `python Scripts/sync_runtime.py`로 배포합니다.
