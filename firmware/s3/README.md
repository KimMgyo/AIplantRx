# AIplantRx S3 펌웨어 (Waveshare ESP32-S3-Touch-LCD-7)

메인 화면과 설정 화면(UI.md 2·4장)을 LVGL 8.4로 그립니다. 테마 셋(유리·다크·라이트), Wi-Fi, 시계(NTP), 다른 기기 찾기(UDP 4210), OTA, AI 서버 연결 시험이 실제로 동작합니다. 카메라·열화상·센서 값은 해당 기기의 새 펌웨어가 아래 인터페이스로 응답하면 바로 화면에 나옵니다. 그 전에는 "기다리는 중"으로 둡니다.

## 빌드와 올리기

~~~
pio run -t upload                                        # USB (보드의 UART 스위치를 UART1에)
pio run -e s3-ota -t upload --upload-port <IP>           # Wi-Fi (espota, aiplantrx-s3.local)
pio run -e s3-relay -t upload --upload-port <C3 COM>     # C3 USB 중계 (ARCHITECTURE.md 4.4, 진입이 가끔 실패)
~~~

- 빌드 전에 tools/gen_assets.py가 자동으로 돈다. 소스의 문자열에서 한글을, F7_이름에서 아이콘을 읽어 폰트를 다시 만든다(바뀐 것만). 문구나 아이콘을 넣고 빌드만 하면 된다. lv_font_conv는 npx 캐시에서 찾는다(없으면 'npx -y lv_font_conv --help' 한 번).
- 식물 이름, AI 문장, SSID처럼 미리 알 수 없는 글은 KS X 1001 한글 2,350자를 다 넣은 폰트(f_r12~15, f_m15, f_b15, f_b32)로 그린다.
- 보드 구동(패널, 터치, LVGL 포트, RGB 바운스 버퍼 패치, board->begin() 전 300ms)은 smartfarm에서 측정해 둔 것을 그대로 가져왔다.
- 빌드 전에 scripts/patch_lvgl.py가 LVGL 8.4의 그리기 버그 네 군데를 고친다(아래 성능 절). 라이브러리를 새로 받아도 매번 다시 적용된다.
- -O2로 빌드한다(프레임워크 기본은 -Os). 폰트는 압축하지 않는다. 앱 크기 4.19MB(7.5MB 중 53%).
- NVS는 smartfarm과 같은 자리(0x9000)라 smartfarm이 저장한 Wi-Fi(net/ssid, net/pass)를 그대로 읽는다. 앱 영역은 7.5MB 두 개(partitions.csv).

## 개발 도구

같은 망에서 HTTP(포트 80), Wi-Fi가 없으면 USB 시리얼(115200)로 쓴다.

| HTTP | 시리얼 | 하는 일 |
| --- | --- | --- |
| GET /info | info | 버전, IP, 신호, 남은 메모리 |
| GET /perf[?reset=1] | perf | 프레임 시간: 화면 갱신 수, 그리기·대기 시간, 사각형·이미지·글자 시간, 카메라 받기·디코딩, 열화상 |
| GET /screen.bmp | shot | 지금 화면 800×480 (HTTP 약 1초, 시리얼 2Mbps 약 4초) |
| GET /tap?x=&y=&ms= | tap x y ms | 누르기 (설정 버튼은 430, 64) |
| GET /swipe?x=&y=&x2=&y2=&ms= | swipe x y x2 y2 ms | 밀기 |
| | aipx-download | ROM 다운로드 모드로 재시작(C3 중계가 esptool SYNC를 보면 보냄) |
| GET /set?llm_url=&llm_key=&pn_key= | | AI 서버 주소·키, PlantNet 키 저장 |
| GET /set?theme=0..2, ?screen=main, ?screen=settings&page=N | | 테마, 화면 전환(시험 스크립트용) |
| GET /update[?check=1 또는 ?install=1] | | 펌웨어 업데이트 상태(/info의 fw와 같음), 지금 확인, 찾은 버전 설치 |
| GET /reboot | reboot | 다시 시작 |

