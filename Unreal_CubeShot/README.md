# CubeShot

Unreal Engine 5.8용 Editor 전용 플러그인입니다. Static Mesh를 앞·뒤·좌·우·위·아래에서 촬영해 프로젝트의 `/Game/CubeShot` 아래에 여섯 개의 Texture2D 에셋으로 저장합니다.

## 주요 기능

- `Tools > CubeShot`에서 단일 인스턴스의 도킹 가능한 에디터 탭을 엽니다.
- Static Mesh, Content Browser의 현재 선택, 에셋 드래그 앤 드롭을 지원합니다.
- 여섯 방향을 한 화면에서 미리 보고 옵션 변경을 실시간으로 반영합니다.
- 투명, 단색, Texture2D 배경을 지원합니다. Texture2D 배경은 비율을 유지해 중앙 cover/crop으로 합성합니다.
- Perspective와 Orthographic 투영, 거리, FOV, Ortho Width, 자동 프레이밍을 제공합니다.
- `256`부터 `4096`까지 정사각 출력 해상도를 지원합니다.
- 원본 Content 경로를 출력 경로에 미러링해 같은 이름의 메시가 서로 충돌하지 않게 합니다.
- 기존 Texture2D를 같은 UObject로 갱신해 그 에셋을 참조하는 다른 에셋의 참조를 보존합니다.

플러그인은 런타임 또는 Blueprint API를 추가하지 않습니다. 결과 에셋은 프로젝트 Content에 생성되며 플러그인 자체에는 Content가 포함되지 않습니다.

## 설치 및 프로젝트 연결

Haven 프로젝트는 `Haven.uproject`의 `AdditionalPluginDirectories`로 이 저장소를 검색합니다.

```text
C:\GitRepo\Unreal_Portfolio\Haven\
├─ Haven.uproject
└─ tool-unreal-plugin\
   └─ Unreal_CubeShot\
      └─ CubeShot.uplugin
```

`CubeShot.uplugin`은 `EnabledByDefault=true`인 Editor 모듈입니다. 다른 프로젝트에서 사용하려면 `Unreal_CubeShot`을 프로젝트의 `Plugins` 아래에 복사하거나, 그 상위 폴더를 `AdditionalPluginDirectories`에 추가합니다. 플러그인이 표시되지 않으면 프로젝트 파일을 다시 생성하고 Editor를 재시작합니다.

## 사용 방법

1. Unreal Editor 상단 메뉴에서 `Tools > CubeShot`을 선택합니다.
2. `Static Mesh`에서 촬영 대상을 선택하거나 `Use Selected`로 Content Browser의 Static Mesh를 가져옵니다.
3. 배경과 카메라 옵션을 조정하고 오른쪽 여섯 방향 미리보기를 확인합니다.
4. 필요하면 `Auto Fit`을 끄고 Camera Distance 또는 Ortho Width를 직접 조정합니다.
5. 출력 해상도를 선택합니다. 사용자 값은 `256~4096` 범위여야 합니다.
6. `Generate 6 Textures`를 누릅니다.

기본값은 Perspective, FOV 45°, Transparent 배경, 1024 해상도입니다. Solid Color의 초기값은 `#202020`입니다. 메시를 바꾸면 여섯 방향 모두가 프레임의 약 90% 안에 들어오도록 Camera Distance와 Ortho Width를 다시 계산합니다.

미리보기 출력은 방향당 최대 512px입니다. 옵션 변경은 약 150ms 뒤 최신 값으로 합쳐서 처리되며, 여섯 방향이 모두 준비된 뒤 한 번에 교체됩니다. 최종 생성 중에는 옵션과 일반 미리보기가 잠기고 캡처 단계에서만 취소할 수 있습니다.

## 방향 규칙

방향 이름은 메시 로컬 축을 기준으로 한 카메라 위치를 뜻합니다.

| 결과 | 카메라 위치 | 보는 방향 | 이미지 위쪽 |
| --- | --- | --- | --- |
| Front | `+X` | `-X` | `+Z` |
| Back | `-X` | `+X` | `+Z` |
| Right | `+Y` | `-Y` | `+Z` |
| Left | `-Y` | `+Y` | `+Z` |
| Top | `+Z` | `-Z` | `+X` |
| Bottom | `-Z` | `+Z` | `+X` |

