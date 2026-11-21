# AIplantRx 기기 간 인터페이스 (v1)

2026-11-21. 모든 기기가 같은 Wi-Fi에서 HTTP/1.1로 주고받는 약속입니다. S3, C3, 가짜 노드(`firmware/s3/tools/fake_nodes.py`)는 이 문서대로 만들었고, 센서 노드(devkit)와 CAM 펌웨어도 이 문서를 따릅니다. 바꿀 때는 `api` 숫자를 올리고 이 문서를 먼저 고칩니다.

공통 규칙:

- 응답에는 항상 `Content-Length`가 있다(esp_http_server의 `httpd_resp_send`). S3와 C3는 기기마다 연결 하나를 열어 두고 계속 쓴다(keep-alive).
- 기기의 HTTP 소켓은 TCP_NODELAY다(공통 모듈). 아니면 헤더와 본문이 따로 나가면서 본문이 상대의 지연 ACK(lwIP 250ms 타이머)를 기다렸다.
- 기기가 응답하는 동안은 UDP 알림이 몇 번 빠져도 S3가 "연결됨"으로 본다(카메라 프레임, `/state` 성공이 마지막 확인 시각을 갱신).
- JSON은 UTF-8. 모르는 필드는 무시하고, 없는 필드는 "모름"으로 다룬다(숫자는 생략하거나 `null`).
- 시각은 epoch 초(UTC). 하루 안의 위치는 0시부터 분(`at_min`, KST).
- 온도 °C, 습도·흙 수분 %, CO2 ppm, EC **mS/cm**, 물 양 ml.

## 1. 모든 기기 (firmware/common, aipnode)

| 요청 | 내용 |
| --- | --- |
| UDP 4210 브로드캐스트, 5초마다 | `{"role":"panel"|"oled"|"cam"|"node","name":"...","ver":"2.0.2","port":80}` |
| `GET /info` | `{"role","name","ver","host","ip","ssid","rssi","ap","uptime_s","reset","heap","heap_min","pending","ota":{"state","pct","err"}, ...기기별}` |
| `POST /wifi` | `{"ssid","pass"}` → `{"ok":true,"trying":"ssid"}`. 그 네트워크에서 주소를 받으면 저장하고, 30초 안에 못 받으면 원래 네트워크로 돌아간다 |
| `POST /ota` | `{"url":"https://...","sha256":"64자","size":N,"version":"1.0.2"}` → 202. 기기가 직접 받아 크기와 SHA-256을 확인한 뒤 재시작. 새 펌웨어는 120초 안에 Wi-Fi에 붙어야 확정, 아니면 롤백. 진행은 `/info`의 `ota` |
| `GET /reboot` | 다시 시작 |

`ota.state`: `idle`, `starting`, `downloading`, `verifying`, `restarting`, `fail`, `espota`. `ota.pct`는 쉬고 있을 때 -1. `reset`은 `esp_reset_reason()`(1 전원, 3 소프트웨어, 4 패닉, 5·6·7 워치독, 9 전압 부족). 받는 동안(`ota.state`가 `idle`·`fail`이 아닐 때)은 UDP 알림을 멈춘다. 다시 켜진 새 펌웨어는 확정될 때까지 `pending`이 true.

## 2. 센서 노드 (role `node`)

### GET /state

1초(S3)와 2초(C3)마다 읽는다. 값이 없는 센서는 필드를 빼거나 `null`.

```json
{
  "api": 1,
  "t": 1795259490,
  "air_c": 24.6, "rh": 58, "co2": 820,
  "soil_c": 22.8, "soil_pct": 47.0, "ec": 1.20, "ph": 6.4,
  "leaf_c": 23.1, "th_max": 26.8, "th_min": 21.0,
  "day": true,
  "light": true, "water": false, "nutr": false, "ph_pump": false,
  "capture": false,
  "water_ml_today": 120, "nutr_ml_today": 0, "ph_ml_today": 0,
  "last_water": 1795255200, "next_water": 0, "next_nutr": 1795514400,
  "events": [{"kind": "water", "at_min": 580, "len_s": 3, "ml": 40}],
  "sensors": {"scd41": true, "mlx": true, "soil": true},
  "profile": {
    "grade": "llm", "name_ko": "바질", "sci": "Ocimum basilicum",
    "temp_day": [20, 28], "temp_night": [18, 22], "rh": [40, 60],
    "soil": [40, 60], "ec": [1.0, 1.6], "ph": [6.0, 7.0],
    "light_on": "06:00", "light_off": "18:00",
    "water_interval_days": 1, "water_ml": 100,
    "nutr_interval_days": 7, "nutr_ml": 10
  }
}
```