/info의 reset은 마지막 재시작 이유(1 전원, 3 소프트웨어, 4 패닉, 5·6·7 워치독, 9 전압 부족), stack_free는 작업별 스택 여유(바이트)다. 크래시는 플래시의 coredump 파티션에도 남는다(esptool read_flash 0xFF0000 0x10000).

- tools/s3ctl.py: 시리얼로 shot, tap, info.
- tools/llm_forward.py: Funnel을 켜기 전 시험용. 이 PC의 18080을 tailnet의 llama-server로 넘긴다. S3의 AI 서버 주소를 http://<이 PC IP>:18080으로 두면 된다.
- tools/fake_nodes.py [PC IP]: 가짜 CAM(:8091)과 센서 노드(:8092). 노드 펌웨어 없이 영상·열화상 부하를 건다.
- tools/perf_run.py [S3 IP] [이름]: 다크·유리 테마에서 메인 영상, 설정 열기, 사이드바·본문 스크롤, 닫기를 돌리고 /perf를 D:/voxl-build/perf_<이름>.json에 남긴다.

## 다른 기기에 기대하는 인터페이스

| 기기 | 요청 | 응답 |
| --- | --- | --- |
| 모든 기기 | UDP 4210 브로드캐스트, 5초마다 | {"role":"cam" 또는 "node" 또는 "oled","name":..,"ver":..,"port":80} (port는 없으면 80) |
| CAM | GET /frame | JPEG, QVGA(320×240). 더 크면 S3가 1/2·1/4로 줄여 푼다 |
| 센서 노드 | GET /state | JSON, 아래 |
| 센서 노드 | GET /thermal | 32×24 int16 리틀엔디언, 0.01°C 단위, 행 순서 (1,536바이트) |

/state 필드: air_c, rh, co2, soil_pct, ec, ph, heater, light, water, nutr(bool, 지금 켜졌는지), heater_min_today, water_ml_today, next_nutr(epoch 초), events([{kind: heater/water/nutr, at_min: 0시부터 분, len_min}]). 없는 필드는 비운다.

S3는 기기마다 HTTP/1.1 연결 하나를 열어 두고(keep-alive) 계속 쓴다. 응답에는 Content-Length가 있어야 한다(esp_http_server의 httpd_resp_send가 그렇게 보낸다). 카메라는 최대 12fps, 열화상은 8Hz, /state는 1초마다 묻는다.

S3도 같은 포트로 {"role":"panel",...}을 알린다.

## 구성

| 파일 | 내용 |
| --- | --- |
| src/main.cpp | 부팅 순서. 이후는 작업(task)만: LVGL은 core 1, 나머지는 core 0. Arduino loop 작업은 지운다. PANEL_CORE로 패널 인터럽트의 코어를 고른다(기본 1) |
| src/net.cpp | Wi-Fi(새 네트워크는 연결된 뒤에만 저장), NTP(KST), mDNS |
| src/nodes.cpp | 기기 찾기, CAM(받기와 디코딩이 두 작업으로 겹쳐 돈다, 화면 버퍼 3개), 센서 상태, 열화상 격자(32×24, 그림은 메인 화면이 그릴 때 보간) |
| src/fwupdate.cpp | GitHub Releases 업데이트: manifest 확인, HTTPS로 받기(인증서 묶음, 리디렉션), 크기·SHA-256 검사, 새 펌웨어 자기 점검과 롤백, 자동 확인·설치 |
| src/llm.cpp | llama-server 연결 시험. Bonsai 2는 chat_template_kwargs enable_thinking=false를 꼭 보낸다 |
| src/devsrv.cpp | 개발 도구, espota |
| src/perf.cpp | /perf 측정 |
| src/ui/ | theme(색표 셋), ui_assets(유리 배경·모서리 조각·AI 아이콘·색 막대를 부팅 때 계산), main_screen, settings_screen, sheets(안내, 키보드) |
| scripts/ | patch_rgb_bounce.py(smartfarm), patch_lvgl.py(아래), assets_hook.py |

