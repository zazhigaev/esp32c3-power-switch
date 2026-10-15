ESP32-C3 Mini-IPMI for IPC-R300

Компактный аппаратный контроллер удалённого управления (Out-of-Band Management / mini-IPMI) для промышленного мини-ПК IPC-R300 на базе чипа ESP32-C3 Super Mini Pro (RISC-V) с выносной U.FL-антенной.

Проект решает задачу надёжного дистанционного контроля электропитания, аппаратного сброса, сбора низкоуровневой системной телеметрии и прямого доступа к консоли Linux по сети без необходимости разворачивать громоздкие KVM-over-IP решения.
Основные возможности

    Гальваническая развязка управления: Включение, выключение и аппаратный сброс реализованы через оптопары PC817/EL817, исключая протекание уравнительных токов и повреждение материнской платы.

    Умный автозапуск (Glitch-free & Safe Auto-Power): Защита от спонтанных нажатий при перезагрузках ESP32 — контроллер проверяет статус шины питания и пингует шелл Linux перед нажатием кнопки включения.

    Сетевой мост консоли (TCP Serial Bridge): Прямой доступ к шеллу ОС через TCP-порт 2323 (telnet / nc). При активной сессии консоли регулярный опрос телеметрии автоматически ставится на паузу.

    Сбор системной телеметрии (FSM): Периодический опрос ядра Linux без использования String в буфере приема (нулевая фрагментация heap):
        Температура процессора (Atom C3538);
        Нагрузка системы (Load Average 1m);
        Использование оперативной памяти (Free / Total);
        Статус накопителей /dev/sda и /dev/sdb через SMART (smartctl).

    Интеграция с Home Assistant: Публикация метрик и состояний по протоколу MQTT с поддержкой подтверждения выполнения команд (OK / BUSY) и очисткой retained-сообщений.

    Captive Portal и сканер сетей: При отсутствии подключения к Wi-Fi плата поднимает изолированную точку доступа с веб-интерфейсом, поиском сетей поблизости, выбором DHCP / Static IP и индикацией мигающим синим светодиодом (GPIO8).

    Поддержка локального mDNS: Доступ к веб-панели по имени хоста [http://ipmi.local](http://ipmi.local) при работе через DHCP.

Архитектура и схемотехника
Аппаратная часть

    Микроконтроллер: ESP32-C3 Super Mini Pro (4MB Flash, встроенный разъем U.FL/IPEX).
    Преобразователь питания: mini360 (вход 12V от разъема материнской платы → выход 5.05–5.08V на вход 5V контроллера).
    Оптопары: 2 шт. PC817 / EL817C (корпус DIP-4 или SOP-4).
    Токоограничивающие резисторы: 2 шт. 220–330 Ом (SMD 0805 или выводные).
    Внешняя антенна: FPC-антенна 2.4 ГГц с пигтейлом IPEX/U.FL (выводится за пределы металлического корпуса).

Назначение контактов (Pinout)
Вывод ESP32-C3  Физический пин платы	Цепь / Назначение                     Примечание

GPIO1           1                     Анод оптопары Power SW (через 220 Ом)	Импульс 300 мс (включение) / 5000 мс (Hard Off)
GPIO3           3                     Анод оптопары Reset (через 220 Ом)    Импульс 300 мс (сброс)
GPIO4           4                     UART RX ← Пин 2 разъёма TTL UART      Прием логов и детекция активности материнки
GPIO5           5                     UART TX → Пин 3 разъёма TTL UART      Передача команд телеметрии и ввод в консоль
GPIO8           Встроенный LED        Синий светодиод индикации             Мигание 2 Гц в режиме AP, выключен в STA
GND             G                     Общий провод (Земля)                  Соединяется с пином 1 разъёма TTL UART
5V              5V                    Вход питания +5В                      Подключается к выходу DC-DC mini360


Схема соединений
Plaintext

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

Сборка и прошивка (PlatformIO)
Структура репозитория
Plaintext

.
├── include/
├── lib/
├── src/
│   └── main.cpp
├── platformio.ini
└── README.md

Конфигурация platformio.ini
Ini, TOML

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

Инструкция по сборке

    Клонируйте репозиторий:
    Bash

    git clone https://github.com/<username>/<repo_name>.git
    cd <repo_name>

    Подключите плату ESP32-C3 через кабель USB Type-C.

    Выполните сборку и загрузку прошивки:
    Bash

    pio run -t upload

    Откройте монитор последовательного порта:
    Bash

    pio device monitor

Начальная настройка (Captive Portal)

    При первом включении или отсутствии настроенной сети Wi-Fi синий светодиод на плате начинает ритмично мигать (2 Гц).

    Подключитесь с ноутбука или смартфона к сервисной точке доступа:

        SSID: IPMI-Config-AP

        Пароль: P@$$w0rd

           

    Откроется всплывающее окно мастера подключения (или перейдите по адресу [http://192.168.4.1](http://192.168.4.1)).

    Нажмите Rescan Networks, выберите вашу рабочую Wi-Fi сеть, укажите пароль и выберите конфигурацию IP (DHCP либо статический адрес).

    Нажмите Save & Connect. Контроллер сохранит параметры в энергонезависимую память NVS, перезагрузится и подключится к сети. Светодиод погаснет.

Использование и сетевые интерфейсы
1. Веб-интерфейс управления

Панель доступна по адресу [http://ipmi.local](http://ipmi.local) (при DHCP) или по заданному статическому IP:

    Кнопки физического управления: Power on/off (импульс 300 мс), Hard Power Off (удержание 5 с), Reset (импульс 300 мс).

    Мониторинг в реальном времени: температура процессора, Load Average, свободная память, статус накопителей.

    Форма настройки подключения к MQTT-брокеру (IP, порт, пользователь, пароль / очистка пароля).

    Кнопка аварийного сброса Wi-Fi и возврата в режим точки доступа.

2. Доступ к терминалу ОС (Serial Bridge)

Подключение к Linux-консоли мини-ПК осуществляется по протоколу TCP через порт 2323:
Bash

telnet ipmi.local 2323
# или
nc ipmi.local 2323

На время активной сетевой сессии опрос фоновой телеметрии автоматически блокируется, предотвращая перекрестное загрязнение ввода/вывода.
Интеграция с Home Assistant

Конфигурация MQTT (configuration.yaml)

YAML

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

   
Аппаратные примечания при монтаже

    ВЧ-перемычка антенны: С завода на платах ESP32-C3 Super Mini нулевой резистор подключен к керамической антенне. Для вывода сигнала на внешний коаксиальный разъем U.FL перепаяйте перемычку на 45° в сторону разъема IPEX (или соедините площадки каплей припоя).   
    PNG

    Калибровка питания: До подключения ESP32 к выходу mini360 обязательно выставьте подстроечным резистором напряжение строго 5.05–5.08 В под контролем мультиметра.

    Изоляция: Внутри металлического корпуса IPC-R300 плата контроллера и модуль оптопар должны быть защищены термоусадочной трубкой или каптоновым скотчем.
