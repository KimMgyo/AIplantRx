# AIplantRx v2

카메라로 식물을 알아보고, 그 식물에 맞는 환경을 AI가 정해 펌프와 조명을 스스로 돌리는 작은 실내 재배 장치입니다.

1. **식별:** ESP32-CAM 사진 3장을 PlantNet에 보내 학명·속·과를 받는다.
2. **프로필:** 그 이름을 LLM(llama-server, Tailscale Funnel)에 보내 목표 환경과 시간표를 받고, S3가 안전 범위로 자른다.
3. **제어:** 센서 노드(ESP32 devkit)가 프로필대로 물·양액·pH 펌프와 조명을 돌린다. Wi-Fi나 화면이 끊겨도 계속한다.

전체 구성은 [ARCHITECTURE.md](ARCHITECTURE.md)에 있습니다.

## 기기

| 기기 | 역할 | 펌웨어 |
| --- | --- | --- |
| Waveshare ESP32-S3-Touch-LCD-7 | 메인·설정 화면, 영상, PlantNet·LLM 호출, 다른 기기에 지시 | [firmware/s3](firmware/s3) |
| ESP32-C3 SuperMini | OLED 5개, 외부 USB-C(전원·업로드 중계) | [firmware/c3_hwtest](firmware/c3_hwtest) (시험판) |
| ESP32-CAM | 화면용 QVGA, 식별용 UXGA 사진 | 아직 없음 |
| ESP32 devkit | 센서, 열화상, 펌프, NeoPixel, 제어 루프 | 아직 없음 |

[firmware/common](firmware/common)은 모든 기기가 함께 쓸 모듈(Wi-Fi, 기기 알림, /info, OTA)입니다(작업 중).

## 폴더

| 경로 | 내용 |
| --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 하드웨어·소프트웨어 구성, 배선, 인터페이스, 결정 기록 |
| [UI.md](UI.md) | 화면, OLED, AI 기능, Wi-Fi·OTA 설계 |
| [LLM_ARCHITECTURE.md](LLM_ARCHITECTURE.md) | 식별 → LLM 프로필 흐름과 값 검사 |
| [PLAN.md](PLAN.md) | 처음 계획(2026-11-11). 바뀐 점은 ARCHITECTURE.md 8장 |
| [cases](cases) | 3D 프린트 케이스 생성 스크립트(build123d), 미리보기, STL, G-code |
| [ui](ui) | 목업과 글꼴 |

## 펌웨어 배포

펌웨어는 이 저장소의 GitHub Releases로 배포합니다. S3가 `releases/latest/download/manifest.json`을 읽고, 각 기기가 직접 받아 검증합니다(UI.md 8장). 키(PlantNet, LLM)는 펌웨어에 들어가지 않고 기기의 NVS에만 저장됩니다.

## 이전 버전 (smartfarm)

2026년 8월까지의 이전 버전(v0.1.0~v0.4.5, 서버 + ESP-NOW 구조)은 [`legacy`](../../tree/legacy) 브랜치에 그대로 있습니다. 그때의 릴리스는 제목에 `[legacy]`를 붙여 프리릴리스로 남겨 두었습니다.
