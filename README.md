# ESP32-C3 Mini-IPMI for IPC-R300

Компактный аппаратный контроллер удалённого управления (Out-of-Band Management / mini-IPMI) для промышленного мини-ПК **IPC-R300** на базе чипа **ESP32-C3 Super Mini Pro** (RISC-V) с выносной U.FL-антенной[cite: 1, 5, 7].

Проект решает задачу надёжного дистанционного контроля электропитания, аппаратного сброса, сбора системной телеметрии и прямого доступа к консоли Linux по сети без необходимости разворачивать KVM-over-IP.

---

## Основные возможности

* **Гальваническая развязка управления:** Включение, выключение и аппаратный сброс реализованы через оптопары PC817/EL817, что исключает протекание уравнительных токов и защищает плату.
* **Безопасный автозапуск (Safe Auto-Power):** Защита от случайных нажатий при перезагрузках ESP32 — контроллер проверяет статус шины питания и отклик шелла Linux перед выдачей импульса на кнопку включения.
* **Сетевой мост консоли (TCP Serial Bridge):** Прямой доступ к шеллу ОС через TCP-порт `2323` (`telnet` / `nc`)[cite: 1]. Во время активной сессии консоли регулярный опрос телеметрии автоматически ставится на паузу.
* **Сбор системной телеметрии (FSM):** Опрос ядра Linux с использованием статического буфера (без фрагментации памяти heap):
  * Температура процессора (Atom C3538);
  * Нагрузка системы (Load Average 1m);
  * Использование оперативной памяти (Free / Total);
  * Статус накопителей `/dev/sda` и `/dev/sdb` через SMART (`smartctl`).
* **Интеграция с Home Assistant:** Публикация метрик и состояний по MQTT с подтверждением команд (`OK` / `BUSY`) и автоочисткой retained-сообщений[cite: 1].
* **Captive Portal и сканер сетей:** При отсутствии подключения к Wi-Fi плата поднимает точку доступа с веб-интерфейсом, сканером сетей, выбором DHCP / Static IP и индикацией мигающим синим светодиодом (GPIO8).
* **Поддержка mDNS:** Доступ к панели по имени хоста `http://ipmi.local` при получении адреса по DHCP.

---

## Архитектура и схемотехника

### Аппаратная часть
* **Микроконтроллер:** ESP32-C3 Super Mini Pro (4MB Flash, разъем U.FL/IPEX)[cite: 1, 5, 7].
* **Преобразователь питания:** mini360 (вход 12V от разъема материнской платы -> выход 5.05–5.08V на вход 5V платы ESP32)[cite: 1, 8].
* **Оптопары:** 2 шт. PC817 / EL817C (корпус DIP-4 или SOP-4)[cite: 1].
* **Токоограничивающие резисторы:** 2 шт. номиналом 220–330 Ом.
* **Внешняя антенна:** FPC-антенна 2.4 ГГц с пигтейлом IPEX/U.FL[cite: 1, 6, 7].

### Назначение контактов (Pinout)

| Вывод ESP32-C3 | Физический пин платы | Цепь / Назначение | Примечание |
| :--- | :--- | :--- | :--- |
| **GPIO1**[cite: 1] | `1` | Анод оптопары Power SW (через резистор 220–330 Ом) | Импульс 300 мс (включение) / 5000 мс (Hard Off)[cite: 1] |
| **GPIO3**[cite: 1] | `3`[cite: 8] | Анод оптопары Reset (через резистор 220–330 Ом) | Импульс 300 мс (сброс)[cite: 1] |
| **GPIO4**[cite: 1] | `4`[cite: 8] | **UART RX** <- Пин 2 разъёма TTL UART | Прием данных и проверка активности материнки |
| **GPIO5**[cite: 1] | `5`[cite: 8] | **UART TX** -> Пин 3 разъёма TTL UART | Передача команд телеметрии и ввод в консоль |
| **GPIO8**[cite: 1] | Встроенный LED | Синий светодиод индикации | Мигание 2 Гц в режиме AP, выключен в STA |
| **GND** | `G`[cite: 8] | Общий провод (Земля) | Подключается к пину 1 разъёма TTL UART |
| **5V** | `5V`[cite: 8] | Вход питания +5В | Подключается к выходу DC-DC mini360 |

---

## Схема соединений

```text
               +-------------------------------------------+
               |             IPC-R300 Mainboard            |
               |                                           |
               |  [Power Button +3.3V]  [Reset Pad +3.3V]  |
               |         |                    |            |
               |         |                    |            |
               |  [TTL UART Header]           |            |
               |   Pin 1: GND                 |            |
               |   Pin 2: TX (3.3V)           |            |
               |   Pin 3: RX (3.3V)           |            |
               +---|------|---------^---------|------^-----+
                   |      |         |         |      |
                   |      |         |    +----+      |
                   |      |         |    |           |
+-------------+    |      |         |    | Pin 4     | Pin 4
|  DC-DC      |    |      |         |  +---------+ +---------+
|  mini360    |    |      |         |  | PC817   | | PC817   |
| 12V -> 5.0V |    |      |         |  | (Reset) | | (Power) |
+------+------+    |      |         |  +---------+ +---------+
   |   |           |      |         |    | Pin 3     | Pin 3
  +5V GND          |      |         |    +----+      +----+
   |   |           |      |         |         |           |
   |   +-----------+------+---------+---------+-----------+
   |   |           | (Общий провод GND)
   v   v
+------+---------------------------------------------------+
|  5V  GND      GPIO4(RX)  GPIO5(TX)     GPIO3       GPIO1 |
|                                          |           |   |
|            ESP32-C3 Super Mini Pro       |220R       |220R
|                                          v           v   |
|                                        Pin 1       Pin 1 |
|                                      (Анод)      (Анод)  |
|                                      +---------+ +-----+ |
|                                      | PC817   | |PC817| |
|                                      +---------+ +-----+ |
|                                        Pin 2       Pin 2 |
|                                       (Катод)     (Катод)|
|                                          +-----------+   |
|                                          |               |
|                                          +---> к GND платы
+----------------------------------------------------------+
```

