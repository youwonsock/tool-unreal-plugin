# tool-unreal-plugin

Unreal Engine 프로젝트에서 재사용하는 Editor 플러그인을 보관하는 저장소입니다.

각 플러그인은 독립 도킹 탭과 프리셋·단계 실행 큐를 제공합니다. MCP 설치·연결·Python 호출은 [MCP 통합 안내](MCP/README.md)에 있습니다. 플러그인 하나만 복사해도 독립적으로 설치할 수 있고, 함께 설치하면 `unreal_tools` 연결 하나에서 17개 도구를 사용합니다. 기존 CubeShot 5개·FBX 4개 호출의 기본 동작과 응답을 유지하며 API와 런타임 버전은 1.2.0입니다. 오류 코드는 동기·비동기 경로에서 공통으로 전달하며 이력 보존 범위 밖의 요청 ID는 새 요청으로 처리됩니다.

## 현재 플러그인

| 플러그인 | 설명 | 문서 |
| --- | --- | --- |
| Unreal_FBX_Exporter | Content Browser에서 Static Mesh와 PBR 텍스처 세트를 FBX로 내보내는 Editor 플러그인 | [플러그인 README](<./Unreal_FBX_Exporter/README.md>) |
| Unreal_CubeShot | Static Mesh를 여섯 방향에서 촬영해 Texture2D 에셋으로 생성하는 Editor 플러그인 | [플러그인 README](<./Unreal_CubeShot/README.md>) |

## 코드 소유권과 호출 경로

- `Shared/Native/`: GUI와 MCP가 함께 사용하는 C++ 작업·프리셋·예약·복구·이력 런타임의 원본입니다.
- `MCP/`: Python 연결·설치·호출 런타임과 실제 도구 catalogue 기반 문서 생성기를 관리합니다.
- `Scripts/sync_runtime.py`: 두 원본을 각 독립 플러그인으로 배포합니다. `MCP/sync_runtime.py`는 기존 명령의 호환 진입점입니다.
- 각 플러그인의 `Source/`: 해당 도구의 옵션·배치 계획·실행 서비스, Slate GUI, MCP 어댑터와 촬영/추출 구현을 관리합니다.

GUI와 MCP는 같은 서비스와 작업 큐를 호출합니다. GUI는 선택·편집·확인·결과 표시를, MCP는 JSON/schema·스레드 전환·응답 포장을 담당합니다. GUI의 미리보기는 transient 출력용 별도 경로이며 저장 작업의 예약·기록을 생성하지 않습니다.

공통 원본을 수정한 뒤 `python Scripts/sync_runtime.py`를 실행합니다. 각 플러그인 내부 동기화 사본을 직접 수정하지 않습니다. `--check`로 drift를 확인하며 원본 헤더 누락은 오류입니다. `--python-only`는 C++ 동기화를 명시적으로 생략합니다. 플러그인 하나를 복사해도 저장소 상위 폴더 없이 빌드·설치할 수 있습니다.
