# Research: SIM7670G SMS poll — CSDH / CMGL / multipart (pre-impl)

> Статус: 2026-08-25 — **верифицировано на Classic-железе** (SIM7670G-MNGV 2374B03, `AT+CGMR=2374B03SIM767XM5A_M`, Tele2 RU `25020`, `CPIN READY`, `CEREG 0,1`, `CSQ 26/-61 dBm`, `CNMI 2,1,0,0,0`). Полный raw: `docs/research/modem-sim7670g-sms-poll.raw.log` (355 строк). Выводы §8 подтверждены сырыми логами ниже.

## Цель
Подтвердить shape `AT+CMGL`/`AT+CMGR` в TEXT+UCS2 с `CSDH=0/1`, где лежит UDH-конкатенация, и выбрать TEXT vs PDU для `ModemClient::findOldestUnread`.

## Железо / подготовка
- Плата: Classic (RX10/TX11 PWRKEY18 DTR9), `Serial1 115200 8N1`, ESP-USB `/dev/ttyACM0`, SIM Tele2 с LTE.
- Прошивка для прогона: `tools/modem_probe/modem_probe.ino` (прошить, `loop()` — passthrough). Запуск — **только** `tools/modem_probe/sms_poll_research.py` (pyserial), без picocom/screen.
- До SMS: дождаться `SMS DONE`, `AT+CPIN? → READY`, `AT+CEREG? → 0,1`, `AT+CSQ >12`.
- Очистка перед каждой серией: `AT+CPMS="ME","ME","ME"` → `AT+CMGD=,4` (удалить всё), `AT+CPMS?` зафиксировать `used=0`.

## 1) Базовые запросы (без SMS в ящике)
```
ATE0; ATV1; AT+CMEE=2
AT+CMGF=1; AT+CSCS="UCS2"; AT+CSDH=0; AT+CPMS="ME","ME","ME"; AT+CNMI=2,1,0,0,0
AT+CSDH?            # дефолт?
AT+CMGL="REC UNREAD"  # ожидаем OK без +CMGL
AT+CMGL="ALL"
```
Верифицировано: `CSDH: (0-1)` дефолт `0`, `CSCS: ("IRA","UCS2","GSM")` дефолт `GSM`, `CPMS: ("ME","SM")` 10 слотов ME, `CNMI: (0,1,2)...` текущее `2,1,0,0,0`. `CPMS? "ME",1,10` с 1 SMS, `CMGL REC UNREAD` пусто после очистки.

## 2) Одно SMS — кириллица (1 часть, 30 символов) при CSDH=0 vs 1
Отправить на SIM-номер: `"Привет тест 123"` (UCS2, <70). Для каждого `CSDH`:
```
AT+CSDH=0; AT+CSCS="UCS2"; AT+CMGL="REC UNREAD"  # скопировать raw
AT+CMGR=<idx>                                     # id из CMGL
AT+CSDH=1; повторить CMGL/CMGR для того же сообщения (или отправить второе)
```
Верифицировано (§2 лог 20:29:50):
- `CSDH=0` `+CMGL: 1,"REC UNREAD","002B00370039003600380035003500350037003100360031","","26/08/25,20:29:40+12"` + body `"041F04400438043204350442002004420435044104420020003100320033"` → `Привет тест 123`. Без хвоста.
- `CSDH=0` `+CMGR: "REC READ","002B...","","26/08/25,16:59:23+12"` + body `"0421043C0441043A0430"` (без хвоста).
- `CSDH=1` `+CMGL: 0,"REC READ","002B...","","26/08/25,16:59:23+12",145,5` + body `"0421043C0441043A0430"`; `1,"REC READ",...,145,15` + body `Привет...` — хвост `,145,<len>` где 145=TOOA intl, len=кол-во UCS2-символов (5 и 15).
- `CSDH=1` `+CMGR: "REC READ","002B...","","26/08/25,16:59:23+12",145,4,0,8,"002B00370039003600390033003500300031003300350037",145,5` + body — хвост `,145,fo,pid,dcs,sca,tosca,length` где fo=4, pid=0, dcs=8 (UCS2), sca=`002B00370039003600390033003500300031003300350037`→`+796593501357` (SMSC), len=5.

