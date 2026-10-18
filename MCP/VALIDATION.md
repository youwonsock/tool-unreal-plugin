# Unreal Tools 1.2 검증 기록

검증 기준일: **2026-10-16**. 현재 실행 환경은 **UE 5.8 / Win64 Editor / D3D12 SM5**입니다. API·runtime·플러그인 버전은 **1.2.0**, job·diagnostic journal schema는 **1**입니다. 이 문서는 현재 실행 증거와 이전 빌드에서 재사용하는 구성요소 증거를 구분합니다.

1.1 원문은 수정 전에 [VALIDATION-v11-preserved.md](../../Saved/UnrealTools/V12/Reports/VALIDATION-v11-preserved.md)로 바이트 그대로 보존했습니다. 원문 SHA256은 `f0f6c45f4071f2458987b9c3205647e3cab480b1dd7a3a934fce67adb89b4f24`입니다. 아래 결과에 1.1의 테스트 수·메모리 측정값·SM6 환경을 합산하거나 승계하지 않았습니다.

## 현재 빌드와 설치 기준

공식 Engine BuildId는 `e994018f-7fb5-4806-a995-d1688e52575d`입니다. [현재 빌드 소스 freeze](../../Saved/UnrealTools/V12/source-build-snapshot-packages20-19.json)는 106개 파일을 포함하며, 이 JSON의 SHA256은 `9cc689d197c3f47645e1a7dc3582a7c64012b9406caeefcd9ff0b8cabeb427d5`입니다.

| 단독 배포본 | 실제 로드한 DLL SHA256 | 빌드·파일 대조 증거 |
|---|---|---|
| `CubeShot_qa20` | `76b91a907e4fa3642431a0b1fdcfc6ae9e346869cc1fbadbe383b7387b3c2cad` | [Cube20 provenance](../../Saved/UnrealTools/V12/Reports/package-cube20-provenance.json) |
| `FBX_Exporter_qa19` | `01f2cf3fa1116a48d584a7a7dd4447fc6605e2944971a56a68a0c877443b4700` | [FBX19 provenance](../../Saved/UnrealTools/V12/Reports/package-fbx19-provenance.json) |

UAT 단독 패키지와 각 QA 프로젝트의 conventional `Plugins/CubeShot`·`Plugins/FBX_Exporter` 설치본을 대조했습니다. 실제 Editor의 프로젝트 전체 경로, PID·생성 시각, MCP listener 소유자, 로드한 DLL 경로·해시·공식 BuildId는 [현재 실행 종합 증거 1](../../Saved/UnrealTools/V12/Reports/current-engine-live-verification-part1.json), [Native8 로드 증거](../../Saved/UnrealTools/V12/Reports/Native-round8/loaded-modules.json), [FBX GUI4 로드 증거](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx4/loaded-modules.json)에 연결됩니다.

**이 문서 갱신은 검증 후의 doc-only 변경입니다.** `MCP/VALIDATION.md`도 위 106개 freeze에 포함되므로, 갱신 후 이 한 파일은 빌드 시점 freeze의 해시와 다릅니다. 기존 freeze·패키지·실행 증거는 덮어쓰지 않습니다. C++ 소스·실행 코드·DLL은 이 문서 작업에서 바꾸지 않았으며 재빌드는 수행하지 않았습니다.

## 현재 실행 결과

