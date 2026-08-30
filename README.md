# SmartFarm 온실 제어 및 모니터링 시스템

> **ESP32-S3 7인치 터치스크린 패널**, **ESP32-CAM 영상 스트리밍 노드**, **ESP32 다기능 센서 노드**, 그리고 **FastAPI 백엔드 서버**가 유기적으로 결합된 차세대 스마트팜 엣지-클라우드 통합 제어 플랫폼입니다.

---

## 목차

- [1. 시스템 개요](#1-시스템-개요)
- [2. 하드웨어 및 시스템 아키텍처](#2-하드웨어-및-시스템-아키텍처)
  - [2.1 메인 제어 패널 (Panel)](#21-메인-제어-패널-panel)
  - [2.2 카메라 스트리밍 노드 (Camera Node)](#22-카메라-스트리밍-노드-camera-node)
  - [2.3 온실 환경 센서 노드 (Sensor Node)](#23-온실-환경-센서-노드-sensor-node)
  - [2.4 백엔드 서버 및 AI 엔진 (Server)](#24-백엔드-서버-및-ai-엔진-server)
- [3. 핵심 엔지니어링 및 기술적 특징](#3-핵심-엔지니어링-및-기술적-특징)
  - [3.1 SemVer 순서 기반 3-Way 무선 OTA](#31-semver-순서-기반-3-way-무선-ota)
  - [3.2 ESP-NOW + WiFi 하이브리드 무선 네트워크](#32-esp-now--wifi-하이브리드-무선-네트워크)
  - [3.3 적응형 저지연 MJPEG 디코딩 & 코어 분리](#33-적응형-저지연-mjpeg-디코딩--코어-분리)
  - [3.4 안전한 엔드투엔드 TLS/HTTPS 통신](#34-안전한-엔드투엔드-tlshttps-통신)
  - [3.5 원격 콘솔 로깅 & 크래시 진단 대시보드](#35-원격-콘솔-로깅--크래시-진단-대시보드)
  - [3.6 소스 기반 동적 한글 글리프 생성 시스템](#36-소스-기반-동적-한글-글리프-생성-시스템)
- [4. 프로젝트 디렉터리 구조](#4-프로젝트-디렉터리-구조)
- [5. 빌드 및 배포 가이드](#5-빌드-및-배포-가이드)
  - [5.1 요구 사항](#51-요구-사항)
  - [5.2 펌웨어 빌드 (PlatformIO)](#52-펌웨어-빌드-platformio)
  - [5.3 백엔드 서버 실행 (FastAPI)](#53-백엔드-서버-실행-fastapi)
- [6. 운영 및 진단 인터페이스](#6-운영-및-진단-인터페이스)

---

## 1. 시스템 개요

SmartFarm 시스템은 온실 내부의 환경 데이터(CO2, 온도, 습도, 조도, 토양 수분, 열화상 분포)를 실시간 수집하고, 고해상도 RGB 카메라 영상과 함께 현장 터치스크린 패널 및 원격 클라우드로 전송합니다. 클라우드 서버의 AI 분석 엔진(PlantRx)과 연동하여 온실 환경 진단 및 자동 제어(환기 팬, 관수, 차광 등)를 수행하며, 현장 망(LTE 핫스팟/농가 WiFi)에 유동적인 환경에서도 안정적인 무선 OTA와 실시간 진단이 가능하도록 설계되었습니다.

---

## 2. 하드웨어 및 시스템 아키텍처

```
                           [ Cloudflare / Dokploy ]
                                      │
                        HTTPS (TLS 1.3 / ISRG Root X1)
                                      │
                                      ▼
                        ┌───────────────────────────┐
                        │   FastAPI Backend Server  │
                        │  (aiplantrx.xenv.cc:443)  │
                        │   - Telemetry & AI Judge  │
                        │   - GitHub Release Poller │
                        │   - Web Admin & Conlog    │
                        └─────────────┬─────────────┘
                                      │
                                HTTPS │ (Cellular / WiFi)
                                      ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                       Main Touch Panel (ESP32-S3)                         │
│  - Waveshare 7" 800x480 RGB LCD (LVGL v8 UI, Core 1)                     │
│  - Network Hub, WiFi / HTTPS Client, OTA Orchestrator (Core 0)            │
│  - 256KB PSRAM Log Ring, Thermal Camera I2C/SPI Processor                │
└───────────────────────▲───────────────────────────▲───────────────────────┘
                        │                           │
          TCP / MJPEG   │                           │ ESP-NOW (Telemetry)
          Streaming     │                           │ Dynamic WiFi (OTA)
                        │                           │
┌───────────────────────┴──────────┐     ┌──────────┴───────────────────────┐
│     Camera Node (ESP32-CAM)      │     │      Sensor Node (ESP32-WROOM)   │
│  - OV2640 MJPEG Streaming        │     │  - SCD41 (CO2, Temp, Humidity)   │
│  - Always WiFi STA (TCP Server)  │     │  - Lux, Soil Moisture Sensor     │
│  - Remote Stand-down OTA Agent   │     │  - Zero-WiFi (ESP-NOW only)      │
│                                  │     │  - On-Demand WiFi for OTA        │
└──────────────────────────────────┘     └──────────────────────────────────┘
```

### 2.1 메인 제어 패널 (Panel)
- **MCU**: Espressif ESP32-S3 (ESP32-S3-WROOM-1 / N16R8, 16MB Quad Flash + 8MB Octal PSRAM)
- **디스플레이**: Waveshare 7인치 800×480 RGB LCD 정전식 터치스크린
- **주요 역할**:
  - LVGL v8 기반 대화면 현장 모니터링 및 제어 UI
  - 센서 노드 및 카메라 노드 통신 중계 허브 (ESP-NOW + TCP MJPEG 수신)
  - 열화상 카메라 데이터 시각화 및 그리드 분석
  - 3개 기기 전체의 펌웨어 상태 감시 및 3-Way 무선 OTA 오케스트레이션
  - 256KB PSRAM 링버퍼 기반 무선 콘솔 로그 스트리밍 (`POST /v1/conlog`)

### 2.2 카메라 스트리밍 노드 (Camera Node)
- **MCU & 센서**: ESP32-CAM (AI-Thinker 모듈, OV2640 카메라, 4MB PSRAM)
- **주요 역할**:
  - 초당 15~20fps의 실시간 온실 내부 MJPEG 스트리밍 (TCP 포트 80)
  - 패널 요청 시 원격 OTA 펌웨어 다운로드 및 무중단 스트림 복구 (`camstream_stand_down` / `camstream_resume`)

### 2.3 온실 환경 센서 노드 (Sensor Node)
- **MCU & 센서**: ESP32 DevKit (SCD41 광학식 NDIR CO2/온습도 센서, 조도, 토양 수분 센서)
- **주요 역할**:
  - 평상시 **WiFi 연결을 완전히 차단**하고 저지연·무간섭 ESP-NOW 패킷만 브로드캐스트
  - 패널 채널 변경에 즉각 적응하는 무선 채널 호핑 아키텍처
  - 펌웨어 업데이트 명령 수신 시에만 일시적으로 WiFi STA로 전환하여 고속 다운로드 후 즉시 라디오 복구

### 2.4 백엔드 서버 및 AI 엔진 (Server)
- **스택**: Python 3.12, FastAPI, SQLite3 (WAL 모드), Uvicorn, Dokploy CI/CD 배포
- **주요 역할**:
  - 환경 텔레메트리 시계열 수집 및 식물 건강도/병해 진단 처방 (PlantRx)
  - GitHub Releases 자동 폴링(`ghfw`)을 통한 신규 펌웨어 배포 매니페스트 제공
  - 기기별 콘솔 로그 수집(`POST /v1/conlog`) 및 실시간 웹 뷰어(`/admin`)
  - 하드웨어 리셋/크래시 이력 분석 대시보드

---

## 3. 핵심 엔지니어링 및 기술적 특징

### 3.1 SemVer 순서 기반 3-Way 무선 OTA
기존 `app_elf_sha256` 해시 단순 비교 방식의 두 가지 치명적 한계점을 극복했습니다:
1. **동일 소스 재빌드 시 불필요한 재다운로드 방지**: 소스가 완전히 동일해도 재빌드 시 ELF 해시가 달라져 1MB 이상의 이미지를 매번 재설치하던 문제 해결.
2. **로컬 USB 빌드 롤백 트랩 방지**: 개발자가 USB로 플래시한 최신 로컬 빌드가 서버의 이전 배포 버전으로 덮어써지던 사고 차단.

- **원리 (`shared/fwver.h`)**:
  - 빌드 시점 `scripts/fwver.py`가 `git describe --tags --dirty --always`를 실행하여 컴파일 매크로 `FW_VERSION`으로 주입.
  - `Major.Minor.Patch-Commits-Dirty` 순서로 시맨틱 버전을 비교하여 **`NEWER`인 경우에만 펌웨어 업데이트를 수행**.
  - 로컬 빌드(예: `v0.4.5-2-g1a2b3c`)는 태그(`v0.4.5`)보다 항상 최신으로 정렬되어 자동 방어.
  - 버전 정보가 누락된 환경에서는 기존 해시 비교 방식으로 안전하게 폴백.

### 3.2 ESP-NOW + WiFi 하이브리드 무선 네트워크
- **센서 노드 비연결 원칙**: 상시 WiFi AP에 연결될 경우 AP 비콘/DTIM 수신으로 인한 전력 소모 및 채널 고정 문제가 발생합니다. 평상시 순수 ESP-NOW 통신만 유지하여 패널이 WiFi 채널을 변경하더라도 즉각 채널을 추적합니다.
- **온디맨드 무선 OTA**: 패널이 ESP-NOW 명령으로 OTA를 지시할 때만 센서 노드가 임시 WiFi 모드로 전환하여 펌웨어를 다운로드하고, 완료 즉시 라디오를 ESP-NOW로 복귀시킵니다.

### 3.3 적응형 저지연 MJPEG 디코딩 & 코어 분리
- **코어 분리**:
  - **Core 0**: WiFi 스택, lwIP 네트워킹, ESP-NOW 수신, MJPEG 풀러 및 인라인 JPEG 디코딩
  - **Core 1**: LVGL v8 GUI 렌더링, 정전식 터치 드라이버, 7인치 RGB LCD 프레임버퍼 플러시
- **적응형 디코드 가드 (`src/camnet.cpp`)**:
  - 정상적인 JPEG 디코딩은 약 2.8ms가 소요되나, 일시적인 PSRAM/버스 경합 시 40~76ms까지 튈 수 있습니다.
  - 고정 지연 대신 최근 디코딩 시간을 추적하는 적응형 듀티 사이클 가드를 도입하여, 디코딩이 길어질 경우 그에 비례하는 휴식 간격을 확보함으로써 Core 0의 lwIP 및 Task Watchdog 기아 현상을 방지합니다.

### 3.4 안전한 엔드투엔드 TLS/HTTPS 통신
- **ISRG Root X1 루트 CA 고정 핀닝 (`shared/srvconn.h`)**: 만료 주기가 짧은 리프 인증서 대신 Let's Encrypt의 루트 CA를 검증하여 인증서 갱신 시에도 연결성을 유지합니다.
- **TCP/TLS 레코드 단편화 방지**: 다수의 잘게 쪼개진 `print()` 호출 대신 스택 버퍼에서 HTTP 헤더를 단일 `write()`로 전송하여 Cloudflare 및 CDN 프록시에서의 TCP RST를 방지합니다.
- **TLS 싱글 세션 게이팅**: 내부 DRAM(약 50KB 소모) 고갈을 방지하기 위해 패널 내 단 하나의 TLS 세션만 활성화되도록 뮤텍스 게이트를 적용했습니다.

### 3.5 원격 콘솔 로깅 & 크래시 진단 대시보드
- **256KB PSRAM 로그 링 (`src/hlog.cpp`)**: 패널 내 모든 `hlogf()` 출력을 PSRAM 링버퍼에 적재하고, 백그라운드 태스크가 서버(`/v1/conlog`)로 청크 단위 동기화합니다.
- **인터럽트 안전 Seqlock 구조**: 대용량 PSRAM 복사 시 `taskENTER_CRITICAL`로 인터럽트가 장시간 차단되는 문제를 해결하기 위해, 커서 스냅샷만 락으로 보호하고 실제 메모리 복사는 락 외부에서 수행하는 Seqlock 패턴을 적용하여 LCD DMA 언더런과 인터럽트 워치독 리셋을 방지합니다.
- **NVS 부팅/리셋 이력 링버퍼 (`src/health.cpp`)**: 전원 차단(`power`), 패닉(`panic`), 워치독(`wdt`, `int_wdt`), 소프트웨어 재부팅(`sw`) 이력을 NVS 링버퍼에 보관하고 서버 웹 대시보드(`/admin`)에 실시간 노출합니다.

### 3.6 소스 기반 동적 한글 글리프 생성 시스템
- **`tools/gen_fonts.py`**: 패널 C++ 코드(`src/`, `include/`), 공유 헤더(`shared/`), 센서/카메라 노드 소스, 서버 Python 코드에 포함된 모든 한글 문자열 리터럴을 AST/정규식으로 자동 추출합니다.
- 사용된 글리프(259자 이상)만을 포함하는 최적화된 LVGL 비트맵 폰트(`font_kr_*.c`)를 생성하여 펌웨어 플래시 크기를 수백 킬로바이트 단위로 절감합니다.

---

## 4. 프로젝트 디렉터리 구조

```
smartfarm/
├── include/                  # 패널 펌웨어 전역 헤더 (UI, 네트워크, 헬스체크 등)
├── src/                      # 메인 터치 패널 (ESP32-S3) 소스 코드
│   ├── ui/                   # LVGL v8 기반 화면 UI 구현체
│   ├── camnet.cpp            # MJPEG 카메라 네트워크 풀러 및 디코더
│   ├── fwpull.cpp            # 패널 자체 HTTPS OTA 다운로더
│   ├── health.cpp            # 리셋 이유, 크래시 카운터, NVS 부팅 링버퍼 관리
│   ├── hlog.cpp              # 256KB PSRAM 콘솔 로그 링 및 원격 푸시
│   ├── nodeota.cpp           # 카메라/센서 노드 무선 OTA 오케스트레이터
│   └── plantrx.cpp           # 백엔드 서버 HTTPS 텔레메트리 통신
├── sensor_node/              # 센서 노드 (ESP32) 프로젝트
│   ├── platformio.ini        # 센서 노드 빌드 설정
│   └── src/                  # SCD41 센서 제어, ESP-NOW 브로드캐스트, 온디맨드 OTA
├── esp32cam-streamer/        # 카메라 노드 (ESP32-CAM) 프로젝트
│   ├── platformio.ini        # 카메라 노드 빌드 설정
│   └── src/                  # OV2640 드라이버, TCP MJPEG 서버, 원격 OTA 에이전트
├── shared/                   # 전 기종 및 서버 공통 공유 프로토콜 헤더
│   ├── fwver.h               # SemVer 버전 순서 정렬 및 파서
│   ├── nodeproto.h           # ESP-NOW 노드 메시지 패킷 규격 및 UI 정본 어휘
│   ├── srvconn.h             # TLS 연결 관리자 및 ISRG Root X1 루트 CA
│   └── srvurl.h              # 초경량 URL 파서
├── server/                   # FastAPI 백엔드 서버
│   ├── app/                  # FastAPI 라우터, DB 스토리지, GitHub 릴리스 동기화
│   ├── static/               # 관리자 콘솔 웹 UI (/admin)
│   └── tests/                # 서버 통합 테스트 스위트 (20개 테스트)
├── scripts/                  # 빌드 파이프라인 스크립트 (버전 주입, RGB 패치)
│   ├── fwver.py              # PlatformIO pre: git describe 버전 자동 주입
│   └── patch_rgb_bounce.py   # ESP32-S3 RGB 디스플레이 캐시 무효화 자동 패치
├── tools/                    # 유틸리티 도구
│   └── gen_fonts.py          # 소스 코드 기반 동적 한글 폰트 생성기
└── platformio.ini            # 메인 터치 패널 빌드 설정 (ESP32-S3 N16R8)
```

---

## 5. 빌드 및 배포 가이드

### 5.1 요구 사항
- **Firmware**: [PlatformIO Core](https://platformio.org/) (CLI 또는 VSCode 확장)
- **Server**: Python 3.12 이상, pip / venv
- **Git**: Git 태그 객체 참조를 위해 전체 클론(`fetch-depth: 0`, `fetch-tags: true`) 권장

### 5.2 펌웨어 빌드 (PlatformIO)

#### 1) 메인 제어 패널 (ESP32-S3)
```bash
# 일반 USB 시리얼 빌드 및 플래시
pio run -e esp32-s3-touch-lcd-7 -t upload

# 로컬 무선 OTA 플래시 (smartfarm-panel.local)
pio run -e esp32-s3-touch-lcd-7-ota -t upload
```

#### 2) 센서 노드 (ESP32)
```bash
cd sensor_node
pio run -t upload
```

#### 3) 카메라 노드 (ESP32-CAM)
```bash
cd esp32cam-streamer
pio run -t upload
```

> **참고**: 모든 빌드는 `scripts/fwver.py`를 통해 현재 Git 상태(`git describe --tags --dirty --always`)를 `FW_VERSION` 매크로로 자동 주입받습니다.

### 5.3 백엔드 서버 실행 (FastAPI)

```bash
cd server

# 가상환경 생성 및 활성화
python -m venv .venv
source .venv/bin/activate  # Windows: .venv\Scripts\activate

# 의존성 설치
pip install -r requirements.txt

# 서버 단위/통합 테스트 실행
python tests/test_admin_state.py
python tests/test_conlog.py
python tests/test_crash_visible.py

# 로컬 개발 서버 구동 (포트 8000)
uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

---

## 6. 운영 및 진단 인터페이스

### 6.1 웹 관리자 대시보드 (`/admin`)
브라우저에서 `https://aiplantrx.xenv.cc/admin`으로 접속하여 다음 기능을 모니터링하고 제어할 수 있습니다:

- **기기별 실시간 가동 상태 및 펌웨어 버전**: 패널, 카메라 노드, 센서 노드의 실행 중인 해시/버전과 최신 릴리스와의 일치 여부 확인 (`이미 최신` / `업데이트 필요`)
- **원클릭 원격 OTA 트리거**: 기기별 개별 무선 OTA 업데이트 시작
- **부팅 및 리셋 이력 타임라인**: 전원 리셋(`power`), 워치독 리셋(`wdt`, `int_wdt`), 패닉(`panic`) 등 최근 8회 이상의 부팅 원인 히스토리 조회
- **하드웨어 패닉 코어덤프 상세**: 크래시 발생 태스크명, 예외 PC 주소, 콜스택 백트레이스 정보 표시
- **실시간 무선 콘솔 뷰어 (`/v1/conlog`)**: 현장에 물리적 시리얼 포트 연결 없이도 패널의 256KB 로그 링버퍼 실시간 스트리밍 확인

### 6.2 릴리스 및 CI/CD 워크플로
저장소에 `v*` 태그(예: `v0.4.5`)를 푸시하면 GitHub Actions 파이프라인(`.github/workflows/firmware.yml`)이 동작하여:
1. 3개 기기(패널, 노드, 카메라)의 펌웨어 바이너리를 자동 빌드합니다.
2. GitHub Release에 바이너리와 파티션 테이블을 게시합니다.
3. 백엔드 서버의 `ghfw.py`가 15분 주기로 릴리스를 자동 감지하여 신규 매니페스트와 버전을 배포합니다.
4. 관리자 페이지나 현장 패널에서 버튼 하나로 전체 온실 기기를 최신 버전으로 순차 업데이트할 수 있습니다.

---

## 7. 라이선스

본 프로젝트의 소스 코드는 자체 비공개/스마트팜 내부 운영 규정을 따릅니다.