촬영은 에디터 월드와 분리된 Preview Scene에서 이뤄집니다. 한 촬영 묶음은 고정된 노출·시간과 카메라 기준 조명을 사용합니다. Opaque와 Masked 머티리얼이 정확성 보장 범위이며 Translucent, Additive, Refraction은 best-effort 결과와 경고를 제공합니다.

## 출력 경로

`/Game` 아래의 원본 경로를 `/Game/CubeShot` 아래에 그대로 미러링합니다.

```text
/Game/Props/Furniture/SM_Chair
└─ /Game/CubeShot/Props/Furniture/SM_Chair/
   ├─ T_SM_Chair_Front
   ├─ T_SM_Chair_Back
   ├─ T_SM_Chair_Left
   ├─ T_SM_Chair_Right
   ├─ T_SM_Chair_Top
   └─ T_SM_Chair_Bottom
```

`/Game` 이외 mount의 메시를 지원할 때는 `/Game/CubeShot/_Mounts/<MountName>/...`에 미러링합니다. 잘못된 패키지 문자는 안전한 이름으로 변환됩니다.

기존 출력이 있으면 여섯 개 전체에 대해 한 번만 덮어쓰기 확인을 받습니다. 기존 경로에 Texture2D 이외의 에셋, redirector, 저장되지 않은 dirty 패키지가 있으면 작업을 중단해 데이터를 보호합니다.

## 이미지와 저장 정책

- `SCS_SceneColorHDR` 단일 캡처의 선형 RGB와 inverse opacity를 함께 읽어 RGB·알파 실루엣을 일치시킵니다.
- 2048 이하 결과는 2배 해상도로 캡처한 뒤 premultiplied RGBA로 다운샘플합니다. 2048 초과는 목표 해상도로 캡처합니다.
- Transparent 결과는 straight alpha로 저장하고 완전 투명 texel로 인접 전경색을 확장해 mip 또는 압축의 검은 테두리를 줄입니다.
- Solid/Texture 배경은 선형 공간에서 합성하고 알파 255로 저장합니다.
- power-of-two 해상도는 일반 mip을 생성합니다. non-power-of-two 값은 정확한 크기를 유지하며 mip을 만들지 않습니다.
- 출력 Texture2D는 BGRA8, sRGB, Default compression, World texture group, Clamp address, Virtual Texture 비활성 설정을 사용합니다.

최종 캡처는 먼저 `Saved/CubeShot/Staging/<Guid>`에 lossless 이미지로 기록됩니다. 여섯 장이 모두 성공하고 기존 패키지 checkout까지 끝난 뒤에만 Texture2D를 변경합니다. 캡처 실패나 그 이전 취소는 staging을 정리하고 UObject를 변경하지 않습니다.

Unreal의 여러 패키지 저장은 원자적이지 않습니다. 일부 저장만 실패하면 성공한 파일을 되돌리지 않고, 실패한 패키지는 dirty 상태로 두며 복구를 위해 staging을 유지합니다. 결과 대화상자에서 성공 경로와 실패 경로를 확인할 수 있습니다. 생성 작업은 Undo 대상이 아닙니다.

## 검증 및 제한

- 대상 엔진: Unreal Engine 5.8
- 확인 플랫폼: Win64 Editor, DX12/SM6
- 실제 캡처 테스트에는 정상 RHI가 필요하며 `-nullrhi`에서는 렌더링 결과를 검증할 수 없습니다.
- 매우 큰 메시와 4096 출력은 GPU 캡처와 readback 시간이 필요합니다. 구현은 방향별 버퍼를 저장 후 즉시 해제해 여섯 장의 HDR 버퍼를 동시에 보관하지 않습니다.

Unreal Editor의 Session Frontend에서 Automation Tests를 열고 `CubeShot.Editor`를 검색하면 방향 basis, Auto Fit, 이미지 합성, 경로·저장 정책, ViewModel 검증 테스트를 실행할 수 있습니다.
