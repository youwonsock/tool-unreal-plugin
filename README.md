# tool-unreal-plugin

Unreal Engine 프로젝트에서 재사용하는 Editor 플러그인을 보관하는 저장소입니다.

각 플러그인은 독립 도킹 탭과 프리셋·단계 실행 큐를 제공합니다. MCP 설치·호출은 [MCP 통합 안내](MCP/README.md)에 있습니다. 플러그인 하나만 복사해도 독립 설치할 수 있고, 함께 설치하면 `unreal_tools` 연결에서 CubeShot 8개·FBX 7개, 총 15개 도구를 사용합니다. API와 Python 런타임 버전은 **2.0.0**입니다. 촬영·추출은 `job_start`로 접수하고 `job_status`로 조회합니다. 구형 단일 실행 호출·설정 이관·기록 보정은 지원하지 않습니다.

## 현재 플러그인

| 플러그인 | 설명 | 문서 |
| --- | --- | --- |
| Unreal_FBX_Exporter | Content Browser에서 Static Mesh와 PBR 텍스처 세트를 FBX로 내보내는 Editor 플러그인 | [플러그인 README](<./Unreal_FBX_Exporter/README.md>) |
| Unreal_CubeShot | Static Mesh를 여섯 방향에서 촬영해 Texture2D 에셋으로 생성하는 Editor 플러그인 | [플러그인 README](<./Unreal_CubeShot/README.md>) |

## 코드 소유권과 호출 경로

- `Shared/Native/`: GUI와 MCP가 함께 사용하는 C++ 작업·프리셋·예약·복구·이력 런타임의 원본입니다.
- `MCP/`: Python 연결·설치·호출 런타임과 실제 도구 catalogue 기반 문서 생성기를 관리합니다.
- `Scripts/sync_runtime.py`: 두 원본을 각 독립 플러그인으로 배포합니다.
- 각 플러그인의 `Source/`: 해당 도구의 옵션·배치 계획·실행 서비스, Slate GUI, MCP 어댑터와 촬영/추출 구현을 관리합니다.

GUI와 MCP는 같은 서비스와 작업 큐를 호출합니다. GUI는 선택·편집·확인·결과 표시를, MCP는 JSON/schema·스레드 전환·응답 포장을 담당합니다. GUI의 미리보기는 transient 출력용 별도 경로이며 저장 작업의 예약·기록을 생성하지 않습니다.

공통 원본을 수정한 뒤 `python Scripts/sync_runtime.py`를 실행합니다. 각 플러그인 내부 동기화 사본을 직접 수정하지 않습니다. `--check`로 drift를 확인하며 원본 헤더 누락은 오류입니다. `--python-only`는 C++ 동기화를 명시적으로 생략합니다. 플러그인 하나를 복사해도 저장소 상위 폴더 없이 빌드·설치할 수 있습니다.

## Editor workspace

두 도구는 행별 독립 옵션과 모듈 소유 workspace를 사용합니다. 기본 옵션 변경은 새 대상에만 반영하며, 기존 행은 명시적인 편집·프리셋·일괄 적용·초기화로 변경합니다. GUI는 확정한 행별 옵션 snapshot으로 기존 작업 서비스를 호출하고 현재 배치 API와 저장 서비스에 전달합니다.

`Shared/Native/UnrealToolsWorkspace.h`는 Unreal 없이 컴파일하는 상태 코어이며 `UnrealToolsWorkspaceController.h`는 JSON·프리셋·이력 서비스 어댑터입니다. 각 플러그인 controller와 Slate 표시 코드는 분리되어 있습니다. 공통 코드는 기존 동기화 명령으로 배포합니다.

`UnrealToolsServiceInstance.h`의 세대 번호로 작업 서비스의 인스턴스 수명을 구분합니다. 같은 주소와 revision으로 서비스가 재생성돼도 이전 결과 캐시나 pin 식별자를 재사용하지 않습니다. 세대 할당기는 Unreal 없이 컴파일·검사하며, 실제 controller의 캐시·pin 재연결은 별도 UE 회귀 테스트 대상입니다.

```sh
python3 Scripts/sync_runtime.py --check
python3 Scripts/test_workspace.py
python3 -m unittest discover -s MCP/tests -q
python3 -m unittest discover -s Unreal_CubeShot/MCP/tests -q
```

[workspace 검증 기록과 Unreal 후속 확인](MCP/WORKSPACE_VALIDATION.md)을 참고하세요.