- `leaf_c`: 열화상 32×24 중 식물 상자(열 9~22, 행 4~14, S3가 그리는 상자와 같음) 칸들의 중앙값. `th_max`, `th_min`은 전체 격자의 최고·최저.
- `day`: 지금 조명 시간표의 낮 구간인지(온도 목표를 낮·밤 중 무엇으로 볼지).
- `light`, `water`, `nutr`, `ph_pump`: 지금 켜져 있는지. `capture`: 촬영용 흰색 조명 중(펌프는 이때 돌지 않는다).
- `events`: 오늘(KST 0시부터) 펌프가 돈 기록, 오래된 것부터 최대 16개(넘으면 오래된 것을 버림). `kind`는 `water`, `nutr`, `ph`. 조명은 시간표로 그리므로 넣지 않는다.
- `last_water`: 마지막 물 주기를 시작한 시각. `next_water`: 시간표로 물을 줄 때(흙 센서가 빠졌을 때 `water_interval_days`마다 `water_ml`)의 다음 시각, 흙 수분으로 줄 때는 0.
- `sensors`: 센서마다 마지막 읽기가 성공했는지. 12V가 빠지면 `soil`이 false가 된다. 수위 플로트를 달면 `"tank": true`(물 있음)/`false`를 더한다(미정).
- `profile`: 지금 돌리고 있는 프로필(아래 PUT /profile 내용 그대로, 펌웨어 안전 범위로 자른 값). `grade`는 `llm` 또는 `default`.

### GET /thermal

MLX90640 32×24, int16 리틀엔디언, 0.01°C 단위, 행 순서. 1,536바이트. S3가 8Hz, C3가 4초마다 읽는다. 열화상이 응답하지 않으면 503.

### PUT /profile

S3가 식물을 찾은 뒤 보낸다. 센서 노드는 하드웨어 안전 범위로 다시 자르고 NVS에 저장한 뒤 `{"ok":true,"profile":{...잘린 값}}`을 돌려준다.

```json
{
  "grade": "llm", "t": 1795259490,
  "name_ko": "바질", "sci": "Ocimum basilicum", "family": "Lamiaceae", "score": 82,
  "temp_day": [20, 28], "temp_night": [18, 22], "rh": [40, 60],
  "soil": [40, 60], "ec": [1.0, 1.6], "ph": [6.0, 7.0],
  "light_on": "06:00", "light_off": "18:00",
  "water_interval_days": 1, "water_ml": 100,
  "nutr_interval_days": 7, "nutr_ml": 10
}
```

필수: `grade`, 범위 여섯 개(`[lo, hi]`), `light_on`, `light_off`, 물·물약의 주기와 양. 나머지(`name_ko`, `sci`, `family`, `score`, `t`)는 선택. 빠진 필수 필드나 잘못된 형식은 400.

센서 노드가 다시 자르는 하드웨어 안전 범위(LLM_ARCHITECTURE.md 4): 온도 10~32, 습도 20~95, 흙 수분 10~80, EC 0.3~3.0, pH 5.0~7.5, 조명 8~16시간(`light_on`을 두고 `light_off`를 옮김), 물 1~21일·20~300ml, 물약 3~30일·0~20ml. 최저가 최고보다 크면 서로 바꾼다.

`t`는 S3의 현재 시각(센서 노드에 RTC가 없어서 NTP가 안 될 때 쓴다). 권한: `llm`이면 물·양액·pH 모두 정상 제어, `default`면 물은 흙이 아주 마를 때만, 양액·pH는 멈춤(ARCHITECTURE.md 4.2).

### POST /pump

`{"pump":"water"|"nutr"|"ph","ml":20}` 또는 `{"pump":"water","s":3}`. 안전 상한(1회 최대 시간, 하루 최대 양) 안에서만 돌리고 정한 양이나 시간이 지나면 스스로 끈다. 펌프는 한 번에 하나만. 다른 펌프가 돌거나 `capture` 중이면 409. 응답 `{"ok":true,"ms":3000,"ml":39}`. 안전 상한을 넘는 요청은 상한까지만 돌린다. 409인 경우: 다른 펌프가 도는 중, `capture` 중, 그 펌프의 하루 상한을 다 씀, OTA 중. pH 펌프는 pH를 낮추는 쪽 한 방향만 넣는다.

### POST /light

`{"mode":"capture"}`: 흰색 최대, 펌프를 멈추고, 30초가 지나면 스스로 `schedule`로 돌아간다. `{"mode":"schedule"}`: 프로필 시간표대로. `{"mode":"on"|"off"}`: 수동(다음 시간표 경계까지). 응답 `{"ok":true,"mode":"capture","light":true}`.

### CAM 중계 (개발·복구용, S3는 쓰지 않음)

| 요청 | 내용 |
| --- | --- |
| TCP 4000 | esptool 중계. 연결되면 CAM을 다운로드 모드로 리셋하고 115200으로 바이트를 넘긴다. 끊기면 CAM을 다시 켠다 |
| `GET /cam/log` | CAM 콘솔 출력 최근 4KB (text) |
| `POST /cam/reset` | CAM 다시 켜기. 중계 중이면 409 |
| `POST /cam/send` | 본문 한 줄을 CAM 콘솔로 |
| `POST /cam/wifi` | devkit에 저장된 네트워크를 CAM 콘솔로 넘김 |

