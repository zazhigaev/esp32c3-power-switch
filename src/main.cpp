#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <esp_timer.h>

// ============================================================
// Аппаратные выводы
// ============================================================
const int PIN_PWR     = 1;  // Оптопара Power SW
const int PIN_RESET   = 3;  // Оптопара Reset
const int UART_RX_PIN = 4;  // RX ESP32 <- TX платы IPC-R300
const int UART_TX_PIN = 5;  // TX ESP32 -> RX платы IPC-R300
const int PIN_LED     = 8;  // Синий светодиод платы Super Mini
const int PIN_BOOT    = 9;  // Аппаратная кнопка BOOT на ESP32

// ============================================================
// Вспомогательные безопасные функции
// ============================================================
void safeStrCopy(char* dst, const char* src, size_t dstSize) {
    if (!dst || dstSize == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

bool parseStrictPort(const String& str, int& outPort) {
    if (str.length() == 0 || str.length() > 5) return false;
    for (size_t i = 0; i < str.length(); ++i) {
        if (!isDigit(str.charAt(i))) return false;
    }
    long val = str.toInt();
    if (val < 1 || val > 65535) return false;
    outPort = (int)val;
    return true;
}

// ============================================================
// Потокобезопасные команды для loopTask
// ============================================================
enum UserAction {
    ACT_NONE,
    ACT_POWER_PULSE,
    ACT_POWER_FORCE,
    ACT_RESET_PULSE,
    ACT_SAVE_MQTT,
    ACT_SAVE_WIFI,
    ACT_RESET_WIFI,
    ACT_SAVE_AUTH
};

struct MqttConfigPayload {
    char host[64];
    int  port;
    char user[32];
    char pass[64];
    bool clearPass;
};

struct WifiConfigPayload {
    char ssid[33];
    char pass[65];
    bool dhcp;
    char ip[16];
    char mask[16];
    char gw[16];
    char dns[16];
};

struct AuthConfigPayload {
    char user[32];
    char pass[64];
};

struct SystemCommand {
    UserAction action;
    union {
        MqttConfigPayload mqttPayload;
        WifiConfigPayload wifiPayload;
        AuthConfigPayload authPayload;
    };
};

QueueHandle_t cmdQueue = nullptr;
SemaphoreHandle_t cfgMutex = nullptr;
SemaphoreHandle_t authMutex = nullptr;
SemaphoreHandle_t authDoneSem = nullptr;

// ============================================================
// Конфигурация NVS
// ============================================================
struct Config {
    String wifi_ssid;
    String wifi_pass;
    bool   wifi_dhcp;
    String wifi_ip;
    String wifi_mask;
    String wifi_gw;
    String wifi_dns;

    String mqtt_host;
    int    mqtt_port;
    String mqtt_user;
    String mqtt_pass;

    String http_user;
    String http_pass;
} appCfg;

// Кэшированные учетные данные для неблокирующей проверки Basic Auth
char cachedHttpUser[32] = "admin";
char cachedHttpPass[64] = "admin";

Preferences prefs;

// ============================================================
// Глобальные объекты и флаги
// ============================================================
bool isApMode = false;
DNSServer dnsServer;
const byte DNS_PORT = 53;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
AsyncWebServer server(80);

bool pulseActive = false;
int pulsePin = -1;
uint32_t pulseEndTime = 0;

const uint32_t AUTO_POWER_DELAY = 5000;
uint32_t bootTime = 0;
bool autoPowerTriggered = false;

bool restartPending = false;
uint32_t restartAt = 0;

char uartBuffer[256];
size_t uartBufferLen = 0;

enum HostState {
    HOST_OFFLINE,
    HOST_BOOTING,
    HOST_ONLINE,
    HOST_HANG
};

struct PcStatus {
    HostState state = HOST_OFFLINE;
    uint32_t lastPostCode = 0;
    char lastCodeHex[16] = "00000000";
    char postDescription[64] = "Powered Off";
    char pwrState[32] = "S4 (Off)";
    bool isError = false;
    uint32_t lastPostChangeTime = 0;
    uint32_t lastTrafficTime = 0;
    uint64_t onlineStartSec = 0;
} pcStatus;

enum RecoveryState {
    REC_IDLE,
    REC_WAIT_PULSE_READY,
    REC_WAIT_SOFT_OFF,
    REC_WAIT_BEFORE_ON
};

RecoveryState recState = REC_IDLE;
uint32_t recTimer = 0;
bool userShutdownActive = false;

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

uint64_t getUptimeSec() {
    return (uint64_t)(esp_timer_get_time() / 1000000ULL);
}

// Таблица расшифровки контрольных точек AMI / Intel FSP
struct PostCodeDesc {
    uint8_t code;
    const char* desc;
    bool isError;
};

const PostCodeDesc POST_TABLE[] = {
    { 0x00, "OS Running / Heartbeat", false },
    { 0x1C, "Power State S4/S5 (Off)", false },
    { 0x24, "RAM SPD Addressing", false },
    { 0x25, "RAM Training MRC", false },
    { 0x55, "RAM Error / Not Detected", true },
    { 0x71, "PCH SATA Controller Init", false },
    { 0x75, "Storage Detection", false },
    { 0x77, "SATA Devices Configured", false },
    { 0x79, "Storage / SSD Failure", true },
    { 0x80, "OEM Early DXE Init", false },
    { 0x88, "PCIe Bus Enumeration", false },
    { 0x91, "PCIe Subsystem Setup", false },
    { 0x95, "PCIe Resource Allocation", false },
    { 0x97, "Console Output Devices", false },
    { 0x98, "Console Input Devices", false },
    { 0x99, "BDS: Super I/O / OS Hand-off", false },
    { 0x9C, "USB Controllers Ready", false },
    { 0xA0, "ACPI Tables Initialized", false },
    { 0xB0, "CPU Early Initialization", false },
    { 0xB2, "Legacy ROM / Option ROM", false },
    { 0xC1, "Recovery Check", false }
};

const char* decodePostCode(uint8_t code, bool& errorFlag) {
    for (size_t i = 0; i < sizeof(POST_TABLE) / sizeof(POST_TABLE[0]); ++i) {
        if (POST_TABLE[i].code == code) {
            errorFlag = POST_TABLE[i].isError;
            return POST_TABLE[i].desc;
        }
    }
    errorFlag = false;
    return "POST Initialization";
}

// ============================================================
// Индикация светодиодом (GPIO8)
// ============================================================
void updateLed() {
    if (isApMode) {
        static uint32_t lastBlink = 0;
        static bool ledState = false;
        if (millis() - lastBlink >= 250) {
            lastBlink = millis();
            ledState = !ledState;
            digitalWrite(PIN_LED, ledState ? LOW : HIGH);
        }
    } else {
        digitalWrite(PIN_LED, HIGH);
    }
}

// ============================================================
// NVS Чтение / Запись (Только loopTask)
// ============================================================
bool loadSettings() {
    if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("[ERROR] loadSettings() failed to take cfgMutex!");
        return false;
    }
    prefs.begin("ipmi_cfg", true);
    appCfg.wifi_ssid = prefs.getString("ssid", "");
    appCfg.wifi_pass = prefs.getString("pass", "");
    appCfg.wifi_dhcp = prefs.getBool("dhcp", true);
    appCfg.wifi_ip   = prefs.getString("ip", "10.10.11.12");
    appCfg.wifi_mask = prefs.getString("mask", "255.255.255.0");
    appCfg.wifi_gw   = prefs.getString("gw", "10.10.11.1");
    appCfg.wifi_dns  = prefs.getString("dns", "10.10.11.1");

    appCfg.mqtt_host = prefs.getString("mq_host", "10.10.11.10");
    appCfg.mqtt_port = prefs.getInt("mq_port", 1883);
    appCfg.mqtt_user = prefs.getString("mq_user", "");
    appCfg.mqtt_pass = prefs.getString("mq_pass", "");

    appCfg.http_user = prefs.getString("http_user", "admin");
    appCfg.http_pass = prefs.getString("http_pass", "admin");

    safeStrCopy(cachedHttpUser, appCfg.http_user.c_str(), sizeof(cachedHttpUser));
    safeStrCopy(cachedHttpPass, appCfg.http_pass.c_str(), sizeof(cachedHttpPass));

    prefs.end();
    xSemaphoreGive(cfgMutex);
    return true;
}

void saveWifiSettings(const String& ssid, const String& pass, bool dhcp, const String& ip, const String& mask, const String& gw, const String& dns) {
    prefs.begin("ipmi_cfg", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putBool("dhcp", dhcp);
    if (!dhcp) {
        prefs.putString("ip", ip);
        prefs.putString("mask", mask);
        prefs.putString("gw", gw);
        prefs.putString("dns", dns);
    }
    prefs.end();
}

void saveMqttSettings(const String& host, int port, const String& user, const String& mqpass, bool clearPass) {
    prefs.begin("ipmi_cfg", false);
    prefs.putString("mq_host", host);
    prefs.putInt("mq_port", port);
    prefs.putString("mq_user", user);
    if (clearPass) {
        prefs.putString("mq_pass", "");
    } else if (mqpass.length() > 0) {
        prefs.putString("mq_pass", mqpass);
    }
    prefs.end();
}

// ============================================================
// Управление импульсами
// ============================================================
bool startPulse(int pin, uint32_t durationMs) {
    if (pulseActive) return false;
    digitalWrite(pin, HIGH);
    pulsePin = pin;
    pulseEndTime = millis() + durationMs;
    pulseActive = true;
    Serial.printf("[PULSE] GPIO%d HIGH for %u ms\n", pin, durationMs);
    return true;
}

void updatePulse() {
    if (!pulseActive) return;
    if ((int32_t)(millis() - pulseEndTime) >= 0) {
        digitalWrite(pulsePin, LOW);
        Serial.printf("[PULSE] GPIO%d LOW\n", pulsePin);
        pulsePin = -1;
        pulseActive = false;
    }
}

void scheduleRestart(uint32_t delayMs = 1000) {
    restartPending = true;
    restartAt = millis() + delayMs;
}

void updateRestart() {
    if (!restartPending) return;
    if ((int32_t)(millis() - restartAt) >= 0) {
        Serial.println("[SYSTEM] Restarting ESP32...");
        ESP.restart();
    }
}

// ============================================================
// Публикация в MQTT
// ============================================================
void publishPcState() {
    if (!mqttClient.connected()) return;

    portENTER_CRITICAL(&statusMux);
    HostState st = pcStatus.state;
    char codeHex[16];
    safeStrCopy(codeHex, pcStatus.lastCodeHex, sizeof(codeHex));
    char desc[64];
    safeStrCopy(desc, pcStatus.postDescription, sizeof(desc));
    char pwr[32];
    safeStrCopy(pwr, pcStatus.pwrState, sizeof(pwr));
    bool err = pcStatus.isError;
    uint64_t startSec = pcStatus.onlineStartSec;
    portEXIT_CRITICAL(&statusMux);

    const char* statusStr = "offline";
    if (st == HOST_ONLINE) statusStr = "online";
    else if (st == HOST_BOOTING) statusStr = "booting";
    else if (st == HOST_HANG) statusStr = "hung";

    mqttClient.publish("homeassistant/sensor/mini_ipmi/status", statusStr, true);
    mqttClient.publish("homeassistant/sensor/mini_ipmi/post_code", codeHex, true);
    mqttClient.publish("homeassistant/sensor/mini_ipmi/post_desc", desc, true);
    mqttClient.publish("homeassistant/sensor/mini_ipmi/pwr_state", pwr, true);

    uint64_t currentSec = getUptimeSec();
    char secBuf[32];
    snprintf(secBuf, sizeof(secBuf), "%llu", currentSec);
    mqttClient.publish("homeassistant/sensor/mini_ipmi/esp_uptime", secBuf, true);

    uint64_t hostUptimeSec = (st == HOST_ONLINE && startSec > 0 && currentSec >= startSec) 
        ? (currentSec - startSec) : 0ULL;
    snprintf(secBuf, sizeof(secBuf), "%llu", hostUptimeSec);
    mqttClient.publish("homeassistant/sensor/mini_ipmi/host_uptime", secBuf, true);

    char jsonBuf[512];
    snprintf(jsonBuf, sizeof(jsonBuf), 
        "{\"status\":\"%s\",\"post_hex\":\"%s\",\"description\":\"%s\",\"power\":\"%s\",\"error\":%s,\"host_uptime\":%llu,\"esp_uptime\":%llu}",
        statusStr, codeHex, desc, pwr, err ? "true" : "false", hostUptimeSec, currentSec
    );
    if (!mqttClient.publish("homeassistant/sensor/mini_ipmi/state", jsonBuf, true)) {
        Serial.println("[MQTT] Warning: Failed to publish telemetry JSON state (buffer overflow?)");
    }
}

// ============================================================
// Парсинг логов EC/POST
// ============================================================
void processEcLine(const char* line) {
    pcStatus.lastTrafficTime = millis();
    bool stateChanged = false;

    if (strstr(line, "Power state S4") != nullptr || strstr(line, "Power state S5") != nullptr) {
        portENTER_CRITICAL(&statusMux);
        pcStatus.state = HOST_OFFLINE;
        pcStatus.isError = false;
        pcStatus.onlineStartSec = 0;
        safeStrCopy(pcStatus.pwrState, "S4/S5 (Off)", sizeof(pcStatus.pwrState));
        safeStrCopy(pcStatus.postDescription, "System Powered Down", sizeof(pcStatus.postDescription));
        safeStrCopy(pcStatus.lastCodeHex, "0000001C", sizeof(pcStatus.lastCodeHex));
        portEXIT_CRITICAL(&statusMux);
        stateChanged = true;

        if (recState == REC_WAIT_SOFT_OFF) {
            Serial.println("[RECOVERY] Soft shutdown confirmed. Waiting 3 seconds before power-on...");
            recState = REC_WAIT_BEFORE_ON;
            recTimer = millis() + 3000;
        }
    }
    else if (strstr(line, "PWR init state") != nullptr) {
        portENTER_CRITICAL(&statusMux);
        if (pcStatus.state != HOST_BOOTING && pcStatus.state != HOST_ONLINE) {
            pcStatus.state = HOST_BOOTING;
            pcStatus.isError = false;
            pcStatus.lastPostCode = 0;
            pcStatus.lastPostChangeTime = millis();
            safeStrCopy(pcStatus.pwrState, "Sequencing", sizeof(pcStatus.pwrState));
            safeStrCopy(pcStatus.postDescription, "Power Rails Sequencing", sizeof(pcStatus.postDescription));
            stateChanged = true;
        }
        portEXIT_CRITICAL(&statusMux);
    }
    else {
        const char* pPost = strstr(line, "POSTCODE ");
        if (pPost != nullptr) {
            pPost += 9;
            char hexBuf[16];
            size_t idx = 0;
            while (*pPost && *pPost != ' ' && *pPost != '\r' && *pPost != '\n' && idx < sizeof(hexBuf) - 1) {
                hexBuf[idx++] = *pPost++;
            }
            hexBuf[idx] = '\0';

            uint32_t val = (uint32_t)strtoul(hexBuf, nullptr, 16);
            uint8_t byteCode = (uint8_t)(val & 0xFF);

            portENTER_CRITICAL(&statusMux);
            if (val != pcStatus.lastPostCode || pcStatus.state == HOST_OFFLINE) {
                pcStatus.lastPostCode = val;
                safeStrCopy(pcStatus.lastCodeHex, hexBuf, sizeof(pcStatus.lastCodeHex));
                pcStatus.lastPostChangeTime = millis();

                bool err = false;
                const char* desc = decodePostCode(byteCode, err);
                pcStatus.isError = err;
                safeStrCopy(pcStatus.postDescription, desc, sizeof(pcStatus.postDescription));

                if (val == 0) {
                    if (pcStatus.state != HOST_ONLINE) {
                        pcStatus.onlineStartSec = getUptimeSec();
                    }
                    pcStatus.state = HOST_ONLINE;
                    userShutdownActive = false;
                    safeStrCopy(pcStatus.pwrState, "S0 (Active)", sizeof(pcStatus.pwrState));
                    recState = REC_IDLE;
                } else {
                    pcStatus.state = HOST_BOOTING;
                    safeStrCopy(pcStatus.pwrState, "POST (S0)", sizeof(pcStatus.pwrState));
                }
                stateChanged = true;
            } else if (val == 0) {
                pcStatus.lastPostChangeTime = millis();
                if (pcStatus.state != HOST_ONLINE) {
                    pcStatus.onlineStartSec = getUptimeSec();
                    pcStatus.state = HOST_ONLINE;
                    userShutdownActive = false;
                    safeStrCopy(pcStatus.pwrState, "S0 (Active)", sizeof(pcStatus.pwrState));
                    recState = REC_IDLE;
                    stateChanged = true;
                }
            }
            portEXIT_CRITICAL(&statusMux);
        }
        else {
            const char* pErr = strstr(line, "E ");
            if (pErr != nullptr) {
                pErr += 2;
                uint32_t errVal = (uint32_t)strtoul(pErr, nullptr, 16);
                if (errVal != 0) {
                    portENTER_CRITICAL(&statusMux);
                    pcStatus.isError = true;
                    snprintf(pcStatus.postDescription, sizeof(pcStatus.postDescription), "EC Event/Fault 0x%02X", (unsigned int)(errVal & 0xFF));
                    portEXIT_CRITICAL(&statusMux);
                    stateChanged = true;
                } else {
                    pcStatus.lastPostChangeTime = millis();
                }
            }
        }
    }

    if (stateChanged) {
        publishPcState();
    }
}

// ============================================================
// Автоматическое восстановление при зависании (Watchdog)
// ============================================================
void updateAutoRecovery() {
    uint32_t now = millis();

    // 1. Детекция зависания на этапе POST (>30 сек)
    if (pcStatus.state == HOST_BOOTING && pcStatus.lastPostCode != 0 && (now - pcStatus.lastPostChangeTime > 30000)) {
        if (recState == REC_IDLE) {
            portENTER_CRITICAL(&statusMux);
            pcStatus.state = HOST_HANG;
            pcStatus.isError = true;
            snprintf(pcStatus.postDescription, sizeof(pcStatus.postDescription), "Boot Hung at 0x%s. Recovering...", pcStatus.lastCodeHex);
            portEXIT_CRITICAL(&statusMux);
            publishPcState();

            Serial.println("[WATCHDOG] POST Hang detected! Arming recovery...");
            recState = REC_WAIT_PULSE_READY;
        }
    }

    // 2. Ожидание готовности импульсной линии
    if (recState == REC_WAIT_PULSE_READY) {
        if (!pulseActive) {
            if (startPulse(PIN_PWR, 200)) {
                Serial.println("[WATCHDOG] Initiated soft power-off pulse.");
                recState = REC_WAIT_SOFT_OFF;
                recTimer = now + 5000;
            }
        }
    }

    // 3. Ожидание реакции на мягкое выключение (5 секунд)
    if (recState == REC_WAIT_SOFT_OFF) {
        if (pcStatus.state == HOST_OFFLINE) {
            Serial.println("[WATCHDOG] Host reached OFFLINE before timeout. Transitioning to power-on wait...");
            recState = REC_WAIT_BEFORE_ON;
            recTimer = now + 3000;
            return;
        }

        if ((int32_t)(now - recTimer) >= 0) {
            if (pcStatus.state == HOST_HANG) {
                Serial.println("[WATCHDOG] Soft power-off timed out and host still HANG. Triggering hardware RESET!");
                if (startPulse(PIN_RESET, 200)) {
                    recState = REC_IDLE;
                    pcStatus.lastPostChangeTime = now;
                }
            } else {
                Serial.println("[WATCHDOG] Soft power-off timed out, but host changed state. Canceling Reset.");
                recState = REC_IDLE;
            }
        }
    }

    // 4. Пауза 3 секунды после выключения и запуск
    if (recState == REC_WAIT_BEFORE_ON) {
        if ((int32_t)(now - recTimer) >= 0) {
            if (!userShutdownActive) {
                Serial.println("[WATCHDOG] 3s pause elapsed. Triggering power-on pulse...");
                if (startPulse(PIN_PWR, 200)) {
                    recState = REC_IDLE;
                }
            } else {
                Serial.println("[WATCHDOG] Manual shutdown active. Power-on suppressed.");
                recState = REC_IDLE;
                userShutdownActive = false;
            }
        }
    }
}

// ============================================================
// Единый конвейер UART и контроль питания
// ============================================================
void updatePostFSM() {
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\n' || c == '\r') {
            if (uartBufferLen > 0) {
                uartBuffer[uartBufferLen] = '\0';
                processEcLine(uartBuffer);
                uartBufferLen = 0;
            }
        } else if (uartBufferLen < sizeof(uartBuffer) - 1) {
            uartBuffer[uartBufferLen++] = c;
        }
    }

    updateAutoRecovery();

    // Детекция обесточивания: физический LOW на RX (полное обесточивание)
    // ЛИБО тишина на шине UART более 10 секунд после ручного запроса на выключение (ACPI Power Off)
    bool hardPowerLost = (digitalRead(UART_RX_PIN) == LOW && (millis() - pcStatus.lastTrafficTime > 5000));
    bool acpiShutdownSilence = (userShutdownActive && (millis() - pcStatus.lastTrafficTime > 10000));

    if (pcStatus.state != HOST_OFFLINE && (hardPowerLost || acpiShutdownSilence)) {
        portENTER_CRITICAL(&statusMux);
        pcStatus.state = HOST_OFFLINE;
        pcStatus.isError = false;
        pcStatus.onlineStartSec = 0;
        safeStrCopy(pcStatus.pwrState, "S4/S5 (Off)", sizeof(pcStatus.pwrState));
        safeStrCopy(pcStatus.postDescription, "System Powered Down", sizeof(pcStatus.postDescription));
        safeStrCopy(pcStatus.lastCodeHex, "0000001C", sizeof(pcStatus.lastCodeHex));
        portEXIT_CRITICAL(&statusMux);
        publishPcState();
        Serial.println("[STATE] Host shutdown confirmed by bus silence.");
    }
}

