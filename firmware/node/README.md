# AIplantRx 센서 노드 펌웨어 (ESP32 devkit)

HG-ESP32-DEVKIT V4(CP2102N). 재배부 케이스 안에서 센서, 펌프, 링 조명을 맡고, ESP32-CAM을 유선으로 다시 써 넣는 통로가 됩니다(ARCHITECTURE.md 2.3).

지금 버전(0.1.x)은 **센서 없이** 돕니다.

- 공통 모듈(firmware/common): Wi-Fi, UDP 알림, `/info`, `/wifi`, `/ota`(GitHub), espota, 롤백.
- INTERFACES.md 2장: `/state`는 센서를 모두 "없음"으로 보냅니다. `/thermal`은 503을 돌려줍니다. `PUT /profile`은 안전 범위로 잘라 NVS에 저장합니다. `POST /pump`와 `POST /light`는 안전 상한 안에서 동작합니다.
- 스스로 도는 제어(흙 수분으로 물 주기, 물·양액 시간표)는 아직 없습니다. 조명만 프로필의 시간표를 따릅니다. 시계는 NTP를 쓰고, NTP가 안 되면 S3가 보낸 `t`를 씁니다.
- 펌프 유량은 아직 재지 않았습니다. 지금은 펌프 셋 모두 13 ml/s로 둡니다(`control.cpp`의 `ML_PER_S`). 펌프를 달면 실제로 재서 고칩니다.

## 빌드와 올리기

~~~
pio run -e node -t upload --upload-port COMxx    # devkit USB-C (처음 한 번: 파티션 표를 씀)
pio run -e node-ota -t upload                     # Wi-Fi (espota, aiplantrx-node.local)
~~~

평소에는 S3가 GitHub 릴리스에서 받아 설치합니다(`python tools/release.py ... --node 0.1.2`).

처음 Wi-Fi는 USB 콘솔(115200)로 줍니다: `wifi <ssid> <pass>`. 그 뒤로는 S3 설정 화면의 "모든 기기 옮기기"(`POST /wifi`)로 옮깁니다.

## USB 콘솔

`wifi <ssid> <pass>` | `info` | `reboot` | `state` | `camlog` | `camreset` | `camwifi` | `camswap on|off` | `cam <한 줄>`

## CAM 중계 (cambridge.cpp)

배선: devkit 18 → 1kΩ → CAM U0R, CAM U0T → 1kΩ → devkit 19. devkit 25와 33은 각각 PC817C를 켜서 CAM IO0과 CAM EN을 GND로 당깁니다(HIGH = CAM 핀 LOW).

| 요청 | 내용 |
| --- | --- |
| TCP 4000 | esptool 중계. 연결되면 CAM을 다운로드 모드로 리셋하고(IO0 LOW + EN 펄스) 115200으로 바이트를 넘깁니다. 연결이 끊기거나 30초 동안 조용하면 IO0을 놓고 CAM을 다시 켭니다 |
| `GET /cam/log` | CAM 콘솔 출력 최근 4KB |
| `POST /cam/reset` | CAM 다시 켜기 |
| `POST /cam/send` | 본문 한 줄을 CAM 콘솔로 (`info`, `reboot`, `cam`) |
| `POST /cam/wifi` | devkit에 저장된 네트워크를 CAM에 줌 |

`/info`의 `cam_link`에서 상태를 봅니다. `rx_bytes`가 0이면 CAM 출력이 들어오지 않는 것입니다(CAM 전원, U0T 선 확인). `swap`은 TX/RX를 반대로 단 경우를 확인할 때 씁니다.

CAM 쪽 사용법은 firmware/cam/README.md에 있습니다. 중계는 115200만 됩니다(esptool의 속도 변경을 따라가지 않음).