`/info`의 `cam_link`: `{"bridge","bridges","last_up","last_down","rx_bytes","rx_age_s","swap"}`. 자세한 내용은 firmware/node/README.md.

## 3. CAM (role `cam`)

| 요청 | 내용 |
| --- | --- |
| `GET :81/stream?fps=15` | 화면용 QVGA(320×240) JPEG를 CAM이 계속 밀어 보낸다. `multipart/x-mixed-replace;boundary=aipframe`, 부분마다 `Content-Length`. `fps` 1~20(기본 15). 한 번에 한 연결. S3가 쓰는 길이고, 연결이 안 되면 S3는 30초 동안 `/frame`으로 읽는다. 2026-11-21 측정: `/frame` 요청 방식은 S3에서 한 장에 80ms(약 11fps), 스트림은 13~15fps |
| `GET /frame` | 화면용 QVGA(320×240) JPEG 한 장. 같은 프레임을 두 번 주지 않는다(드라이버 버퍼를 한 번씩만 꺼냄) |
| `GET /capture?size=sxga&q=12` | 식별용 사진. `size`는 `uxga`(1600×1200, 기본) 또는 `sxga`(1280×1024), `q`는 OV2640 JPEG 품질(4~63, 낮을수록 좋음, 기본 12). S3는 `sxga`, `q=12`로 부른다(짧은 변 1024px로 PlantNet에 충분하고, 먼 서버로 올리는 시간이 UXGA의 약 1/3). 1~3초 걸린다. 실패하면 503, 모르는 값은 400. 찍는 동안 `/frame`은 기다린다(서버 작업이 하나) |

`/info`의 `camera`: `{"ok","err","psram","psram_free","frames","captures","fails","stream":{"on","clients","frames","fps"},"last_capture":{"w","h","bytes","ms"}}`. 카메라가 안 켜지면 `/frame`, `/capture`는 503이고 10초마다 다시 켠다.

## 4. C3 (role `oled`)

공통 요청만 연다. OLED 데이터는 센서 노드에서 직접 받으므로(UI.md 5) S3가 C3에 보낼 것은 없다. USB는 S3 업로드 중계 전용이라 로그를 내지 않는다.

## 5. 펌웨어 배포 (GitHub Releases)

릴리스 하나가 함께 시험한 세트다. `releases/latest/download/manifest.json`:

```json
{
  "release": "2.1.0",
  "api": 1,
  "published": "2026-11-21T10:51:52Z",
  "devices": {
    "s3":   {"version": "2.1.0", "file": "aiplantrx-s3-2.1.0.bin", "url": "https://github.com/KimMgyo/AIplantRx/releases/download/v2.1.0/aiplantrx-s3-2.1.0.bin", "size": 4334288, "sha256": "..."},
    "c3":   {"version": "1.0.0", "...": "..."},
    "cam":  {"version": "...", "...": "..."},
    "node": {"version": "...", "...": "..."}
  }
}
```

기기 버전은 따로 올라간다(고치지 않은 기기는 같은 버전이라 다시 설치하지 않는다). 매 릴리스에 모든 기기의 이미지를 다시 첨부한다. S3는 다른 기기를 먼저(기기마다 `POST /ota` 후 새 버전으로 다시 알릴 때까지 기다림), 자신을 마지막에 업데이트한다. 기기가 manifest보다 새 버전(개발 빌드)이면 건드리지 않는다.

이미지 경로 (S3 2.1.3부터):

- 기기의 `/info`에 `"ota":{"http":true}`가 있으면 S3는 `POST /ota`의 `url`로 자기 주소 `http://<S3>/fw/<기기>.bin`을 준다. S3가 GitHub에서 TLS로 받아(버퍼는 PSRAM) 같은 Wi-Fi 안에서 평문 HTTP로 넘긴다. 기기는 TLS를 하지 않고, 받은 이미지를 manifest의 SHA-256으로 확인한다.
- 없으면(옛 펌웨어) GitHub 주소를 그대로 준다.
- 이유(2026-11-22 측정): C3(PSRAM 없음)가 GitHub에서 직접 받으면 남는 메모리가 2.3KB까지 떨어지고, 여러 번 중 한 번꼴로만 성공했다(`begin: ESP_ERR_HTTP_CONNECT`, `download: ESP_FAIL`). S3를 거치면 최저 62KB가 남았고 한 번에 끝났다(1.35MB에 107초).
- 실패하면 S3가 5초 뒤 한 번 더 시도한다. 기기의 `ota.err`에는 실패 때 남은 메모리가 붙는다(`(heap N, min M)`).