// ============================================================
// Безопасный автозапуск при подаче питания
// ============================================================
void checkAutoPower() {
    if (autoPowerTriggered || pulseActive || recState != REC_IDLE || (millis() - bootTime < AUTO_POWER_DELAY)) {
        return;
    }

    if (pcStatus.state != HOST_OFFLINE) {
        Serial.println("[AUTO] Host is already powered (Online/Booting/Hang). Skipping auto-power.");
        autoPowerTriggered = true;
        return;
    }

    Serial.println("[AUTO] Host confirmed OFF. Triggering initial power-on pulse...");
    if (startPulse(PIN_PWR, 200)) {
        autoPowerTriggered = true;
    }
}

// ============================================================
// Обработка очереди входящих команд (Строго в loopTask)
// ============================================================
void processIncomingCommands() {
    SystemCommand cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        switch (cmd.action) {
            case ACT_POWER_PULSE:
                userShutdownActive = (pcStatus.state != HOST_OFFLINE);
                startPulse(PIN_PWR, 200);
                break;
            case ACT_POWER_FORCE:
                userShutdownActive = true;
                startPulse(PIN_PWR, 5000);
                portENTER_CRITICAL(&statusMux);
                pcStatus.state = HOST_OFFLINE;
                pcStatus.isError = false;
                pcStatus.onlineStartSec = 0;
                safeStrCopy(pcStatus.pwrState, "S5 (Forced Off)", sizeof(pcStatus.pwrState));
                safeStrCopy(pcStatus.postDescription, "Hard Power Off", sizeof(pcStatus.postDescription));
                safeStrCopy(pcStatus.lastCodeHex, "0000001C", sizeof(pcStatus.lastCodeHex));
                portEXIT_CRITICAL(&statusMux);
                publishPcState();
                break;
            case ACT_RESET_PULSE:
                recState = REC_IDLE;
                startPulse(PIN_RESET, 200);
                break;
            case ACT_SAVE_MQTT: {
                if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    appCfg.mqtt_host = String(cmd.mqttPayload.host);
                    appCfg.mqtt_port = cmd.mqttPayload.port;
                    appCfg.mqtt_user = String(cmd.mqttPayload.user);
                    if (cmd.mqttPayload.clearPass) {
                        appCfg.mqtt_pass = "";
                    } else if (strlen(cmd.mqttPayload.pass) > 0) {
                        appCfg.mqtt_pass = String(cmd.mqttPayload.pass);
                    }
                    saveMqttSettings(appCfg.mqtt_host, appCfg.mqtt_port, appCfg.mqtt_user, appCfg.mqtt_pass, cmd.mqttPayload.clearPass);
                    
                    mqttClient.disconnect();
                    mqttClient.setServer(appCfg.mqtt_host.c_str(), appCfg.mqtt_port);
                    xSemaphoreGive(cfgMutex);
                    Serial.println("[MQTT] Applied and saved new broker settings in loopTask.");
                }
                break;
            }
            case ACT_SAVE_WIFI: {
                if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    appCfg.wifi_ssid = String(cmd.wifiPayload.ssid);
                    appCfg.wifi_pass = String(cmd.wifiPayload.pass);
                    appCfg.wifi_dhcp = cmd.wifiPayload.dhcp;
                    if (!appCfg.wifi_dhcp) {
                        appCfg.wifi_ip   = String(cmd.wifiPayload.ip);
                        appCfg.wifi_mask = String(cmd.wifiPayload.mask);
                        appCfg.wifi_gw   = String(cmd.wifiPayload.gw);
                        appCfg.wifi_dns  = String(cmd.wifiPayload.dns);
                    }

                    saveWifiSettings(appCfg.wifi_ssid, appCfg.wifi_pass, appCfg.wifi_dhcp,
                                     appCfg.wifi_ip, appCfg.wifi_mask, appCfg.wifi_gw, appCfg.wifi_dns);
                    xSemaphoreGive(cfgMutex);
                    Serial.println("[WIFI] New credentials saved to NVS in loopTask. Restarting...");
                    scheduleRestart(1500);
                }
                break;
            }
            case ACT_SAVE_AUTH: {
                if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    appCfg.http_user = String(cmd.authPayload.user);
                    appCfg.http_pass = String(cmd.authPayload.pass);

                    prefs.begin("ipmi_cfg", false);
                    prefs.putString("http_user", appCfg.http_user);
                    prefs.putString("http_pass", appCfg.http_pass);
                    prefs.end();

                    if (xSemaphoreTake(authMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        safeStrCopy(cachedHttpUser, appCfg.http_user.c_str(), sizeof(cachedHttpUser));
                        safeStrCopy(cachedHttpPass, appCfg.http_pass.c_str(), sizeof(cachedHttpPass));
                        xSemaphoreGive(authMutex);
                    }

                    xSemaphoreGive(cfgMutex);
                    Serial.println("[AUTH] HTTP Basic credentials updated in NVS and cache.");
                }
                // Отдаем подтверждение выполнившей запрос задаче
                if (authDoneSem != nullptr) {
                    xSemaphoreGive(authDoneSem);
                }
                break;
            }
            case ACT_RESET_WIFI:
                prefs.begin("ipmi_cfg", false);
                prefs.remove("ssid");
                prefs.remove("pass");
                prefs.end();
                Serial.println("[WIFI] Settings cleared. Restarting into AP Mode...");
                scheduleRestart(1000);
                break;
            default:
                break;
        }
    }
}

