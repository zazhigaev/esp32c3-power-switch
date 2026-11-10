# IPC-R300 Mini-IPMI & EC Hardware Watchdog

Компактная плата удаленного аппаратного управления и мониторинга сервера (mini-IPMI) на базе микроконтроллера **ESP32-C3 Super Mini**, предназначенная для установки внутри шасси материнской платы **IPC-R300**.

Прошивка реализует пассивный перехват и строчный анализ сервисного UART-потока встроенного контроллера (EC / CPLD платформы AMI Aptio V / Intel FSP), аппаратный сторожевой таймер зависания хоста (Watchdog FSM), гальванически развязанное управление кнопками питания и сброса, защищенный веб-интерфейс (Basic Auth с возможностью смены пароля и безопасного завершения сессии), а также публикацию телеметрии и прием команд по протоколу MQTT для интеграции в Home Assistant.

---

## 1. Ключевые возможности

* **Пассивный EC/POST декодер:** Не требует установки драйверов, ОС-демонов или агентского ПО на стороне хоста. Контроллер перехватывает фазы инициализации вторичных шин (`PWR init state`), дежурные режимы ACPI (`Power state S4/S5`) и контрольные точки UEFI BIOS.
* **Аппаратный FSM Watchdog (восстановление хоста):**
  * Если BIOS застывает на этапе инициализации оборудования (`HOST_BOOTING`) дольше 30 секунд, контроллер подает короткий импульс питания (200 мс) для выключения.
  * Если система не выключилась за 5 секунд — выдается аппаратный сброс (Reset 200 мс).
  * При подтвержденном выключении через 3 секунды подается чистый импульс включения.
* **Безопасный автозапуск (Auto-Power):** При подаче питания на стойку/БП контроллер выжидает 5 секунд. Если хост уже работает, загружается или находится в цикле восстановления — импульс блокируется.
* **Потокобезопасная архитектура FreeRTOS:**
  * Задачи асинхронного веб-сервера (`AsyncTCP`) и основной цикл (`loopTask`) строго изолированы.
  * Все команды на изменение питания и запись в NVS передаются через очередь `cmdQueue`.
  * Чтение и обновление системной конфигурации защищены мьютексом `cfgMutex`.
  * Кэш учетных данных авторизации защищен отдельным легковесным мьютексом `authMutex`, а смена пароля подтверждается клиенту синхронно через бинарный семафор `authDoneSem`.
* **64-битный монотонный Uptime:** Подсчет непрерывного времени работы хоста и ESP32 осуществляется через нативный таймер `esp_timer_get_time()`, исключающий сброс в ноль при переполнении 32-битного `millis()` каждые 49.7 суток.
* **Аппаратный аварийный сброс (GPIO9 / BOOT):** Возможность мгновенного возврата учетных данных веб-интерфейса к заводским значениям по удержанию физической кнопки при включении.

---

## 2. Аппаратная схема подключения

### Назначение выводов ESP32-C3 Super Mini

| Пин ESP32-C3 | Направление | Назначение | Описание подключения |
| :--- | :---: | :--- | :--- |
| **GPIO1** | Выход | Power SW | Резистор 150 Ом $\to$ Анод оптопары PC817 (1) |
| **GPIO3** | Выход | Reset SW | Резистор 150 Ом $\to$ Анод оптопары PC817 (2) |
| **GPIO4** | Вход | UART1 RX | TX сервисного разъема EC платы IPC-R300 |
| **GPIO5** | Выход | UART1 TX | RX разъема EC (зарезервировано) |
| **GPIO8** | Выход | Светодиод платы | Синий LED платы (индикация режимов и сброса) |
| **GPIO9** | Вход | Кнопка BOOT | Аппаратная кнопка на плате (подтяжка к 3.3V) |
| **GND** | Питание | Общий минус | GND дежурного питания сервера |
| **5V / VIN** | Питание | +5V Standby | Дежурная линия +5VSB источника питания |

### Принципиальная схема включения

```
               ESP32-C3 Super Mini               PC817              IPC-R300 FP Header
             +---------------------+          +---------+          +-------------------+
             |                     |          |  1   4  |          |
             |               GPIO1 +--[150R]--+--|>|-+  +----------+ PWR_SW+ (+3.3V)   |
             |                     |          |      |  |          |                   |
             |                 GND +----------+------+--+----------+ PWR_SW- (GND)     |
             |                     |          |  2   3  |          |                   |
             |                     |          +---------+          |                   |
             |                     |                               |                   |
             |                     |          +---------+          |                   |
             |               GPIO3 +--[150R]--+--|>|-+  +----------+ RESET+ (+3.3V)    |
             |                     |          |      |  |          |                   |
             |                 GND +----------+------+--+----------+ RESET- (GND)      |
             |                     |          |  2   3  |          +-------------------+
             |                     |          +---------+
             |                     |
             |   GPIO4 (UART RX)   +<------------------------------ TX EC Debug Header
             |                 GND +------------------------------- GND Материнской платы
             |                     |
             |   GPIO9 (BOOT BTN)  +--[SW]--+ (на плате)
             |                 GND +--------+
             +---------------------+
```