## 메모리 (2026-11-12 실측)

- PSRAM은 4.9MB로 잡힌다(앱의 읽기 전용 데이터가 PSRAM으로 복사되기 때문). 화면 버퍼 1.5MB, 카메라 버퍼 0.46MB와 JPEG 버퍼 0.19MB. 열화상은 버퍼가 없다.
- 유리 테마는 배경 0.77MB와 사이드바 0.23MB를 더 쓴다. 다크·라이트로 바꾸면 돌려준다.
- 화면 캡처 버퍼 0.77MB는 캡처하는 동안만 잡는다.
- 다크에서 남는 PSRAM 약 1.97MB, 내부 RAM 약 120KB(폰트 압축을 풀고 LVGL 그리기 루프를 IRAM에 올린 만큼 줄었다).

## 성능 (2026-11-12 실측)

tools/perf_run.py, 가짜 노드(카메라 QVGA JPEG 8.5KB, 열화상 32×24)를 붙인 상태. 그리기는 화면 갱신 1번에 걸린 시간(패널 주기 26.8ms).

| 장면 | 전 | 후 |
| --- | --- | --- |
| 메인, 아무 일 없을 때 | 초당 5번 다시 그림 | 바뀔 때만 |
| 메인, 영상 | 영상 받자마자 크래시(영상은 0×0이라 그려진 적 없음) | 카메라 12fps·열화상 8Hz, 그리기 41ms, LVGL 52% |
| 다크, 사이드바 스크롤 | 36.1ms | 26.5ms |
| 다크, 본문 스크롤 | 52.3ms (12.7fps) | 42.8ms (17.3fps) |
| 유리, 사이드바 스크롤 | 63.1ms (12.5fps) | 28.2ms (20.1fps) |
| 유리, 본문 스크롤 | 83.1ms (9.3fps) | 54.7ms (12.5fps) |
| 부팅 | ipc1 스택 넘침으로 연속 재부팅(한 번은 25번) | 리셋 28번 모두 2.3초에 정상 |

이유와 고친 것:

- LVGL 8.4 그리기 버그(scripts/patch_lvgl.py). lv_obj_get_transformed_area()가 회전·확대가 없어도 영역을 사방 5px 넓혀, 8px 떨어진 두 영상 칸의 다시 그릴 영역이 겹쳐 하나로 합쳐졌다. 그 영역을 덮는 객체는 화면뿐이라 영상 한 장마다 화면 배경, 두 칸 배경과 그 위 전부를 다시 칠했다(갱신 51ms 중 불투명 채우기 20ms → 0.5ms). 직접 모드에서 맨 위 객체를 화면 전체 기준으로 찾던 것, 이미지 커버 검사가 엉뚱한 값을 읽던 것과 이미지가 결코 덮지 못하던 것도 고쳤다.
- 크래시 두 가지. 요청마다 지역 변수로 만든 HTTPClient가 keep-alive 연결을 붙잡은 채 사라진 WiFiClient를 닫아 힙이 깨졌다(Wi-Fi 작업에서 LoadProhibited, 진짜 ESP32-CAM에서도 같았을 것). 지금은 기기마다 연결 객체 하나를 계속 쓴다. 부팅 때는 GPIO 인터럽트 서비스가 1KB짜리 IPC 작업에서 등록되는 순간 GT911이 남긴 인터럽트가 터져 스택이 넘쳤다. setup() 맨 앞에서 미리 등록한다.
- 영상: 카메라 칸과 열화상 칸을 같은 갱신에서 함께 그리고(직접 모드의 버퍼 동기화 복사가 사라진다), 열화상은 32×24 격자에서 프레임버퍼로 바로 보간한다(PSRAM 그림 버퍼와 그 복사가 없다, 칸당 7ms). 카메라는 받기와 디코딩을 겹쳤다. 처음 프레임 전에 만든 이미지가 0×0으로 굳던 것도 고쳤다.
- 메인의 0.2초 타이머는 값이 같아도 스타일을 다시 넣어 초당 5번 화면을 그렸다. 바뀔 때만 넣는다. 설정은 카메라 fps나 Wi-Fi 스캔 때마다 페이지 전체를 다시 만들었다. 숫자와 스피너는 제자리에서 바꾸고, 다시 만들 때는 손을 떼고 스크롤이 멈춘 뒤에 한다.
- 터치는 10ms마다 읽는다(30ms는 26.8ms 패널 주기와 어긋나 스크롤이 고르지 않았다). GT911은 새 좌표가 있을 때만 인터럽트를 주므로, 그 사이의 읽기는 직전 상태를 그대로 준다(그대로 두면 손을 뗀 것으로 읽혀 드래그가 끊긴다).
- 버튼 눌림은 70% 투명도 대신 색 필터(투명도는 버튼을 따로 그려 섞는다).