| 검증 | 결과 | 증거와 범위 |
|---|---|---|
| Native8 | **97개 실행: 86 Success + 11 SuccessWithWarnings**, 실패·notRun·inProcess 0 | [최종 검증](../../Saved/UnrealTools/V12/Reports/Native-round8/verification.json), [원본 index](../../Saved/UnrealTools/V12/Reports/Native-round8/index.json). Cube57 + FBX40이며 공통 runtime16개씩은 이 수에 이미 포함됩니다. |
| Native8 화면·렌더 | **27개 검사 통과** | [새 화면 검토](../../Saved/UnrealTools/V12/Reports/native8-gui-layout-verification.json). 두 작업 공간의 새 1280·1920 이미지 4장과 Cube 미리보기 12타일을 직접 판독·독립 픽셀 지표로 검사했습니다. 명목 client 크기는 1280×720 / 1920×1080, 저장 이미지 실제 크기는 1280×722 / 1920×1082입니다. |
| 공통 Python | **68개 통과의 기존 기록 유지** | [Python 로그](../../Saved/UnrealTools/V12/python-final-tests.log), [동기화 check](../../Saved/UnrealTools/V12/sync-final-check.log). 해당 Python 구현이 변경되지 않은 범위의 증거이며, 현재 Engine에서 68개를 새로 실행했다는 주장은 하지 않습니다. |
| 실제 MCP Combined | **14 checks / 61 calls 통과** | [CombinedCurrent 보고서](../../Saved/UnrealTools/V12/Reports/CurrentEngineMCP/Reports/CombinedCurrent-20261016-091928-b8d66648/report.json). 현재 Cube20·FBX19 조합입니다. |
| 실제 MCP Cube 단독 | **7 checks / 33 calls 통과** | [CubeCurrent 보고서](../../Saved/UnrealTools/V12/Reports/CurrentEngineMCP/Reports/CubeCurrent-20261016-092230-e53209b2/report.json). 실제 6면 패키지·PNG RGBA crop/padding/rotation, 입력·프리셋·job 계약을 확인했습니다. |
| 실제 MCP FBX 단독 | **8 checks / 28 calls 통과** | [FBX19 보고서](../../Saved/UnrealTools/V12/Reports/CurrentEngineMCP/Reports/FbxOnly-20261016-092439-a39d1c6c/report.json). 프리셋·입력·UUID 재접수·fail-fast·취소/wait·geometry·PBR·정리를 확인했습니다. |
| 단독 첫 cold PBR | **2 checks / 3 calls 통과** | [새 cold 보고서](../../Saved/UnrealTools/V12/Reports/ColdPBR-FbxOnly-20261016-092417-86c07713/report.json), [현재 실행 종합 증거 2](../../Saved/UnrealTools/V12/Reports/current-engine-live-verification-part2.json). 뒤의 full MCP8 실행과 별도 순서로 기록됐습니다. |
| 실제 NullRHI | **2 checks / 7 calls 통과** | [현재 NullRHI 보고서](../../Saved/UnrealTools/V12/Reports/nullrhi-CombinedCurrent-20261016-092139-06248b23/report.json). Cube 촬영·접수와 FBX 텍스처 접수를 거부하고 geometry-only FBX는 성공했습니다. 이 프로세스의 사후 강제 종료를 정상 종료 증거로 취급하지 않습니다. |
| FBX19 실제 GUI | **파일·영수증 37개 + 독립 GUI 결합 28개 검사 통과** | [파일 검증](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-actions/current19-active4096-verification.json), [GUI 최종 검증](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-actions/current19-gui-final-verification.json). Native 테스트 수에 합산하지 않습니다. |
| 현재 FBX19 정상 재시작 | **33개 검사 통과** | [프로세스 간 재시작 검증](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-actions/current19-cross-restart-verification.json). PID19224 정상 종료 후 새 PID36560에서 동일 작업·출력을 읽고 다시 정상 종료했습니다. |

Native8의 **11개 경고 포함 테스트**와 **1,657개 warning event**는 다른 수치입니다. event는 Engine MetaSound 열거 1,624개, 소유 fixture의 Asset Registry 알림 24개, 의도한 terminal persistence 실패 6개, 잘못된 asset 입력 fixture 3개로 분류돼 있습니다. 실제 내역은 Native8 최종 검증에 남아 있으며, 무경고 실행이라고 표현하지 않습니다.

FBX 단독 보고서의 SDK 세션 종료 stderr에는 `Session termination failed: 202`가 남아 있습니다. 도구 응답·출력 검증과 이 종료 진단을 구분합니다.

Native8 Cube 미리보기 12타일에는 실제 WorldGrid 메시가 보이며 비-checker 배경 픽셀 비율은 42.37~43.70%입니다. FBX의 작은 1280 화면은 하단 상세·Advanced 열람에 세로 스크롤이 필요합니다. 전체 내용을 한 화면에 동시에 표시한다고 주장하지 않으며, 접힌 Advanced의 Native8 화면만으로 실제 JSON focus-loss 동작을 입증하지 않습니다. 그 동작은 별도 current19 실제 GUI 증거를 사용합니다.

`GuardedPublishingCrash` 테스트 함수는 Native8에서 성공 처리되지만, 명시적 opt-in 플래그가 없어 **강제 종료 분기를 건너뛰었습니다**. Native8 실행을 새 강제 크래시 검증으로 계산하지 않습니다.