> **Важно по токоограничивающим резисторам:** На управляющих входах оптопар (GPIO1, GPIO3) необходимо применять токоограничивающие резисторы сопротивлением **150 Ом**. При логическом уровне 3.3 В номинал 150 Ом задает ток через инфракрасный диод около $13\dots14\text{ мА}$, обеспечивая переход фототранзистора PC817 в режим глубокого насыщения ($R_{CE} < 10\text{ Ом}$) для преодоления низкоомных подтяжек на материнской плате.

---

## 3. Таблица контрольных точек BIOS (AMI / Intel FSP)

| POST (HEX) | Статус платформы | Описание аппаратного этапа |
| :--- | :--- | :--- |
| `0x00000000` | **OS Running / Heartbeat** | Управление передано ядру ОС (S0), система активна |
| `0x0000001C` | **Power State S4/S5 (Off)** | Завершено снятие вторичных шин питания (дежурный режим) |
| `0x00000024` | **RAM SPD Addressing** | Опрос микросхем SPD модулей оперативной памяти |
| `0x00000025` | **RAM Training MRC** | Инициализация и калибровка контроллера памяти Intel MRC |
| `0x00000055` | **RAM Error / Not Detected** | Аппаратный сбой памяти: модуль не обнаружен / ошибка MRC |
| `0x00000071` | **PCH SATA Controller Init** | Инициализация SATA-контроллера чипсета |
| `0x00000075` | **Storage Detection** | Поиск и идентификация накопителей (SSD/HDD) |
| `0x00000077` | **SATA Devices Configured** | Накопители успешно опознаны и настроены |
| `0x00000079` | **Storage / SSD Failure** | Аппаратный сбой инициализации дисковой подсистемы |
| `0x00000088` | **PCIe Bus Enumeration** | Сканирование и конфигурирование шины PCI Express |
| `0x00000091` | **PCIe Subsystem Setup** | Инициализация устройств PCIe (сетевые чипы Intel i211/i225) |
| `0x00000095` | **PCIe Resource Allocation** | Распределение ресурсов адресации и прерываний PCIe |
| `0x00000098` | **Console Input Devices** | Инициализация интерфейсов ввода (USB HID / клавиатура) |
| `0x00000099` | **BDS: Super I/O Hand-off** | Передача управления BDS-секвенсеру загрузки |
| `0x0000009C` | **USB Controllers Ready** | Контроллеры USB xHCI готовы к работе |

---

## 4. Безопасность, аутентификация и сброс через GPIO9

### Доступ к веб-интерфейсу

Все страницы управления, эндпоинты импульсов и REST API защищены HTTP Basic Authentication.
* **Заводские учетные данные по умолчанию:**
  * **Логин:** `admin`
  * **Пароль:** `admin`
* При первичной настройке в режиме точки доступа (`IPMI-Config-AP`, IP `192.168.4.1`) мастер настройки сети открыт без пароля.

### Синхронная смена пароля и завершение сессии

1. В карточке **Security / Password** можно задать новый логин (1–31 символов) и пароль (4–63 символов).
2. Запрос отправляется на `/api/settings/auth`. Команда передается в `cmdQueue`, а HTTP-задача синхронно ожидает подтверждения через семафор `authDoneSem`. Браузер получает ответ `200 OK` только тогда, когда новые данные сохранены в NVS и зафиксированы в оперативной памяти под защитой `authMutex`.
3. После смены учетных данных происходит автоматический редирект на `/logout`.
4. Кнопка **Logout** в интерфейсе отправляет клиенту сброс сохраненных учетных данных без застревания на технической странице 401.

### Аварийный сброс учетных данных (Кнопка BOOT на GPIO9)

Если логин или пароль от веб-интерфейса утеряны:

1. Обесточьте плату ESP32-C3.
2. Зажмите физическую кнопку **BOOT** на плате ESP32-C3.
3. Подайте питание (или кратковременно нажмите Reset), продолжая удерживать **BOOT**.
4. Светодиод (GPIO8) перейдет в режим частого стробирования (100 мс).
5. Удерживайте кнопку **3 секунды**, пока светодиод не загорится непрерывно на 1 секунду.
6. Отпустите кнопку. Контроллер очистит ключи `http_user` и `http_pass` в NVS и восстановит пару `admin / admin`. Настройки Wi-Fi и параметры брокера MQTT сохраняются без изменений.

---

## 5. REST API спецификация

| Метод | Эндпоинт | Аутентификация | Описание |
| :--- | :--- | :---: | :--- |
| `GET` | `/` | Basic Auth | Главный дашборд управления и телеметрии |
| `GET` | `/logout` | Нет | Завершение текущей сессии и очистка заголовков |
| `GET` | `/api/metrics` | Basic Auth | JSON-снапшот статуса, кодов POST и аптайма |
| `POST` | `/pwr/pulse` | Basic Auth | Импульс кнопки Power (200 мс) |
| `POST` | `/pwr/force` | Basic Auth | Аппаратное удержание Power (5 сек, Force Off) |
| `POST` | `/reset` | Basic Auth | Импульс кнопки Reset (200 мс) |
| `GET` | `/api/settings/mqtt` | Basic Auth | Получение текущей конфигурации MQTT |
| `POST` | `/api/settings/mqtt` | Basic Auth | Сохранение настроек брокера MQTT |
| `POST` | `/api/settings/auth` | Basic Auth | Синхронное обновление логина и пароля |
| `POST` | `/api/reset_wifi` | Basic Auth | Сброс сетевых настроек и рестарт в режим AP |