남은 한계: 다크 본문 스크롤은 시간 대부분(31ms)이 화면 배경과 그 위 카드의 불투명 채우기다. PSRAM 쓰기 속도가 한계라 디자인을 바꾸지 않으면 더 줄기 어렵다. 패널 인터럽트를 core 0으로 옮기면(PANEL_CORE 0) 그리기가 10~15% 빨라지지만 Wi-Fi·디코딩과 같은 코어를 써서 기본은 1로 둔다. 무선 상태가 나쁠 때는(ping 50ms 안팎) 카메라 받기가 120ms까지 늘어 영상이 8fps로 떨어진다.

## 원격 업데이트 (GitHub Releases, 2026-11-21 시험)

- 릴리스: 저장소 루트에서 `python tools/release.py <버전> --notes "..."`. platformio.ini의 FW_VERSION을 바꾸고 빌드한 뒤, 이미지와 manifest.json을 KimMgyo/AIplantRx 릴리스로 올린다(KimMgyo 계정 토큰 사용). `--prerelease`로 올리면 보드는 보지 않는다.
- S3는 부팅 1분 뒤와 6시간마다 `releases/latest/download/manifest.json`을 읽는다. 받기는 HTTPS(프레임워크 인증서 묶음, GitHub 리디렉션 두 번), 크기와 SHA-256을 manifest와 맞춘 뒤에야 부팅 칸을 바꾼다.
- 새 펌웨어는 확인 대기로 부팅해 2분 안에 UI가 돌고 Wi-Fi에 붙으면 스스로 확정한다. 아니면 이전 칸으로 돌아가고, 돌아온 쪽이 설정과 /update에 되돌린 버전을 보여 준다.
- 시험 결과: v2.0.1 → v2.0.2 자동 설치. 메인 화면에서 10분(가동 602초) 손이 안 닿자 시작, 4.3MB를 약 80초에 받고 검증한 뒤 재시작(재시작 이유 3), 21초 뒤 확정. 받는 동안 내부 RAM 최저 41KB. 롤백: 확정하지 않는 시험 빌드(-DFWU_TEST_NO_CONFIRM)를 espota로 올리니 부팅 뒤 2분에 2.0.2로 돌아가고 "2.0.3-rbtest → 이전 버전"이 표시됨.
- 주의: 확정 전인 펌웨어 위에 또 올리면(espota 포함) 두 칸 모두 확정 안 된 펌웨어가 되어 돌아갈 곳이 없어진다. 새 펌웨어는 확정(/update의 pending=false)을 본 뒤에 다음 것을 올린다.
- espota로 2.0.0-dev → 2.0.1을 올릴 때 한 번 재시작 이유가 패닉(4)으로 남았다(옛 펌웨어가 OTA를 끝내고 재시작하던 중). GitHub 경로와 그 뒤 espota에서는 다시 나오지 않았다.

## 아직 없는 것

- 식물 찾기(PlantNet → LLM 프로필)와 AI 한마디 생성: CAM·센서 노드 새 펌웨어와 PlantNet 키가 있어야 시험할 수 있어 다음 단계.
- 다른 기기 OTA(POST /ota).