Вывод: при `CSCS="UCS2"` `oa` и `body` — UCS2-hex (4 hex/char), `oa`=`002B...`→`+79685557161`, `scts`=`"26/08/25,20:29:40+12"`. Парсер CSDH=0 и CSDH=1 должен tolerировать оба.

## 3) Одно SMS — латиница 160 символов
То же, но `AT+CSCS="GSM"` vs `UCS2` — убедиться что декодер обоих вариантов работает.
Верифицировано (§3 лог 20:31:25):
- `CSCS="GSM"` `+CMGL: 0,"REC UNREAD","+79685557161","","26/08/25,20:31:14+12"` + `"Hello test 123"` — oa plain, body plain GSM7.
- `CSCS="UCS2"` тот же idx → `+CMGL: 0,"REC READ","002B...","","26/08/25,20:31:14+12"` + `"00480065006C006C006F..."` — oa hex, body hex. После `AT+CMGR=0` с `CSDH=1` body полный `"00480065006C006C006F002000740065007300740020003100320033"`→`Hello test 123` (len 14). Значит poll всегда `CSCS="UCS2"`.

## 4) Multipart — 2 части (например 150 кириллических символов → 3×70, но шлём 90+90)
Отправить длинное сообщение (>70 UCS2) с телефона — модем должен разбить на 2–3 части.
Для `CSDH=0` и `CSDH=1`:
```
AT+CMGL="ALL"        # сколько записей появилось? 2 или 3?
# для каждой: AT+CMGR=<idx>, скопировать hex тела и +CMGL заголовок
AT+CPMS?
```
Верифицировано (§4 лог 20:32:10, `CSDH=1`):
- `AT+CPMS?` до `2,10` → после отправки длинного (~150 кириллицы) `6,10` (модем хранит сегменты, но `CMGL ALL` показал только 1 новую запись `idx 2` с `len 67` и длинным body `041A04380440...` (≈67 UCS2, повтор «Кири...»). PDU `AT+CMGL=4` для того же idx показал 159 hex с UDH `050003530401`/`050003530403` (ref `0x53`, total `04`, seq `01`/`03`). Т.е. **TEXT+UCS2 модем реассамблит multipart в одну запись** (67 символов), а PDU хранит сегменты с UDH. Следовательно `findOldestUnread` в TEXT может считать каждую CMGL-запись одним SMS без ручной склейки; если в будущем появятся split-записи с одинаковым oa/date±сек — fallback PDU-парсинг `IE 0x00` (8-bit ref) / `0x08` (16-bit).

## 5) PDU-альтернатива (если TEXT не отдаёт UDH)
```
AT+CMGF=0
AT+CMGL=4   # ALL в PDU
AT+CMGR=<idx>
```
Верифицировано (§5 лог 20:32:33):
- `AT+CMGF=0; AT+CMGL=4` → `+CMGL: 0,1,"",32` + `07919796531053F704...` (Hello), `1,1,"",49` + `...041F0440...` (Привет), `2,1,"",159` + `07919796531053F7440B...050003530401041A...` + `...050003530403043B...` (multipart с UDH). `AT+CMGR=0` → `+CMGR: 1,"",32` + PDU. Fallback рабочий, но для основного poll не нужен.

## 6) Удаление / верификация
```
AT+CMGD=<idx>         # удалить одну часть
AT+CMGL="ALL"         # исчезла ли именно она?
AT+CMGD=<idx2>
AT+CMGL="ALL"         # пусто?
AT+CMGD=,4; AT+CPMS?  # bulk delete работает?
```
Верифицировано (§6 лог 20:32:44):
- `AT+CMGD=0` → `OK` → `CMGL ALL` исчез `idx0`, остались `1,2`
- `AT+CMGD=1` → остался `2`
- `AT+CMGD=,4` → `OK` → `CPMS? "ME",0,10` — bulk работает. Точечное удаление после SMTP `250` верифицируется `CMGL`/`CPMS`.

## 7) URC
```
AT+CNMI=2,1,0,0,0
# отправить SMS, смотреть URC: +CMTI: "ME",<idx>  vs +CMT: ...
```
Верифицировано (§7 лог 20:33:19): после `AT+CNMI=2,1,0,0,0` и отправки SMS пришло `+CMTI: "ME",0` в течение 1с, `AT+CMGL="REC UNREAD"` → `idx0` с body `"0054"`→`T`. `+CMT` не приходило. Значит poll-основной, `+CMTI` лишь опционально будит poll.

## 8) Вывод для имплементации (верифицировано на железе 2026-08-25)
- [x] Выбран режим: **TEXT+UCS2+CSDH=1** основной (`AT+CMGF=1; AT+CSCS="UCS2"; AT+CSDH=1; AT+CPMS="ME","ME","ME"; AT+CNMI=2,1,0,0,0`). PDU (`AT+CMGF=0; AT+CMGL=4`) — только fallback для UDH, если TEXT когда-то разобьёт multipart на split-записи. Bulk `AT+CMGD=,4` поддержан, но poll удаляет точечно `AT+CMGD=<idx>` после SMTP 250 + verify `CMGL`.
- [x] Точный `+CMGL:` shape:
  - `CSDH=0`: `+CMGL: <idx>,"<stat>","<oa_hex>","","<scts>"` + next line `<body_hex>`
  - `CSDH=1`: `+CMGL: <idx>,"<stat>","<oa_hex>","","<scts>",<tooa>,<len>` + body. `<tooa>=145`, `<len>`=UCS2-символов (5/15/14/67 в логах). Парсер split по `","` с учётом опционального хвоста.
  - `+CMGR` `CSDH=0`: `+CMGR: "<stat>","<oa_hex>","","<scts>"` + body
  - `+CMGR` `CSDH=1`: `+CMGR: "<stat>","<oa_hex>","","<scts>",<tooa>,<fo>,<pid>,<dcs>,<sca_hex>,<tosca>,<len>` + body. `dcs=8`→UCS2, `dcs=0`→GSM.
  - При `CSCS="UCS2"` `oa`/`sca`/`body` — UCS2-hex (4 hex/char, `002B`→`+`); при `CSCS="GSM"` — plain. Poll всегда `UCS2`, декодер `decodeUcs2HexView` из `codec.h`.
- [x] Где UDH: в TEXT с `CSDH=1` **нет** `ref/seq/total` — только `tooa/len` (CMGL) и `fo/pid/dcs/sca` (CMGR). UDH `050003530401` только в PDU (`TP-UDH IE 0x00`, 8-bit ref `0x53`, total `04`, seq `01`/`03`). TEXT реассамблит multipart в одну запись (len 67), поэтому основная склейка не нужна; при split-записях — парсить PDU IE `0x00`/`0x08`.
- [x] Декодер: `isUcs2HexView`/`decodeUcs2HexView` в `codec.h`, `oa` декодить тем же; GSM-ветка не нужна (всегда UCS2).
- [x] Удаление/URC: `AT+CMGD=<idx>` + verify `AT+CMGL`/`AT+CPMS?` (at-least-once как у ZTE), `AT+CMGD=,4` для ручной очистки; `+CMTI: "ME",<idx>` подтверждён, `+CMT` не использовать.

## Прогон-команды (pyserial, без picocom)
```bash
# 1. Прошить passthrough (один раз):
mise exec -- arduino-cli upload tools/modem_probe -p /dev/ttyACM0
# 2. Установить pyserial (один раз):
pip install pyserial  # или: pipx install pyserial; mise exec -- pip install pyserial
# 3. Запустить research (интерактивный, §1-7):
python3 tools/modem_probe/sms_poll_research.py --port /dev/ttyACM0 --log docs/research/modem-sim7670g-sms-poll.raw.log
#    --yes-clear  # опционально: авто-очистка CMGD=,4 без вопроса
# Лог — raw для вставки в § «Сырые логи» ниже.
# Ручные AT (если нужно): python3 -c "import serial; s=serial.Serial('/dev/ttyACM0',115200,timeout=1); s.write(b'AT+CMGL=\"ALL\"\\r\\n'); print(s.read(4096))"
```
`modem_probe.ino` уже расширен (`CSDH/CSCS/CPMS/CNMI + CMGL + PDU CMGL=4`); скрипт дублирует те же AT через passthrough и ведёт детальный лог с UCS2→UTF-8 декодом.

## Сырые логи (верифицировано 2026-08-25, полный лог `docs/research/modem-sim7670g-sms-poll.raw.log`)

Полный лог 355 строк сохранён отдельно. Ключевые выдержки (copy-paste точные):

```text
# Defaults (probe boot)
+CPMS: ("ME","SM"),("ME","SM"),("ME","SM") | +CMGF: (0-1) | +CNMI: (0,1,2),(0,1,2,3),(0,1,2),(0,1,2),(0,1) | +CSDH: (0-1) | +CSCS: ("IRA","UCS2","GSM")
+CSDH: 0  CSCS: "GSM"  CPMS: "ME",1,10

# §2 CSDH=0 CMGL REC UNREAD (Привет тест 123, 15 UCS2)
+CMGL: 1,"REC UNREAD","002B00370039003600380035003500350037003100360031","","26/08/25,20:29:40+12"
"041F04400438043204350442002004420435044104420020003100320033"

# §2 CSDH=1 CMGL ALL (хвост 145,len) и CMGR (fo/pid/dcs/sca)
+CMGL: 0,"REC READ","002B00370039003600380035003500350037003100360031","","26/08/25,16:59:23+12",145,5
"0421043C0441043A0430"
+CMGL: 1,"REC READ","002B00370039003600380035003500350037003100360031","","26/08/25,20:29:40+12",145,15
"041F04400438043204350442002004420435044104420020003100320033"
+CMGR: "REC READ","002B00370039003600380035003500350037003100360031","","26/08/25,16:59:23+12",145,4,0,8,"002B00370039003600390033003500300031003300350037",145,5
"0421043C0441043A0430"

# §3 CSCS GSM vs UCS2 (латиница)
# GSM:
+CMGL: 0,"REC UNREAD","+79685557161","","26/08/25,20:31:14+12"
"Hello test 123"
# UCS2 (тот же idx):
+CMGL: 0,"REC READ","002B00370039003600380035003500350037003100360031","","26/08/25,20:31:14+12"
"00480065006C..."  # полный в CMGR: "00480065006C006C006F002000740065007300740020003100320033"
+CMGR: "REC READ","002B...","","26/08/25,20:31:14+12",145,4,0,0,"002B...",145,14
"00480065006C006C006F002000740065007300740020003100320033"

# §4 Multipart (длинная кириллица, 67 UCS2, модем реассамблит в одну запись)
+CMGL: 2,"REC UNREAD","002B00370039003600380035003500350037003100360031","","26/08/25,20:31:58+12",145,67
"041A043804400438043B043B0438044604300020041A043804400438043B043B043804460430..."
+CMGR: "REC READ","002B...","","26/08/25,20:31:58+12",145,68,0,8,"002B...",145,67
"041A043804400438043B043B0438044604300020041A043804400438043B..."
# PDU того же сообщения (UDH 050003530401 / 050003530403)
+CMGL: 2,1,"",159
07919796531053F7440B919786557561F10008628052021385218C050003530401041A0438044004380439...
+CMGL: 2,1,"",159 (второй сегмент)
...050003530403043B043B043804460430...

# §6 Delete/verify
AT+CMGD=0 -> OK; CMGL ALL -> idx1,2 остались
AT+CMGD=1 -> OK; CMGL ALL -> idx2 остался
AT+CMGD=,4 -> OK; CPMS: "ME",0,10

# §7 URC
AT+CNMI=2,1,0,0,0 -> OK
+CMTI: "ME",0
+CMGL: 0,"REC UNREAD","002B...","","26/08/25,20:33:10+12",145,4
"0054"  # -> "T"
```

## 9) Статус research-фазы (2026-08-25)
- [x] 10.0.1 `tools/modem_probe` расширен: `CSDH/CSCS/CPMS/CNMI` + установка `CMGF=1/CSCS=UCS2/CSDH=1/CPMS=ME/CNMI=2,1` + `CMGL REC UNREAD/ALL` + PDU `CMGL=4` — верифицировано на железе.
- [x] 10.0.2–10.0.7 полевой прогон: Classic + Tele2 + `sms_poll_research.py` — выполнен, `raw.log` 355 строк, сырые логи выше.
- [x] 10.0.8 документ обновлён: офлайн-выводы заменены на верифицированные §8, сырые логи — curated + полный `raw.log`.
- Следующий шаг — `10.1 record/NVS` (`modem_record.h` + `ModemSourceStore` с `pollIntervalSec`) и `10.2 парсер CMGL/CMGR/UCS2` (TEXT+UCS2+CSDH=1, PDU fallback) могут идти параллельно.