---

## 6. Интеграция с MQTT и Home Assistant

### Топики MQTT

| Топик | QOS / Retain | Формат нагрузки | Назначение |
| :--- | :---: | :--- | :--- |
| `homeassistant/sensor/mini_ipmi/status` | 1 / True | `online` \| `booting` \| `hung` \| `offline` | Текущий рабочий статус хоста |
| `homeassistant/sensor/mini_ipmi/post_code` | 1 / True | `00000000` (HEX) | Последний перехваченный POST-код |
| `homeassistant/sensor/mini_ipmi/post_desc` | 1 / True | Строка UTF-8 | Текстовая расшифровка этапа |
| `homeassistant/sensor/mini_ipmi/pwr_state` | 1 / True | Строка | Линия питания (`S0 (Active)`, `S4/S5 (Off)`) |
| `homeassistant/sensor/mini_ipmi/host_uptime` | 1 / True | Целое (секунды) | Аптайм ОС хоста (с момента кода 0x00) |
| `homeassistant/sensor/mini_ipmi/esp_uptime` | 1 / True | Целое (секунды) | Непрерывный аптайм контроллера ESP32 |
| `homeassistant/sensor/mini_ipmi/state` | 1 / True | JSON (до 512 B) | Сводный телеметрический пакет |
| `homeassistant/switch/mini_ipmi/power/set` | 0 / False | `PRESS` \| `FORCE_OFF` | Команда управления питанием |
| `homeassistant/switch/mini_ipmi/reset/set` | 0 / False | `PRESS` \| `RESET` | Команда аппаратного сброса |
| `homeassistant/switch/mini_ipmi/result` | 1 / False | `OK` \| `BUSY` | Статус исполнения команды контроллером |

### Конфигурация сущностей для Home Assistant (`configuration.yaml`)

```yaml
mqtt:
  button:
    - name: "IPC-R300 Power Press"
      unique_id: ipcr300_power_press
      command_topic: "homeassistant/switch/mini_ipmi/power/set"
      payload_press: "PRESS"
      icon: "mdi:power"

    - name: "IPC-R300 Hard Power Off"
      unique_id: ipcr300_power_force_off
      command_topic: "homeassistant/switch/mini_ipmi/power/set"
      payload_press: "FORCE_OFF"
      icon: "mdi:power-plug-off"

    - name: "IPC-R300 Hardware Reset"
      unique_id: ipcr300_hardware_reset
      command_topic: "homeassistant/switch/mini_ipmi/reset/set"
      payload_press: "PRESS"
      icon: "mdi:restart"

  sensor:
    - name: "IPC-R300 Platform Status"
      unique_id: ipcr300_platform_status
      state_topic: "homeassistant/sensor/mini_ipmi/status"
      icon: "mdi:server"

    - name: "IPC-R300 POST Code"
      unique_id: ipcr300_post_code
      state_topic: "homeassistant/sensor/mini_ipmi/post_code"
      value_template: "0x{{ value }}"
      icon: "mdi:barcode"

    - name: "IPC-R300 POST Description"
      unique_id: ipcr300_post_desc
      state_topic: "homeassistant/sensor/mini_ipmi/post_desc"
      icon: "mdi:information-outline"

    - name: "IPC-R300 Power Rail"
      unique_id: ipcr300_power_rail
      state_topic: "homeassistant/sensor/mini_ipmi/pwr_state"
      icon: "mdi:flash"

    - name: "IPC-R300 Host Uptime"
      unique_id: ipcr300_host_uptime
      state_topic: "homeassistant/sensor/mini_ipmi/host_uptime"
      unit_of_measurement: "s"
      device_class: duration
      icon: "mdi:timer-outline"

    - name: "IPC-R300 Controller Uptime"
      unique_id: ipcr300_esp_uptime
      state_topic: "homeassistant/sensor/mini_ipmi/esp_uptime"
      unit_of_measurement: "s"
      device_class: duration
      icon: "mdi:chip"

  binary_sensor:
    - name: "IPC-R300 Hardware Problem"
      unique_id: ipcr300_hw_problem
      state_topic: "homeassistant/sensor/mini_ipmi/state"
      value_template: "{{ 'ON' if value_json.error else 'OFF' }}"
      device_class: problem
```

---

## 7. Сборка проекта (PlatformIO)

Файл `platformio.ini`:

```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
monitor_speed = 115200

build_flags = 
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
    bblanchon/ArduinoJson@^7.0.0
    knolleary/PubSubClient@^2.8
    mathieucarbou/ESPAsyncWebServer@^3.1.5
    mathieucarbou/AsyncTCP@^3.1.4
```

Сборка и прошивка через CLI:
```bash
pio run -t upload
pio device monitor
```
