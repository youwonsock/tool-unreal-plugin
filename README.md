# tool-unreal-plugin

Unreal Engine 프로젝트에서 재사용하는 Editor 플러그인을 보관하는 저장소입니다.

## 현재 플러그인

| 플러그인 | 설명 | 문서 |
| --- | --- | --- |
| Unreal_FBX_Exporter | Content Browser에서 Static Mesh와 PBR 텍스처 세트를 FBX로 내보내는 Editor 플러그인 | [플러그인 README](<./Unreal_FBX_Exporter/README.md>) |

## 폴더 운영

- 각 플러그인은 독립된 최상위 폴더에 보관합니다.
- 플러그인 폴더에는 uplugin manifest, Source, README를 함께 둡니다.
- Unreal Editor가 생성하는 Binaries, Intermediate, Saved 등의 결과물은 플러그인별 무시 규칙으로 제외합니다.
- 플러그인 사용법과 엔진 버전 요구사항은 각 플러그인의 README에 기록합니다.

## Haven 프로젝트 연결

Haven은 다음 경로에 있는 플러그인을 사용합니다.

~~~text
C:\GitRepo\Unreal_Portfolio\Haven\
├─ Haven.uproject
└─ tool-unreal-plugin\
   └─ Unreal_FBX_Exporter\
~~~

Haven.uproject의 AdditionalPluginDirectories에는 tool-unreal-plugin이 등록되어 있습니다. 플러그인이 표시되지 않으면 프로젝트 파일을 다시 생성하고 Unreal Editor를 재시작합니다.

새 플러그인을 추가할 때는 이 README의 현재 플러그인 목록과 해당 플러그인의 README를 함께 갱신합니다.