---
## Сборка и прошивка (PlatformIO)

Конфигурация platformio.ini
```text
[env:esp32-c3-supermini]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino

board_build.flash_mode = dio
board_build.flash_size = 4MB

monitor_speed = 115200

build_flags = 
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
    ESP32Async/AsyncTCP @ 3.3.2
    ESP32Async/ESPAsyncWebServer @ 3.6.0
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^7.0.4
```
---
## Начальная настройка (Captive Portal)

1. При первом включении синий светодиод начнет мигать (2 раза в секунду).
2. Подключитесь к Wi-Fi сети:
   * SSID: IPMI-Config-AP
   * Пароль: P@$$w0rd
3. Дождитесь всплывающего окна авторизации или перейдите на http://192.168.4.1.
4. Нажмите Rescan Networks, выберите вашу рабочую точку доступа и введите пароль.
5. При необходимости укажите Static IP или оставьте DHCP.
6. Нажмите Save & Connect. Контроллер сохранит настройки в NVS, перезагрузится и подключится к домашней сети. Светодиод погаснет.

---
## Сетевые интерфейсы
1. Веб-интерфейс

Доступен по адресу http://ipmi.local или по назначенному IP:

* Кнопки физического управления: Power on/off (импульс 300 мс), Hard Power Off (5 с), Reset (импульс 300 мс).
* Показания телеметрии (температура CPU, Load Average, RAM, SMART).
* Настройки подключения к MQTT-брокеру (хост, порт, логин, пароль / удаление пароля).
* Сброс Wi-Fi для повторного перехода в режим AP.

## Мост консоли (Serial Bridge)
Подключение к Linux-консоли мини-ПК выполняется по TCP через порт 2323:
```text
telnet ipmi.local 2323
# или
nc ipmi.local 2323
```
# Интеграция с Home Assistant (configuration.yaml)
```text
mqtt:
  button:
    - name: "IPC-R300 Power Press"
      command_topic: "homeassistant/switch/mini_ipmi/power/set"
      payload_press: "PRESS"
      icon: "mdi:power"

    - name: "IPC-R300 Hard Power Off"
      command_topic: "homeassistant/switch/mini_ipmi/power/set"
      payload_press: "FORCE_OFF"
      icon: "mdi:power-plug-off"

    - name: "IPC-R300 Hardware Reset"
      command_topic: "homeassistant/switch/mini_ipmi/reset/set"
      payload_press: "PRESS"
      icon: "mdi:restart"

  sensor:
    - name: "IPC-R300 Status"
      state_topic: "homeassistant/sensor/mini_ipmi/status"
      icon: "mdi:server"

    - name: "IPC-R300 CPU Temperature"
      state_topic: "homeassistant/sensor/mini_ipmi/state"
      unit_of_measurement: "°C"
      value_template: "{{ value_json.temp }}"
      device_class: temperature

    - name: "IPC-R300 Load Average"
      state_topic: "homeassistant/sensor/mini_ipmi/state"
      value_template: "{{ value_json.load }}"
      icon: "mdi:cpu-64-bit"

    - name: "IPC-R300 Free RAM"
      state_topic: "homeassistant/sensor/mini_ipmi/state"
      unit_of_measurement: "MB"
      value_template: "{{ value_json.mfree }}"
      icon: "mdi:memory"

  binary_sensor:
    - name: "IPC-R300 /dev/sda SMART"
      state_topic: "homeassistant/sensor/mini_ipmi/state"
      value_template: "{{ 'ON' if value_json.sda == 1 else 'OFF' }}"
      device_class: problem
      payload_on: "OFF"
      payload_off: "ON"

    - name: "IPC-R300 /dev/sdb SMART"
      state_topic: "homeassistant/sensor/mini_ipmi/state"
      value_template: "{{ 'ON' if value_json.sdb == 1 else 'OFF' }}"
      device_class: problem
      payload_on: "OFF"
      payload_off: "ON"
```
---
## Аппаратные примечания при монтаже
* Перемычка антенны: С завода нулевой резистор ведет к керамической антенне[cite: 8]. Для внешнего U.FL-разъема перепаяйте перемычку на контактную площадку разъема IPEX (или соедините площадки припоем).
* Калибровка питания: До подключения ESP32 к mini360 настройте на выходе стабилизатора строго 5.05–5.08 В.
* Изоляция: Плату контроллера и модуль оптопар внутри металлического шасси обязательно изолируйте термоусадкой или каптоновым скотчем.