// ============================================================
// Быстрая неблокирующая проверка Basic Auth
// ============================================================
inline bool isAuthenticated(AsyncWebServerRequest *request) {
    char u[32];
    char p[64];
    if (xSemaphoreTake(authMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        safeStrCopy(u, cachedHttpUser, sizeof(u));
        safeStrCopy(p, cachedHttpPass, sizeof(p));
        xSemaphoreGive(authMutex);
    } else {
        return false; // При невозможности захватить мьютекс отклоняем запрос без паники
    }
    return request->authenticate(u, p);
}

// ============================================================
// HTML Интерфейсы (PROGMEM)
// ============================================================
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="utf-8">
  <title>IPC-R300 Wi-Fi Setup</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; background: #1e1e2e; color: #cdd6f4; margin: 15px; text-align: center; }
    h2 { color: #89b4fa; }
    form { background: #313244; padding: 20px; border-radius: 8px; max-width: 320px; margin: 0 auto; text-align: left; }
    label { font-size: 13px; color: #a6adc8; display: block; margin-top: 10px; }
    input, select { width: 100%; padding: 8px; margin-top: 4px; box-sizing: border-box; border-radius: 4px; border: 1px solid #45475a; background: #181825; color: #cdd6f4; }
    button { width: 100%; padding: 12px; margin-top: 15px; background: #a6e3a1; color: #11111b; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; }
    .btn-scan { background: #89b4fa; padding: 8px; margin-top: 6px; }
    #err { color: #f38ba8; font-size: 13px; margin-top: 8px; display: none; }
    #overlay { display: none; margin-top: 20px; font-weight: bold; color: #f9e2af; }
  </style>
</head>
<body>
  <h2>Wi-Fi Setup</h2>
  <div id="setup_box">
    <form id="wifiForm">
      <label>Wi-Fi Network</label>
      <select id="ssid_select" name="ssid">
        <option value="">Searching for networks...</option>
      </select>
      <button type="button" class="btn-scan" onclick="startScan()">Rescan Networks</button>

      <label>Wi-Fi Password</label>
      <input type="password" id="pass" name="pass" placeholder="Password">

      <label>IP Configuration</label>
      <select id="dhcp" name="dhcp" onchange="toggleIpFields()">
        <option value="1">DHCP (Automatic)</option>
        <option value="0">Static IP</option>
      </select>

      <div id="static_fields" style="display:none;">
        <label>Static IP</label>
        <input type="text" id="ip" name="ip" value="10.10.11.12">
        <label>Subnet Mask</label>
        <input type="text" id="mask" name="mask" value="255.255.255.0">
        <label>Gateway</label>
        <input type="text" id="gw" name="gw" value="10.10.11.1">
        <label>DNS</label>
        <input type="text" id="dns" name="dns" value="10.10.11.1">
      </div>

      <div id="err"></div>
      <button type="button" onclick="submitConfig()">Save & Connect</button>
    </form>
  </div>

  <div id="overlay">
    <p>Settings saved!</p>
    <p id="countdown">Connecting... Redirecting in 8 seconds</p>
  </div>

  <script>
    let pollTimer = null;

    function toggleIpFields() {
      const isStatic = document.getElementById('dhcp').value === "0";
      document.getElementById('static_fields').style.display = isStatic ? "block" : "none";
    }

    function startScan() {
      if (pollTimer) clearInterval(pollTimer);
      const sel = document.getElementById('ssid_select');
      sel.innerHTML = '<option value="">Scanning networks...</option>';

      pollTimer = setInterval(() => {
        fetch('/scan').then(r => r.json()).then(data => {
          if (!data.scanning) {
            clearInterval(pollTimer);
            pollTimer = null;
            sel.innerHTML = '';
            if (!data.networks || data.networks.length === 0) {
              sel.innerHTML = '<option value="">No networks found (click rescan)</option>';
              return;
            }
            data.networks.forEach(item => {
              const opt = document.createElement('option');
              opt.value = item.ssid;
              opt.text = item.ssid + ' (' + item.rssi + ' dBm)';
              sel.appendChild(opt);
            });
          }
        }).catch(() => {
          clearInterval(pollTimer);
          pollTimer = null;
          sel.innerHTML = '<option value="">Scan failed (click rescan)</option>';
        });
      }, 600);
    }

    function submitConfig() {
      const errBox = document.getElementById('err');
      errBox.style.display = 'none';

      const p = new URLSearchParams();
      const sel = document.getElementById('ssid_select');
      p.append('ssid', sel.value);
      p.append('pass', document.getElementById('pass').value);
      const isDhcp = document.getElementById('dhcp').value;
      p.append('dhcp', isDhcp);
      const ipVal = document.getElementById('ip').value;
      if (isDhcp === "0") {
        p.append('ip', ipVal);
        p.append('mask', document.getElementById('mask').value);
        p.append('gw', document.getElementById('gw').value);
        p.append('dns', document.getElementById('dns').value);
      }

      fetch('/save', { method: 'POST', body: p }).then(res => {
        if (!res.ok) return res.text().then(t => { throw new Error(t); });
        document.getElementById('setup_box').style.display = 'none';
        document.getElementById('overlay').style.display = 'block';

        let count = 8;
        const targetHost = (isDhcp === "0") ? ipVal : "ipmi.local";
        const timer = setInterval(() => {
          count--;
          document.getElementById('countdown').innerText = 'Redirecting to http://' + targetHost + ' in ' + count + 's...';
          if (count <= 0) {
            clearInterval(timer);
            window.location.href = 'http://' + targetHost;
          }
        }, 1000);
      }).catch(err => {
        errBox.innerText = err.message;
        errBox.style.display = 'block';
      });
    }

    window.onload = startScan;
  </script>
</body></html>
)rawliteral";

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="utf-8">
  <title>IPC-R300 mini-IPMI</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 15px; background: #1e1e2e; color: #cdd6f4; }
    .top-bar { display: flex; justify-content: space-between; align-items: center; max-width: 340px; margin: 0 auto 10px; }
    h2 { color: #89b4fa; margin: 0; }
    .btn-logout { color: #f38ba8; text-decoration: none; font-size: 13px; font-weight: bold; border: 1px solid #45475a; padding: 4px 8px; border-radius: 4px; }
    .card { background: #313244; padding: 15px; border-radius: 8px; max-width: 340px; margin: 10px auto; text-align: left; box-sizing: border-box; }
    .metric { margin: 6px 0; font-size: 14px; display: flex; justify-content: space-between; align-items: center; }
    .metric-val { font-weight: bold; font-family: monospace; font-size: 13px; }
    .metric-ok { color: #a6e3a1; }
    .metric-bad { color: #f38ba8; }
    .metric-info { color: #89b4fa; }
    .desc-box { background: #181825; padding: 8px 10px; border-radius: 4px; margin-top: 8px; font-size: 13px; color: #f9e2af; border-left: 3px solid #89b4fa; word-break: break-word; }
    .btn { display: block; width: 100%; max-width: 250px; margin: 8px auto; padding: 12px; font-size: 15px; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; }
    .btn-pwr { background: #a6e3a1; color: #11111b; }
    .btn-force { background: #f38ba8; color: #11111b; }
    .btn-rst { background: #fab387; color: #11111b; }
    .btn-cfg { background: #89b4fa; color: #11111b; }
    .btn-danger { background: #eba0ac; color: #11111b; font-size: 12px; padding: 8px; margin-top: 15px; }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    input[type="text"], input[type="number"], input[type="password"] { width: 100%; padding: 7px; margin-top: 4px; box-sizing: border-box; border-radius: 4px; border: 1px solid #45475a; background: #181825; color: #cdd6f4; }
    label { font-size: 12px; color: #a6adc8; display: block; margin-top: 6px; }
    .check-label { display: flex; align-items: center; margin-top: 8px; cursor: pointer; font-size: 12px; }
    .check-label input { width: auto; margin-right: 8px; }
    #status { margin-top: 10px; font-style: italic; color: #a6adc8; font-size: 13px; }
  </style>
</head>
<body>
  <div class="top-bar">
    <h2>Mini-IPMI</h2>
    <a href="#" class="btn-logout" onclick="return doLogout(event)">Logout</a>
  </div>

  <div class="card">
    <div style="font-weight:bold; color:#89b4fa; margin-bottom:8px; border-bottom:1px solid #45475a; padding-bottom:4px;">EC Hardware State</div>
    <div class="metric">Platform Status: <span id="m_stat" class="metric-val">Checking...</span></div>
    <div class="metric">Power Rail: <span id="m_pwr" class="metric-val">-</span></div>
    <div class="metric">Current POST Code: <span id="m_hex" class="metric-val">-</span></div>
    <div class="metric">Host Uptime: <span id="m_huptime" class="metric-val">-</span></div>
    <div class="metric">ESP32 Uptime: <span id="m_euptime" class="metric-val">-</span></div>
    <div class="desc-box" id="m_desc">Listening to EC UART...</div>
  </div>

  <button class="btn btn-pwr action-btn" onclick="sendCmd('/pwr/pulse')">Power on/off</button>
  <button class="btn btn-force action-btn" onclick="sendCmd('/pwr/force')">Hard Power Off (5s)</button>
  <button class="btn btn-rst action-btn" onclick="sendCmd('/reset')">Reset</button>

  <div class="card">
    <div style="font-weight:bold; color:#89b4fa; cursor:pointer;" onclick="toggleMqttForm()">
      &#9881; MQTT Settings (<span id="mqtt_state">click to expand</span>)
    </div>
    <div id="mqtt_form" style="display:none; margin-top:8px;">
      <label>Broker IP</label>
      <input type="text" id="cfg_host">
      <label>Port (1-65535)</label>
      <input type="number" id="cfg_port">
      <label>User</label>
      <input type="text" id="cfg_user">
      <label>Password</label>
      <input type="password" id="cfg_pass" placeholder="Leave blank to keep unchanged">
      <label class="check-label">
        <input type="checkbox" id="cfg_clear_pass"> Remove/clear password
      </label>
      <button class="btn btn-cfg" style="width:100%; margin-top:10px;" onclick="saveMqtt()">Save MQTT</button>
      <button class="btn btn-danger" style="width:100%;" onclick="resetWifi()">Reset Wi-Fi & Re-enter AP</button>
    </div>
  </div>

  <div class="card">
    <div style="font-weight:bold; color:#89b4fa; cursor:pointer;" onclick="toggleAuthForm()">
      &#128273; Security / Password (<span id="auth_state">click to expand</span>)
    </div>
    <div id="auth_form" style="display:none; margin-top:8px;">
      <label>Username</label>
      <input type="text" id="cfg_auth_user" placeholder="admin">
      <label>New Password (min 4 characters)</label>
      <input type="password" id="cfg_auth_pass" placeholder="New password">
      <button class="btn btn-cfg" style="width:100%; margin-top:10px;" onclick="saveAuth()">Update Password</button>
    </div>
  </div>

  <div id="status">Ready</div>

  <script>
    function fmtUptime(sec) {
      if (!sec || sec <= 0) return "Down";
      const d = Math.floor(sec / 86400);
      const h = Math.floor((sec % 86400) / 3600);
      const m = Math.floor((sec % 3600) / 60);
      const s = sec % 60;
      if (d > 0) return d + "d " + h + "h " + m + "m";
      if (h > 0) return h + "h " + m + "m " + s + "s";
      return m + "m " + s + "s";
    }

    function doLogout(e) {
      if (e) e.preventDefault();
      if (!confirm('Are you sure you want to log out?')) return false;

      // Отправляем неверные учетные данные для сброса Basic Auth в кэше браузера
      const xhr = new XMLHttpRequest();
      xhr.open("GET", "/logout", true, "invalid_user", "invalid_pass");
      xhr.onreadystatechange = function () {
        if (xhr.readyState === 4) {
          window.location.href = "/";
        }
      };
      xhr.send();
      return false;
    }

    function updateMetrics() {
      fetch('/api/metrics').then(r => {
        if (r.status === 401) { window.location.reload(); return; }
        return r.json();
      }).then(d => {
        if (!d) return;
        const s = document.getElementById('m_stat');
        if (d.status === "online") {
          s.innerText = "Online (S0)";
          s.className = "metric-val metric-ok";
        } else if (d.status === "booting") {
          s.innerText = "Booting (POST)";
          s.className = "metric-val metric-info";
        } else if (d.status === "hung") {
          s.innerText = "POST HUNG";
          s.className = "metric-val metric-bad";
        } else {
          s.innerText = "Offline (S4/S5)";
          s.className = "metric-val metric-bad";
        }

        document.getElementById('m_pwr').innerText = d.power;
        document.getElementById('m_hex').innerText = '0x' + d.post_hex;
        document.getElementById('m_huptime').innerText = (d.status === "online") ? fmtUptime(d.host_uptime) : "Down";
        document.getElementById('m_euptime').innerText = fmtUptime(d.esp_uptime);

        const descEl = document.getElementById('m_desc');
        descEl.innerText = d.description;
        descEl.style.borderLeftColor = d.error ? '#f38ba8' : (d.status === 'online' ? '#a6e3a1' : '#89b4fa');
      }).catch(() => {});
    }
    setInterval(updateMetrics, 2000);
    updateMetrics();

    function sendCmd(endpoint) {
      const b = document.querySelectorAll('.action-btn');
      b.forEach(x => x.disabled = true);
      document.getElementById('status').innerText = 'Sending command...';

      fetch(endpoint, { method: 'POST' }).then(r => {
        if (r.status === 401) { window.location.reload(); return; }
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.text();
      }).then(t => {
        if (t) document.getElementById('status').innerText = t;
      }).catch(e => {
        document.getElementById('status').innerText = 'Error: ' + e.message;
      }).finally(() => {
        b.forEach(x => x.disabled = false);
      });
    }

    function toggleMqttForm() {
      const f = document.getElementById('mqtt_form');
      const isHidden = (f.style.display === 'none');
      f.style.display = isHidden ? 'block' : 'none';
      document.getElementById('mqtt_state').innerText = isHidden ? 'click to collapse' : 'click to expand';
      if (isHidden) {
        fetch('/api/settings/mqtt').then(r => {
          if (r.status === 401) { window.location.reload(); return; }
          return r.json();
        }).then(d => {
          if (!d) return;
          document.getElementById('cfg_host').value = d.host;
          document.getElementById('cfg_port').value = d.port;
          document.getElementById('cfg_user').value = d.user;
          document.getElementById('cfg_pass').value = '';
          document.getElementById('cfg_clear_pass').checked = false;
        });
      }
    }

    function saveMqtt() {
      const p = new URLSearchParams();
      p.append('mq_host', document.getElementById('cfg_host').value);
      p.append('mq_port', document.getElementById('cfg_port').value);
      p.append('mq_user', document.getElementById('cfg_user').value);
      
      const clearPass = document.getElementById('cfg_clear_pass').checked;
      p.append('clear_pass', clearPass ? "1" : "0");

      const pass = document.getElementById('cfg_pass').value;
      if (pass) p.append('mq_pass', pass);

      document.getElementById('status').innerText = 'Queuing MQTT save...';
      fetch('/api/settings/mqtt', { method: 'POST', body: p })
        .then(r => {
          if (r.status === 401) { window.location.reload(); return; }
          if (!r.ok) return r.text().then(t => { throw new Error(t); });
          return r.text();
        })
        .then(t => {
          document.getElementById('status').innerText = t;
          toggleMqttForm();
        }).catch(e => {
          document.getElementById('status').innerText = 'Error: ' + e.message;
        });
    }

    function toggleAuthForm() {
      const f = document.getElementById('auth_form');
      const isHidden = (f.style.display === 'none');
      f.style.display = isHidden ? 'block' : 'none';
      document.getElementById('auth_state').innerText = isHidden ? 'click to collapse' : 'click to expand';
    }

    function saveAuth() {
      const u = document.getElementById('cfg_auth_user').value.trim();
      const p = document.getElementById('cfg_auth_pass').value.trim();
      if (!u || u.length > 31) {
        alert('Username must be 1 to 31 characters');
        return;
      }
      if (p.length < 4 || p.length > 63) {
        alert('Password must be 4 to 63 characters');
        return;
      }

      const params = new URLSearchParams();
      params.append('user', u);
      params.append('pass', p);

      fetch('/api/settings/auth', { method: 'POST', body: params })
        .then(r => {
          if (r.status === 401) { window.location.reload(); return; }
          if (!r.ok) return r.text().then(t => { throw new Error(t); });
          alert('Credentials updated! You will now be redirected to log in with new password.');
          window.location.href = '/logout';
        })
        .catch(e => alert('Error: ' + e.message));
    }

    function resetWifi() {
      if (confirm('Erase Wi-Fi settings and start AP Mode?')) {
        fetch('/api/reset_wifi', { method: 'POST' }).then(r => {
          if (!r.ok) throw new Error('Busy');
          document.getElementById('status').innerText = 'Controller restarting into AP Mode...';
        }).catch(() => {
          document.getElementById('status').innerText = 'Error: System busy';
        });
      }
    }
  </script>
</body></html>
)rawliteral";

// ============================================================
// MQTT Реконнект и обработка команд
// ============================================================
void checkMqtt() {
    if (isApMode || WiFi.status() != WL_CONNECTED) return;

    if (!mqttClient.connected()) {
        static uint32_t lastMqtt = 0;
        if (millis() - lastMqtt > 5000) {
            lastMqtt = millis();
            String cid = "ESP32-IPMI-" + String(WiFi.macAddress());
            
            String user, pass;
            if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                user = appCfg.mqtt_user;
                pass = appCfg.mqtt_pass;
                xSemaphoreGive(cfgMutex);
            }

            if (mqttClient.connect(cid.c_str(), user.c_str(), pass.c_str(), "homeassistant/sensor/mini_ipmi/status", 1, true, "offline")) {
                Serial.println("[MQTT] Connected!");
                publishPcState();
                mqttClient.subscribe("homeassistant/switch/mini_ipmi/power/set");
                mqttClient.subscribe("homeassistant/switch/mini_ipmi/reset/set");
            }
        }
    } else {
        mqttClient.loop();
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg((char*)payload, length);
    msg.trim();
    String t = String(topic);

    SystemCommand cmd;
    memset(&cmd, 0, sizeof(cmd));

    if (t == "homeassistant/switch/mini_ipmi/power/set") {
        if (msg == "PRESS" || msg == "PULSE" || msg == "ON") {
            cmd.action = ACT_POWER_PULSE;
        } else if (msg == "FORCE_OFF") {
            cmd.action = ACT_POWER_FORCE;
        }

        if (cmd.action != ACT_NONE) {
            if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "OK");
                mqttClient.publish(topic, "", true);
            } else {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "BUSY");
            }
        }
    } else if (t == "homeassistant/switch/mini_ipmi/reset/set") {
        if (msg == "PRESS" || msg == "RESET" || msg == "ON") {
            cmd.action = ACT_RESET_PULSE;
            if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "OK");
                mqttClient.publish(topic, "", true);
            } else {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "BUSY");
            }
        }
    }
}

// ============================================================
// Запуск точки доступа (Captive Portal)
// ============================================================
void startCaptivePortal() {
    isApMode = true;
    Serial.println("\n[WIFI] Entering AP_STA Mode (Captive Portal)...");

    WiFi.mode(WIFI_AP_STA);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("IPMI-Config-AP", "P@$$w0rd");

    dnsServer.start(DNS_PORT, "*", apIP);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html; charset=utf-8", config_html);
    });

    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        int n = WiFi.scanComplete();

        if (n == -2) {
            WiFi.scanNetworks(true);
            request->send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
            return;
        }

        if (n == -1) {
            request->send(200, "application/json", "{\"scanning\":true,\"networks\":[]}");
            return;
        }

        JsonDocument doc;
        doc["scanning"] = false;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; ++i) {
            JsonObject item = arr.add<JsonObject>();
            item["ssid"] = WiFi.SSID(i);
            item["rssi"] = WiFi.RSSI(i);
        }
        WiFi.scanDelete();
        WiFi.scanNetworks(true);

        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        String ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : "";
        String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";
        bool dhcp   = request->hasParam("dhcp", true) ? (request->getParam("dhcp", true)->value() == "1") : true;
        String ip   = request->hasParam("ip", true) ? request->getParam("ip", true)->value() : "";
        String mask = request->hasParam("mask", true) ? request->getParam("mask", true)->value() : "";
        String gw   = request->hasParam("gw", true) ? request->getParam("gw", true)->value() : "";
        String dns  = request->hasParam("dns", true) ? request->getParam("dns", true)->value() : "";

        if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
            request->send(400, "text/plain", "Invalid SSID or password length");
            return;
        }

        if (!dhcp) {
            IPAddress tIp, tMask, tGw, tDns;
            if (!tIp.fromString(ip) || !tMask.fromString(mask) || !tGw.fromString(gw) || !tDns.fromString(dns)) {
                request->send(400, "text/plain", "Invalid Static IP configuration");
                return;
            }
        }

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_SAVE_WIFI;
        safeStrCopy(cmd.wifiPayload.ssid, ssid.c_str(), sizeof(cmd.wifiPayload.ssid));
        safeStrCopy(cmd.wifiPayload.pass, pass.c_str(), sizeof(cmd.wifiPayload.pass));
        cmd.wifiPayload.dhcp = dhcp;
        safeStrCopy(cmd.wifiPayload.ip, ip.c_str(), sizeof(cmd.wifiPayload.ip));
        safeStrCopy(cmd.wifiPayload.mask, mask.c_str(), sizeof(cmd.wifiPayload.mask));
        safeStrCopy(cmd.wifiPayload.gw, gw.c_str(), sizeof(cmd.wifiPayload.gw));
        safeStrCopy(cmd.wifiPayload.dns, dns.c_str(), sizeof(cmd.wifiPayload.dns));

        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            request->send(200, "text/plain", "OK");
        } else {
            request->send(500, "text/plain", "Queue full, retry");
        }
    });

    server.begin();
    WiFi.scanNetworks(true);
}

// ============================================================
// Setup
// ============================================================
void setup() {
    digitalWrite(PIN_PWR, LOW);
    digitalWrite(PIN_RESET, LOW);
    pinMode(PIN_PWR, OUTPUT);
    pinMode(PIN_RESET, OUTPUT);

    digitalWrite(PIN_LED, HIGH);
    pinMode(PIN_LED, OUTPUT);

    pinMode(PIN_BOOT, INPUT_PULLUP);
    pinMode(UART_RX_PIN, INPUT_PULLDOWN);

    // Проверка сброса учетных данных по удержанию кнопки BOOT (GPIO9) в течение 3 секунд
    if (digitalRead(PIN_BOOT) == LOW) {
        unsigned long pressStart = millis();
        bool stillPressed = true;
        while (millis() - pressStart < 3000) {
            digitalWrite(PIN_LED, (millis() / 100) % 2); // Быстрое мигание
            delay(10);
            if (digitalRead(PIN_BOOT) == HIGH) {
                stillPressed = false;
                break;
            }
        }
        if (stillPressed) {
            prefs.begin("ipmi_cfg", false);
            prefs.remove("http_user");
            prefs.remove("http_pass");
            prefs.end();
            safeStrCopy(cachedHttpUser, "admin", sizeof(cachedHttpUser));
            safeStrCopy(cachedHttpPass, "admin", sizeof(cachedHttpPass));
            // Сигнал об успешном сбросе (горит 1 секунду)
            digitalWrite(PIN_LED, LOW);
            delay(1000);
        }
        digitalWrite(PIN_LED, HIGH);
    }

    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    uint32_t serialStart = millis();
    while (!Serial && (millis() - serialStart < 2000)) delay(10);

    Serial.println("\n[SYSTEM] ESP32-C3 mini-IPMI v6.0 starting...");
    bootTime = millis();

    cfgMutex = xSemaphoreCreateMutex();
    authMutex = xSemaphoreCreateMutex();
    authDoneSem = xSemaphoreCreateBinary();
    cmdQueue = xQueueCreate(10, sizeof(SystemCommand));

    if (cfgMutex == nullptr || authMutex == nullptr || authDoneSem == nullptr || cmdQueue == nullptr) {
        Serial.println("[FATAL] Failed to allocate FreeRTOS primitives! System halted.");
        while (true) {
            digitalWrite(PIN_LED, LOW);
            delay(100);
            digitalWrite(PIN_LED, HIGH);
            delay(100);
        }
    }

    String currentSsid = "";
    String currentPass = "";
    bool currentDhcp = true;
    String currentIp = "10.10.11.12";
    String currentGw = "10.10.11.1";
    String currentMask = "255.255.255.0";
    String currentDns = "10.10.11.1";

    if (!loadSettings()) {
        Serial.println("[WARN] Using fail-safe defaults for Wi-Fi.");
    } else {
        if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            currentSsid = appCfg.wifi_ssid;
            currentPass = appCfg.wifi_pass;
            currentDhcp = appCfg.wifi_dhcp;
            currentIp   = appCfg.wifi_ip;
            currentGw   = appCfg.wifi_gw;
            currentMask = appCfg.wifi_mask;
            currentDns  = appCfg.wifi_dns;
            xSemaphoreGive(cfgMutex);
        } else {
            Serial.println("[WARN] Timeout acquiring cfgMutex in setup()! Falling back to AP mode.");
            currentSsid = "";
        }
    }

    if (currentSsid.length() == 0) {
        startCaptivePortal();
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    if (!currentDhcp && currentIp.length() > 0) {
        IPAddress localIP, gateway, subnet, dns;
        if (localIP.fromString(currentIp) && gateway.fromString(currentGw) &&
            subnet.fromString(currentMask) && dns.fromString(currentDns)) {
            WiFi.config(localIP, gateway, subnet, dns);
        }
    }

    WiFi.begin(currentSsid.c_str(), currentPass.c_str());

    uint32_t wifiWait = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWait < 12000)) {
        updatePostFSM();
        checkAutoPower();
        updatePulse();
        delay(20);
        if (restartPending) updateRestart();
    }

    if (WiFi.status() != WL_CONNECTED) {
        startCaptivePortal();
        return;
    }

    Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(PIN_LED, HIGH);

    if (MDNS.begin("ipmi")) {
        MDNS.addService("http", "tcp", 80);
    }

    if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        mqttClient.setServer(appCfg.mqtt_host.c_str(), appCfg.mqtt_port);
        xSemaphoreGive(cfgMutex);
    }
    
    if (!mqttClient.setBufferSize(512)) {
        Serial.println("[WARN] PubSubClient buffer allocation failed! Buffer remains at default 256 bytes.");
    }
    mqttClient.setCallback(mqttCallback);

    // ============================================================
    // Маршруты веб-сервера
    // ============================================================
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();
        req->send(200, "text/html; charset=utf-8", index_html);
    });

    server.on("/logout", HTTP_GET, [](AsyncWebServerRequest *req) {
        // Если клиент пришел с валидными учетными данными — запрашиваем ввод заново (сброс кэша браузера)
        if (isAuthenticated(req)) {
            return req->requestAuthentication();
        }
        // Как только браузер отказался от старых данных — сразу перенаправляем на главную
        req->redirect("/");
    });

    server.on("/api/metrics", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        portENTER_CRITICAL(&statusMux);
        HostState st = pcStatus.state;
        char codeHex[16];
        safeStrCopy(codeHex, pcStatus.lastCodeHex, sizeof(codeHex));
        char desc[64];
        safeStrCopy(desc, pcStatus.postDescription, sizeof(desc));
        char pwr[32];
        safeStrCopy(pwr, pcStatus.pwrState, sizeof(pwr));
        bool err = pcStatus.isError;
        uint64_t startSec = pcStatus.onlineStartSec;
        portEXIT_CRITICAL(&statusMux);

        JsonDocument doc;
        if (st == HOST_ONLINE) doc["status"] = "online";
        else if (st == HOST_BOOTING) doc["status"] = "booting";
        else if (st == HOST_HANG) doc["status"] = "hung";
        else doc["status"] = "offline";

        doc["power"] = pwr;
        doc["post_hex"] = codeHex;
        doc["description"] = desc;
        doc["error"] = err;

        uint64_t currentSec = getUptimeSec();
        doc["esp_uptime"] = currentSec;
        if (st == HOST_ONLINE && startSec > 0 && currentSec >= startSec) {
            doc["host_uptime"] = currentSec - startSec;
        } else {
            doc["host_uptime"] = 0;
        }

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/pwr/pulse", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_POWER_PULSE;
        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            req->send(200, "text/plain", "Power pulse queued");
        } else {
            req->send(409, "text/plain", "Busy / Queue Full");
        }
    });

    server.on("/pwr/force", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_POWER_FORCE;
        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            req->send(200, "text/plain", "Force off queued (5s)");
        } else {
            req->send(409, "text/plain", "Busy / Queue Full");
        }
    });

    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_RESET_PULSE;
        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            req->send(200, "text/plain", "Reset pulse queued");
        } else {
            req->send(409, "text/plain", "Busy / Queue Full");
        }
    });

    server.on("/api/settings/mqtt", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        JsonDocument doc;
        if (xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            doc["host"] = appCfg.mqtt_host;
            doc["port"] = appCfg.mqtt_port;
            doc["user"] = appCfg.mqtt_user;
            xSemaphoreGive(cfgMutex);
        } else {
            req->send(503, "text/plain", "Resource busy");
            return;
        }
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/api/settings/mqtt", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        String host = req->hasParam("mq_host", true) ? req->getParam("mq_host", true)->value() : "";
        String portStr = req->hasParam("mq_port", true) ? req->getParam("mq_port", true)->value() : "";
        String user = req->hasParam("mq_user", true) ? req->getParam("mq_user", true)->value() : "";
        String pass = req->hasParam("mq_pass", true) ? req->getParam("mq_pass", true)->value() : "";
        bool clearPass = req->hasParam("clear_pass", true) ? (req->getParam("clear_pass", true)->value() == "1") : false;

        host.trim();
        portStr.trim();
        user.trim();

        int port = 0;
        if (!parseStrictPort(portStr, port)) {
            req->send(400, "text/plain", "Invalid TCP port (1-65535 only)");
            return;
        }

        if (host.length() == 0 || host.length() > 63 || user.length() > 31 || pass.length() > 63) {
            req->send(400, "text/plain", "Invalid input or length exceeds buffer limit");
            return;
        }

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_SAVE_MQTT;
        safeStrCopy(cmd.mqttPayload.host, host.c_str(), sizeof(cmd.mqttPayload.host));
        cmd.mqttPayload.port = port;
        safeStrCopy(cmd.mqttPayload.user, user.c_str(), sizeof(cmd.mqttPayload.user));
        safeStrCopy(cmd.mqttPayload.pass, pass.c_str(), sizeof(cmd.mqttPayload.pass));
        cmd.mqttPayload.clearPass = clearPass;

        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            req->send(200, "text/plain", "MQTT settings queued for update!");
        } else {
            req->send(409, "text/plain", "Command queue busy");
        }
    });

    server.on("/api/settings/auth", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        String user = req->hasParam("user", true) ? req->getParam("user", true)->value() : "";
        String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";

        user.trim();
        pass.trim();

        if (user.length() == 0 || user.length() > 31 || pass.length() < 4 || pass.length() > 63) {
            req->send(400, "text/plain", "User (1-31 chars) or Pass (4-63 chars) invalid");
            return;
        }

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_SAVE_AUTH;
        safeStrCopy(cmd.authPayload.user, user.c_str(), sizeof(cmd.authPayload.user));
        safeStrCopy(cmd.authPayload.pass, pass.c_str(), sizeof(cmd.authPayload.pass));

        // Очищаем предыдущее состояние семафора перед отправкой команды
        xSemaphoreTake(authDoneSem, 0);

        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            // Ждем фактического применения в loopTask до 1.5 секунд
            if (xSemaphoreTake(authDoneSem, pdMS_TO_TICKS(1500)) == pdTRUE) {
                req->send(200, "text/plain", "Credentials applied successfully!");
            } else {
                req->send(504, "text/plain", "Timeout applying credentials");
            }
        } else {
            req->send(409, "text/plain", "Command queue busy");
        }
    });

    server.on("/api/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!isAuthenticated(req)) return req->requestAuthentication();

        SystemCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.action = ACT_RESET_WIFI;
        if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
            req->send(200, "text/plain", "Rebooting to AP...");
        } else {
            req->send(409, "text/plain", "Command queue busy");
        }
    });

    server.begin();
    Serial.println("[SERVER] Production Web Server started with Fast Basic Auth.");
}

// ============================================================
// Loop (loopTask)
// ============================================================
void loop() {
    updateLed();

    if (isApMode) {
        dnsServer.processNextRequest();
        updatePulse();
        updatePostFSM();
        checkAutoPower();
        updateRestart();
        processIncomingCommands();
        delay(5);
        return;
    }

    processIncomingCommands();
    updatePulse();
    updatePostFSM();
    checkMqtt();
    checkAutoPower();
    updateRestart();

    delay(2);
}