Provider manifest 집합은 Cube **9개**, FBX **8개**, 통합 **17개**입니다. Engine 기본 `list_toolsets`·`describe_toolset`·`call_tool` 3개는 함께 존재할 수 있습니다. 따라서 단독 FBX catalogue 전체가 8개여야 한다고 제한하지 않습니다. 실제 집합·schema·project identity와 출력 대조는 두 현재 실행 종합 보고서에 기록돼 있습니다.

## 현재 FBX19 GUI·게시·재시작

실제 작업 `fbx_exporter-D40412B94567E03DA208CE85C24EAC09`는 두 타깃 모두 **succeeded / done / 2 of 2**입니다. PBR4096·전체 6맵·embed=true·reuse_direct_textures=false·새 출력 root를 사용해 **28개 파일, 24개 PNG**를 게시했습니다. 모든 PNG의 CRC·크기·fixture 기반 독립 픽셀 수학과 두 FBX 안의 **24개 내장 PNG payload 해시·중복 개수**를 검사했습니다. 원본 3개 fixture asset, 기존 작업 journal·출력·Saved CLI 영수증도 보존됐습니다.

GUI의 Advanced JSON은 bulk 적용 후 현재 4096과 새 출력 root를 표시했습니다. 값을 고치지 않고 포커스를 옮긴 뒤에도 Inspect는 **28 planned / 0 existing / 0 errors**를 유지했습니다. 잘못된 `{"resolution":` 원문과 오류가 포커스 이탈 후 남고 Queue가 거절되는 화면을 확인했습니다. 이 구간의 [접수 없음 비교](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-actions/current19-invalid-no-admission-20261016-092848-75976e77.json)는 정상 작업 **18개**, journal·출력·영수증 바이트가 그대로임을 확인합니다. 유효한 `{}` 수정과 global 재적용 후 정상 계획으로 복귀했으며, 최종 두 item의 durable 옵션도 4096입니다. 실제 텍스트 위젯의 모델 갱신·focus-loss commit·invalid draft/valid 복귀는 Native8의 기존 `WorkspaceSelectionState`에서도 검사했습니다.

[명시적 GUI 행동 기록](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-actions/current19-tab-close-actions.json)상 탭 닫기는 `00:30:03.060Z`, FBX 창이 없는 main 화면 확인은 `00:30:03.203Z`, 작업 종료는 `00:30:10.837Z`입니다. 즉 닫힌 화면 확인 뒤에도 **7.634초** 동안 실행이 계속됐습니다. 재열기 화면과 SDK 조회에서 같은 PID19224·전체 작업 ID·완료 결과를 확인했습니다. 영수증 파일 이름으로 실제 UI 행동을 추론하지 않고, 직접 검토한 닫힌 main/재열기 이미지와 기록 시각·같은 PID/ID의 영수증을 함께 사용했습니다.

완료 화면 두 시점은 **2.277초** 간격이며 진행바 ROI971×15와 완료 라벨 ROI740×21의 픽셀 차이는 0입니다. 전체 화면은 아래쪽 커서/glow 영역이 달라 동일 프레임이라고 주장하지 않습니다. 위젯 소유 스타일의 fill animation=false와 표시된 `Completed · done`을 함께 확인했으며 연속 영상 검증은 아닙니다. 실제 결과에는 복제 메시의 `Material slot 2 has no assigned material.` 경고 1개가 보이고 원본 메시의 경고는 0개입니다.

정상 재시작 검증은 정상 종료 로그·PID/listener 부재와 새 프로세스 생성 시각을 대조했습니다. **정상 job19개·전체 출력121개·선택 작업 출력28개·Saved 영수증9개**의 바이트·SHA·속성과 D404의 request/options/results/finished_at이 보존됐습니다. `revision` 140→14는 프로세스 메모리 변경 번호이므로 durable journal 필드와 구분했습니다. 관찰한 status·파일·로그에서 자동 재실행 흔적은 없지만 프로세스 전체 구간의 내부 호출을 연속 계측한 증거로 확대하지 않습니다.

## 이전 증거를 재사용하는 범위

