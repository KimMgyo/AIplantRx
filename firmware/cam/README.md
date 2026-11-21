# AIplantRx CAM 펌웨어 (AI-Thinker ESP32-CAM)

OV2640, PSRAM 4MB. S3 화면에 보낼 실시간 영상과 식물 식별용 사진을 맡습니다(INTERFACES.md 3).

- `GET /frame`: QVGA(320×240) JPEG, 최신 프레임.
- `GET /capture?size=uxga|sxga&q=N`: 식별용 사진 한 장. SXGA q12는 2026-11-21 측정으로 0.6초, 약 30KB였습니다(어두운 방 기준).
- 공통 모듈(firmware/common): Wi-Fi, UDP 알림, `/info`(`camera` 필드 포함), `/wifi`, `/ota`, espota, 롤백.
- 카메라 드라이버는 UXGA로 초기화해서 큰 사진용 버퍼를 잡습니다. 실시간 영상은 같은 드라이버를 QVGA로 낮춰 쓰고, 사진을 찍을 때만 한 장 동안 크기를 올렸다 내립니다. smartfarm은 QVGA로 초기화했는데, 거기서 크기를 올리면 프레임이 하나도 나오지 않았습니다.
- XCLK는 8MHz입니다. 20MHz에서는 카메라 DMA가 Wi-Fi를 굶겼습니다(smartfarm 측정: ping 387ms, 8MHz에서는 5ms).
- OTA 다운로드(`/ota`, espota) 중에는 카메라를 끄고, 실패하면 10초 뒤 다시 켭니다.
- UART0이 콘솔입니다: `wifi <ssid> <pass>` | `info` | `reboot` | `cam`. 케이스 안에서는 센서 노드(devkit)의 UART1에 이어져 있습니다.
- NVS의 예전 smartfarm 네트워크(`cam` 이름공간)는 처음 켤 때 `net`으로 옮겨집니다.

## 올리기

CAM에는 자기 USB가 없습니다. 방법은 세 가지입니다.

~~~
pio run -e cam-ota -t upload                    # Wi-Fi (espota, aiplantrx-cam.local). 평소 개발용
pio run -e cam-wired -t upload                  # 센서 노드 중계 (devkit TCP 4000). Wi-Fi OTA가 망가졌을 때, 첫 설치
python tools/ch340_flash.py COM27               # 책상 위에서 CAM-MB(CH340) USB로
~~~

- 평소에는 S3가 GitHub 릴리스에서 받아 설치합니다(`python tools/release.py ... --cam 0.1.2`).
- `cam-wired`는 devkit이 CAM과 4가닥(U0R, U0T, IO0, EN)으로 이어져 있고 12V(CAM 전원)가 켜져 있어야 합니다. 업로드 포트는 `socket://aiplantrx-node.local:4000`입니다. 이 PC에서 mDNS가 안 되면 `--upload-port socket://<devkit IP>:4000`으로 줍니다.
- 중계로 처음 설치한 CAM은 Wi-Fi가 없습니다. 이때 devkit에 `POST /cam/wifi`를 보내면 devkit의 네트워크가 CAM으로 넘어갑니다.

## CAM-MB(CH340) 문제

이 CAM-MB의 CH340(드라이버 3.9.2024.9)은 **지금 쓰는 속도와 같은 속도로 다시 설정하면 오류 31**("시스템에 부착된 장치가 작동하지 않습니다")을 냅니다. pyserial은 timeout을 바꿀 때마다 같은 설정을 다시 보내서, esptool과 pio 업로드가 포트를 여는 단계에서 실패합니다. 칩이 고장 난 것은 아니고, 리셋, 수신, 업로드 모두 됩니다. tools/ch340_flash.py가 이 실패를 무시하고 esptool을 돌립니다(2026-11-21, 115200으로 1.25MB를 72초에 올림). 460800은 쓰지 않습니다.
