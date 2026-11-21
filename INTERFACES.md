# AIplantRx 기기 간 인터페이스 (v1)

2026-11-21. 모든 기기가 같은 Wi-Fi에서 HTTP/1.1로 주고받는 약속입니다. S3, C3, 가짜 노드(`firmware/s3/tools/fake_nodes.py`)는 이 문서대로 만들었고, 센서 노드(devkit)와 CAM 펌웨어도 이 문서를 따릅니다. 바꿀 때는 `api` 숫자를 올리고 이 문서를 먼저 고칩니다.

공통 규칙:

- 응답에는 항상 `Content-Length`가 있다(esp_http_server의 `httpd_resp_send`). S3와 C3는 기기마다 연결 하나를 열어 두고 계속 쓴다(keep-alive).
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

`ota.state`: `idle`, `starting`, `downloading`, `verifying`, `restarting`, `fail`, `espota`.

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

- `day`: 지금 조명 시간표의 낮 구간인지(온도 목표를 낮·밤 중 무엇으로 볼지).
- `light`, `water`, `nutr`, `ph_pump`: 지금 켜져 있는지. `capture`: 촬영용 흰색 조명 중(펌프는 이때 돌지 않는다).
- `events`: 오늘(KST 0시부터) 펌프가 돈 기록. `kind`는 `water`, `nutr`, `ph`. 조명은 시간표로 그리므로 넣지 않는다.
- `sensors`: 센서마다 마지막 읽기가 성공했는지. 12V가 빠지면 `soil`이 false가 된다.
- `profile`: 지금 돌리고 있는 프로필(아래 PUT /profile 내용 그대로, 펌웨어 안전 범위로 자른 값). `grade`는 `llm` 또는 `default`.

### GET /thermal

MLX90640 32×24, int16 리틀엔디언, 0.01°C 단위, 행 순서. 1,536바이트. S3가 8Hz, C3가 4초마다 읽는다.

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

`t`는 S3의 현재 시각(센서 노드에 RTC가 없어서 NTP가 안 될 때 쓴다). 권한: `llm`이면 물·양액·pH 모두 정상 제어, `default`면 물은 흙이 아주 마를 때만, 양액·pH는 멈춤(ARCHITECTURE.md 4.2).

### POST /pump

`{"pump":"water"|"nutr"|"ph","ml":20}` 또는 `{"pump":"water","s":3}`. 안전 상한(1회 최대 시간, 하루 최대 양) 안에서만 돌리고 정한 양이나 시간이 지나면 스스로 끈다. 펌프는 한 번에 하나만. 다른 펌프가 돌거나 `capture` 중이면 409. 응답 `{"ok":true,"ms":3000}`.

### POST /light

`{"mode":"capture"}`: 흰색 최대, 펌프를 멈추고, 30초가 지나면 스스로 `schedule`로 돌아간다. `{"mode":"schedule"}`: 프로필 시간표대로. `{"mode":"on"|"off"}`: 수동(다음 시간표 경계까지).

## 3. CAM (role `cam`)

| 요청 | 내용 |
| --- | --- |
| `GET /frame` | 화면용 QVGA(320×240) JPEG. S3가 최대 12fps로 읽는다. 같은 프레임을 두 번 줄 수 있다(S3가 걸러 냄) |
| `GET /capture?size=uxga` | 식별용 사진. `uxga`(1600×1200, 기본) 또는 `sxga`. 1~3초 걸린다. 실패하면 503 |

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

기기 버전은 따로 올라간다(고치지 않은 기기는 같은 버전이라 다시 설치하지 않는다). 매 릴리스에 모든 기기의 이미지를 다시 첨부한다. S3는 다른 기기를 먼저(기기마다 `POST /ota` 후 새 버전으로 다시 알릴 때까지 기다림), 자신을 마지막에 업데이트한다.