- [이전 Cube GUI 동작 검증](../../Saved/UnrealTools/V12/Reports/GUI-current-cube-actions/cube-actions-verification.json)은 당시 Cube19/Engine 빌드에서 정상 저장·overwrite No·checkout Cancel·탭 수명 동작을 관찰한 구성요소 증거입니다. 의도한 Plane empty-trim 실패도 포함하므로 모든 타깃이 성공했다고 해석하지 않습니다. 변경되지 않은 구현에 한정해 재사용하며 현재 Cube20에서 같은 수동 동작을 재실행했다고 승격하지 않습니다.
- [Cube 진단 GUI](../../Saved/UnrealTools/V12/Reports/GUI-current-cube-diagnostic/verification.json), [FBX 진단 GUI](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-diagnostic/verification.json), [FBX strict 진단의 새 PID durable 확인](../../Saved/UnrealTools/V12/Reports/diagnostic-fbx_exporter-20261015-083934-7d56c1dd/verify-durable-20261015-085128-292d44bd.json)은 각각 기록된 이전 소스·DLL·Engine 범위의 실제 증거입니다. 현재 Native8 공통 진단 회귀와 구분하며 새 Engine에서 strict 진단 chain을 다시 실행했다고 주장하지 않습니다.
- [Crash3 검증](../../Saved/UnrealTools/V12/Reports/Crash-round3/verification.json)은 Cube19/FBX18의 실제 exit86·첫 복구/명시적 ack·두 번째 cold 확인과 출력/staging/backup 3개 sentinel 보존을 기록합니다. fixture는 `Jobs.Start` 뒤 running/publish/checkpoint metadata를 직접 기록하고 **`Testing::PublishWithJournal`를 직접 호출**합니다. 실제 domain publisher 강제 종료와 durable 복구를 입증하지만, 현재 queued runner가 precommit부터 강제 종료까지 전 구간을 주행한 end-to-end 증거는 아닙니다. 변경되지 않은 domain/runtime 구성요소에 한정해 재사용합니다.
- [이전 첫 FBX GUI 배치](../../Saved/UnrealTools/V12/Reports/GUI-current-fbx-actions/first-batch-files-verification.json)는 PBR2048 출력/경고 증거로 보존했습니다. 그 A518 작업의 탭 닫기는 완료 **51.394초 뒤**였으므로 active tab-close 증거가 아닙니다. 위 D404의 새 성공 증거가 이 과거 제한을 소급 변경하지 않습니다.

## 해석과 재현 경계

취소는 native/GPU 호출 안에서 즉시 중단하는 계약이 아니라 안전한 경계에서 적용됩니다. CubeShot 여섯 Texture2D 패키지 변경 전체는 원자적이지 않으며 부분 저장·dirty 패키지·staging 검토가 필요합니다. 파일 게시 journal/예약 해제와 진단 metadata ack도 출력·백업·복구 staging의 자동 복원·삭제·재실행을 의미하지 않습니다.

MaterialBaking linear true/false 복원·CRC와 소유 HDR false/true matrix는 현재 Native8에서 검사했습니다. 프로덕션은 실제 bake 범위에서 이전 linear 값을 복원하고 HDR 전역 setter를 호출하지 않습니다. 단독 cold 성공과 `FrameDriverSeparatesCoreTicker` 실행은 별도 증거입니다. 실제 MCP 요청의 tools/call→RunAsync→StartLegacyStructured→OnEndFrame 내부 순서는 소스 근거이며 개별 요청 내부 trace를 직접 수집한 것은 아닙니다.

전송 장애·timeout·재연결·만료 wait의 무재접수 계약은 변경되지 않은 Python 장애 주입 증거 범위입니다. 실제 네트워크 단절이나 native GPU 호출 내부의 강제 중단, 외부 DCC 전체 워크플로를 검증했다고 표현하지 않습니다. FBX SDK 재가져오기·머티리얼 연결·HDR/GPU 출력은 해당 Native8 테스트 범위에서 확인했습니다.

상대 `Saved/UnrealTools/V12` 링크는 이 검증 작업공간의 보존 증거를 가리키며 단독 플러그인 패키지에 증거 전체를 동봉했다는 뜻은 아닙니다. 재검증 진입점·sync·catalogue 문서 생성은 [MCP 안내](README.md), [CubeShot 안내](../Unreal_CubeShot/README.md), [FBX Exporter 안내](../Unreal_FBX_Exporter/README.md)와 네이티브 Tests를 참고하십시오. 강제 종료 fixture는 개발 자동화 빌드·소유 QA 경로·명시적 플래그가 필요합니다. 이 문서는 개별 게이트의 증거를 정리하며 전체 요구사항 완료 판정은 별도 최종 집계에 따릅니다.
