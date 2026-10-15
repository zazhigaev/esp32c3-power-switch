#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

// ============================================================
// Аппаратные выводы
// ============================================================
const int PIN_PWR     = 1;  // Оптопара Power SW
const int PIN_RESET   = 3;  // Оптопара Reset
const int UART_RX_PIN = 4;  // RX ESP32 <- TX платы IPC-R300
const int UART_TX_PIN = 5;  // TX ESP32 -> RX платы IPC-R300
const int PIN_LED     = 8;  // Синий светодиод платы Super Mini

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
} appCfg;

Preferences prefs;

// ============================================================
// Глобальные объекты
// ============================================================
bool isApMode = false;
DNSServer dnsServer;
const byte DNS_PORT = 53;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

AsyncWebServer server(80);
AsyncServer consoleServer(2323);
AsyncClient* activeClient = nullptr;

// Импульсы
bool pulseActive = false;
int pulsePin = -1;
unsigned long pulseEndTime = 0;

// Автозапуск
const unsigned long AUTO_POWER_DELAY = 5000;
unsigned long bootTime = 0;
bool autoPowerTriggered = false;

// Отложенная перезагрузка
bool restartPending = false;
unsigned long restartAt = 0;

// Телеметрия FSM
enum TelemetryState { TEL_IDLE, TEL_SEND_CMD, TEL_WAIT_RESP, TEL_PAUSED };
TelemetryState telState = TEL_IDLE;
unsigned long lastTelemetryPoll = 0;
unsigned long cmdSentTime = 0;
const unsigned long POLL_INTERVAL = 30000;
const unsigned long CMD_TIMEOUT   = 5000;

// Фиксированный статический буфер для исключения фрагментации heap
char uartBuffer[512];
size_t uartBufferLen = 0;

enum HostState {
    HOST_UNKNOWN = 0,
    HOST_ONLINE  = 1,
    HOST_OFFLINE = 2
};

struct SystemMetrics {
    float temp = 0.0;
    float load1 = 0.0;
    int memFreeMb = 0;
    int memTotalMb = 0;
    int sdaSmart = -1;
    int sdbSmart = -1;
    HostState hostState = HOST_UNKNOWN;
} metrics;

// ============================================================
// Индикация светодиодом (GPIO8)
// ============================================================
void updateLed() {
    if (isApMode) {
        static unsigned long lastBlink = 0;
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
// NVS Чтение / Запись
// ============================================================
void loadSettings() {
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
    prefs.end();
}

void saveWifiSettings(const String& ssid, const String& pass, bool dhcp, const String& ip, const String& mask, const String& gw, const String& dns) {
    prefs.begin("ipmi_cfg", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putBool("dhcp", dhcp);
    prefs.putString("ip", ip);
    prefs.putString("mask", mask);
    prefs.putString("gw", gw);
    prefs.putString("dns", dns);
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
bool startPulse(int pin, unsigned long durationMs) {
    if (pulseActive) return false;
    digitalWrite(pin, HIGH);
    pulsePin = pin;
    pulseEndTime = millis() + durationMs;
    pulseActive = true;
    Serial.printf("[PULSE] GPIO%d HIGH for %lu ms\n", pin, durationMs);
    return true;
}

void updatePulse() {
    if (!pulseActive) return;
    if ((long)(millis() - pulseEndTime) >= 0) {
        digitalWrite(pulsePin, LOW);
        Serial.printf("[PULSE] GPIO%d LOW\n", pulsePin);
        pulsePin = -1;
        pulseActive = false;
    }
}

void scheduleRestart(unsigned long delayMs = 1000) {
    restartPending = true;
    restartAt = millis() + delayMs;
}

void updateRestart() {
    if (!restartPending) return;
    if ((long)(millis() - restartAt) >= 0) {
        Serial.println("[SYSTEM] Restarting...");
        ESP.restart();
    }
}

// ============================================================
// Проверка активности хоста и автозапуск
// ============================================================
bool isHostAlreadyOn() {
    // 1. Проверяем наличие входящих байт в буфере UART
    if (Serial1.available() > 0) {
        Serial.println("[AUTO] Host is active: incoming UART traffic detected.");
        return true;
    }

    // 2. Проверяем физический уровень на линии TX материнской платы (GPIO4)
    // В дежурном режиме S5 основная системная шина обесточена (уровень LOW)
    if (digitalRead(UART_RX_PIN) == LOW) {
        Serial.println("[AUTO] Host is OFF: UART RX pin is LOW (no VCC).");
        return false;
    }

    // 3. Линия подтянута к HIGH -> пингуем шелл переводом строки
    Serial.println("[AUTO] UART RX is HIGH. Sending probe newline...");
    while (Serial1.available()) Serial1.read();
    Serial1.print("\n");

    unsigned long waitStart = millis();
    while (millis() - waitStart < 500) {
        if (Serial1.available() > 0) {
            Serial.println("[AUTO] Host is active: responded to UART ping.");
            return true;
        }
        delay(10);
    }

    Serial.println("[AUTO] Host did not respond to probe. Assuming OFF.");
    return false;
}

void checkAutoPower() {
    if (autoPowerTriggered || pulseActive || (millis() - bootTime < AUTO_POWER_DELAY)) return;

    if (isHostAlreadyOn()) {
        Serial.println("[AUTO] Host confirmed running. Skipping power pulse.");
        autoPowerTriggered = true;
        metrics.hostState = HOST_ONLINE;
        return;
    }

    Serial.println("[AUTO] Host confirmed OFF. Triggering initial power-on pulse...");
    if (startPulse(PIN_PWR, 300)) {
        autoPowerTriggered = true;
    }
}

// ============================================================
// Wi-Fi Events (STA автоматический реконнект)
// ============================================================
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("[WIFI] Connection lost. Background auto-reconnect active...");
            break;
        default:
            break;
    }
}

// ============================================================
// HTML Captive Portal (Мастер настройки Wi-Fi)
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
        <option value="">Scanning networks...</option>
      </select>
      <button type="button" class="btn-scan" onclick="scanWifi()">Rescan Networks</button>

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
    function toggleIpFields() {
      const isStatic = document.getElementById('dhcp').value === "0";
      document.getElementById('static_fields').style.display = isStatic ? "block" : "none";
    }

    function scanWifi() {
      const sel = document.getElementById('ssid_select');
      sel.innerHTML = '<option value="">Scanning...</option>';
      fetch('/scan').then(r => r.json()).then(list => {
        sel.innerHTML = '';
        if (list.length === 0) {
          sel.innerHTML = '<option value="">No networks found</option>';
          return;
        }
        list.forEach(item => {
          const opt = document.createElement('option');
          opt.value = item.ssid;
          opt.text = item.ssid + ' (' + item.rssi + ' dBm)';
          sel.appendChild(opt);
        });
      }).catch(() => {
        sel.innerHTML = '<option value="">Error scanning</option>';
      });
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

    window.onload = scanWifi;
  </script>
</body></html>
)rawliteral";

// ============================================================
// HTML Рабочий интерфейс
// ============================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="utf-8">
  <title>IPC-R300 mini-IPMI</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 15px; background: #1e1e2e; color: #cdd6f4; }
    h2 { color: #89b4fa; margin-bottom: 5px; }
    .card { background: #313244; padding: 15px; border-radius: 8px; max-width: 320px; margin: 10px auto; text-align: left; }
    .metric { margin: 4px 0; font-size: 14px; }
    .metric-ok { color: #a6e3a1; }
    .metric-bad { color: #f38ba8; }
    .metric-na { color: #a6adc8; }
    .metric-unk { color: #f9e2af; }
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
    #status { margin-top: 10px; font-style: italic; color: #a6adc8; }
  </style>
</head>
<body>
  <h2>Mini-IPMI Control</h2>
  <div class="card">
    <div style="font-weight:bold; color:#f9e2af; margin-bottom:6px;">Telemetry</div>
    <div class="metric">Status: <span id="m_stat">Checking...</span></div>
    <div class="metric">CPU Temp: <span id="m_temp">-</span> &deg;C</div>
    <div class="metric">Load Avg: <span id="m_load">-</span></div>
    <div class="metric">RAM Free: <span id="m_ram">-</span> MB</div>
    <div class="metric">SDA SMART: <span id="m_sda">-</span></div>
    <div class="metric">SDB SMART: <span id="m_sdb">-</span></div>
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
        <input type="checkbox" id="cfg_clear_pass"> Clear mqtt password
      </label>
      <button class="btn btn-cfg" style="width:100%; margin-top:10px;" onclick="saveMqtt()">Save MQTT</button>
      <button class="btn btn-danger" style="width:100%;" onclick="resetWifi()">Reset Wi-Fi & Re-enter AP</button>
    </div>
  </div>

  <div id="status">Ready</div>

  <script>
    function smartText(val) {
      if (val === 1) return { text: "OK", cls: "metric-ok" };
      if (val === 0) return { text: "BAD", cls: "metric-bad" };
      return { text: "N/A", cls: "metric-na" };
    }

    function setSmartElement(id, val) {
      const el = document.getElementById(id);
      const res = smartText(val);
      el.innerText = res.text;
      el.className = res.cls;
    }

    function updateMetrics() {
      fetch('/api/metrics').then(r => r.json()).then(d => {
        const s = document.getElementById('m_stat');
        if (d.status === "online") {
          s.innerText = "Online";
          s.className = "metric-ok";
        } else if (d.status === "offline") {
          s.innerText = "Offline";
          s.className = "metric-bad";
        } else {
          s.innerText = "Pending...";
          s.className = "metric-unk";
        }

        document.getElementById('m_temp').innerText = (d.status === "online") ? d.temp : "-";
        document.getElementById('m_load').innerText = (d.status === "online") ? d.load : "-";
        document.getElementById('m_ram').innerText = (d.status === "online") ? (d.mfree + ' / ' + d.mtotal) : "-";
        setSmartElement('m_sda', (d.status === "online") ? d.sda : -1);
        setSmartElement('m_sdb', (d.status === "online") ? d.sdb : -1);
      }).catch(() => {});
    }
    setInterval(updateMetrics, 5000);
    updateMetrics();

    function sendCmd(endpoint) {
      const b = document.querySelectorAll('.action-btn');
      b.forEach(x => x.disabled = true);
      document.getElementById('status').innerText = 'Sending...';

      fetch(endpoint, { method: 'POST' }).then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.text();
      }).then(t => {
        document.getElementById('status').innerText = t;
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
        fetch('/api/settings/mqtt').then(r => r.json()).then(d => {
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

      document.getElementById('status').innerText = 'Saving MQTT...';
      fetch('/api/settings/mqtt', { method: 'POST', body: p })
        .then(r => {
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

    function resetWifi() {
      if (confirm('Erase Wi-Fi settings and start AP Mode?')) {
        fetch('/api/reset_wifi', { method: 'POST' }).then(() => {
          document.getElementById('status').innerText = 'Controller restarting in AP Mode...';
        });
      }
    }
  </script>
</body></html>
)rawliteral";

// ============================================================
// Запуск точки доступа (Captive Portal)
// ============================================================
void startCaptivePortal() {
    isApMode = true;
    Serial.println("\n[WIFI] Entering AP Mode (Captive Portal)...");

    WiFi.mode(WIFI_AP);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("IPMI-Config-AP", "P@$$w0rd");

    dnsServer.start(DNS_PORT, "*", apIP);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html; charset=utf-8", config_html);
    });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html; charset=utf-8", config_html);
    });
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html; charset=utf-8", config_html);
    });

    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        int n = WiFi.scanComplete();
        if (n == -2) {
            WiFi.scanNetworks(true);
            request->send(200, "application/json", "[]");
            return;
        } else if (n == -1) {
            request->send(200, "application/json", "[]");
            return;
        }

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
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

        if (ssid.length() == 0) {
            request->send(400, "text/plain", "SSID cannot be empty");
            return;
        }

        if (!dhcp) {
            IPAddress tIp, tMask, tGw, tDns;
            if (!tIp.fromString(ip) || !tMask.fromString(mask) || !tGw.fromString(gw) || !tDns.fromString(dns)) {
                request->send(400, "text/plain", "Invalid Static IP configuration");
                return;
            }
        }

        saveWifiSettings(ssid, pass, dhcp, ip, mask, gw, dns);
        request->send(200, "text/plain", "OK");
        scheduleRestart(1500);
    });

    server.begin();
    WiFi.scanNetworks(true);
    Serial.printf("[AP] Access Point started: SSID 'IPMI-Config-AP', IP: %s\n", apIP.toString().c_str());
}

// ============================================================
// Консольный TCP-сервер
// ============================================================
void setupConsoleServer() {
    consoleServer.onClient([](void *arg, AsyncClient *client) {
        if (activeClient != nullptr) {
            client->write("Another session active!\r\n");
            client->close();
            return;
        }
        activeClient = client;
        telState = TEL_PAUSED;
        Serial.println("[CONSOLE] Client connected. Telemetry paused.");
        activeClient->write("\r\n=== IPC-R300 mini-IPMI Console Bridge ===\r\n");

        activeClient->onData([](void *arg, AsyncClient *c, void *data, size_t len) {
            Serial1.write((const uint8_t*)data, len);
        });
        activeClient->onDisconnect([](void *arg, AsyncClient *c) {
            if (activeClient == c) {
                activeClient = nullptr;
            }
            telState = TEL_IDLE;
            lastTelemetryPoll = millis();
            Serial.println("[CONSOLE] Client disconnected. Resuming telemetry.");
            delete c;
        });
    }, nullptr);

    consoleServer.begin();
    Serial.println("[CONSOLE] TCP Console server listening on port 2323");
}

void routeUartToNetwork() {
    if (!activeClient || !activeClient->connected()) return;
    size_t avail = Serial1.available();
    if (avail == 0) return;

    uint8_t buf[128];
    size_t len = (avail > sizeof(buf)) ? sizeof(buf) : avail;
    size_t readBytes = Serial1.readBytes(buf, len);
    activeClient->write((const char*)buf, readBytes);
}

// ============================================================
// Телеметрия и MQTT
// ============================================================
void processTelemetryLine(const char* line) {
    const char* p = strstr(line, "IPMI:");
    if (!p) return;
    p += 5;

    JsonDocument doc;
    if (deserializeJson(doc, p)) return;

    metrics.temp = doc["temp"] | 0.0f;
    metrics.load1 = doc["load"] | 0.0f;
    metrics.memFreeMb = doc["mfree"] | 0;
    metrics.memTotalMb = doc["mtotal"] | 0;
    metrics.sdaSmart = doc["sda"] | -1;
    metrics.sdbSmart = doc["sdb"] | -1;
    metrics.hostState = HOST_ONLINE;

    if (mqttClient.connected()) {
        char outBuf[384];
        serializeJson(doc, outBuf, sizeof(outBuf));
        mqttClient.publish("homeassistant/sensor/mini_ipmi/state", outBuf);
        mqttClient.publish("homeassistant/sensor/mini_ipmi/status", "online");
    }
}

void sendTelemetryQuery() {
    while (Serial1.available()) Serial1.read();
    uartBufferLen = 0;
    uartBuffer[0] = '\0';

    Serial1.print(
        "\nprintf 'IPMI:{\"temp\":%s,\"load\":%s,\"mfree\":%s,\"mtotal\":%s,\"sda\":%s,\"sdb\":%s}\\n' "
        "\"$(awk '{print $1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)\" "
        "\"$(awk '{print $1}' /proc/loadavg 2>/dev/null || echo 0)\" "
        "\"$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)\" "
        "\"$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)\" "
        "\"$([ -b /dev/sda ] && (smartctl -H /dev/sda 2>/dev/null | grep -iqE 'PASSED|OK' && echo 1 || echo 0) || echo -1)\" "
        "\"$([ -b /dev/sdb ] && (smartctl -H /dev/sdb 2>/dev/null | grep -iqE 'PASSED|OK' && echo 1 || echo 0) || echo -1)\"\n"
    );

    cmdSentTime = millis();
    telState = TEL_WAIT_RESP;
}

void updateTelemetryFSM() {
    if (activeClient != nullptr) return;
    unsigned long now = millis();

    switch (telState) {
        case TEL_IDLE:
            if (now - lastTelemetryPoll >= POLL_INTERVAL) sendTelemetryQuery();
            break;
        case TEL_WAIT_RESP:
            while (Serial1.available()) {
                char c = (char)Serial1.read();
                if (c == '\n' || c == '\r') {
                    if (uartBufferLen > 0) {
                        uartBuffer[uartBufferLen] = '\0';
                        
                        char* start = uartBuffer;
                        while (*start == ' ' || *start == '\t') start++;

                        if (strncmp(start, "IPMI:", 5) == 0) {
                            processTelemetryLine(start);
                            telState = TEL_IDLE;
                            lastTelemetryPoll = now;
                            uartBufferLen = 0;
                            uartBuffer[0] = '\0';
                            return;
                        }
                        uartBufferLen = 0;
                        uartBuffer[0] = '\0';
                    }
                } else if (uartBufferLen < sizeof(uartBuffer) - 1) {
                    uartBuffer[uartBufferLen++] = c;
                }
            }

            if (now - cmdSentTime >= CMD_TIMEOUT) {
                metrics.hostState = HOST_OFFLINE;
                telState = TEL_IDLE;
                lastTelemetryPoll = now;
                uartBufferLen = 0;
                uartBuffer[0] = '\0';
                if (mqttClient.connected()) mqttClient.publish("homeassistant/sensor/mini_ipmi/status", "offline");
            }
            break;
        default:
            break;
    }
}

void checkMqtt() {
    if (isApMode || WiFi.status() != WL_CONNECTED) return;
    if (!mqttClient.connected()) {
        static unsigned long lastMqtt = 0;
        if (millis() - lastMqtt > 5000) {
            lastMqtt = millis();
            String cid = "ESP32-IPMI-" + String(WiFi.macAddress());
            if (mqttClient.connect(cid.c_str(), appCfg.mqtt_user.c_str(), appCfg.mqtt_pass.c_str(), "homeassistant/sensor/mini_ipmi/status", 1, true, "offline")) {
                Serial.println("[MQTT] Connected!");
                mqttClient.publish("homeassistant/sensor/mini_ipmi/status", "online", true);
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

    if (t == "homeassistant/switch/mini_ipmi/power/set") {
        if (msg == "PRESS" || msg == "PULSE" || msg == "ON") {
            if (startPulse(PIN_PWR, 300)) {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "OK");
                mqttClient.publish(topic, "", true);
            } else {
                Serial.println("[MQTT] Power PRESS rejected: pulse busy");
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "BUSY");
            }
        } else if (msg == "FORCE_OFF") {
            if (startPulse(PIN_PWR, 5000)) {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "OK");
                mqttClient.publish(topic, "", true);
            } else {
                Serial.println("[MQTT] Power FORCE_OFF rejected: pulse busy");
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "BUSY");
            }
        }
    } else if (t == "homeassistant/switch/mini_ipmi/reset/set") {
        if (msg == "PRESS" || msg == "RESET" || msg == "ON") {
            if (startPulse(PIN_RESET, 300)) {
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "OK");
                mqttClient.publish(topic, "", true);
            } else {
                Serial.println("[MQTT] Reset rejected: pulse busy");
                mqttClient.publish("homeassistant/switch/mini_ipmi/result", "BUSY");
            }
        }
    }
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

    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart < 2000)) delay(10);

    Serial.println("\n[SYSTEM] ESP32-C3 mini-IPMI v4.6 starting...");
    bootTime = millis();

    WiFi.onEvent(WiFiEvent);
    WiFi.setAutoReconnect(true);

    loadSettings();

    if (appCfg.wifi_ssid.length() == 0) {
        Serial.println("[WIFI] No saved configuration.");
        startCaptivePortal();
        return;
    }

    Serial.printf("[WIFI] Connecting to '%s'...\n", appCfg.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);

    if (!appCfg.wifi_dhcp && appCfg.wifi_ip.length() > 0) {
        IPAddress localIP, gateway, subnet, dns;
        bool v1 = localIP.fromString(appCfg.wifi_ip);
        bool v2 = gateway.fromString(appCfg.wifi_gw);
        bool v3 = subnet.fromString(appCfg.wifi_mask);
        bool v4 = dns.fromString(appCfg.wifi_dns);

        if (v1 && v2 && v3 && v4) {
            if (WiFi.config(localIP, gateway, subnet, dns)) {
                Serial.printf("[WIFI] Applying Static IP: %s\n", appCfg.wifi_ip.c_str());
            } else {
                Serial.println("[WIFI] ERROR: WiFi.config() failed! Falling back to DHCP.");
            }
        } else {
            Serial.println("[WIFI] ERROR: Stored static IP configuration is invalid! Falling back to DHCP.");
        }
    }

    WiFi.begin(appCfg.wifi_ssid.c_str(), appCfg.wifi_pass.c_str());

    unsigned long wifiWait = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWait < 12000)) {
        checkAutoPower();
        updatePulse();
        delay(20);
        if (restartPending) updateRestart();
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Connection failed. Fallback to AP Mode.");
        startCaptivePortal();
        return;
    }

    Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(PIN_LED, HIGH);

    if (MDNS.begin("ipmi")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[MDNS] Responder started: http://ipmi.local");
    }

    mqttClient.setServer(appCfg.mqtt_host.c_str(), appCfg.mqtt_port);
    mqttClient.setCallback(mqttCallback);

    setupConsoleServer();

    // Маршруты рабочего сервера
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html; charset=utf-8", index_html);
    });

    server.on("/api/metrics", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        if (metrics.hostState == HOST_ONLINE) doc["status"] = "online";
        else if (metrics.hostState == HOST_OFFLINE) doc["status"] = "offline";
        else doc["status"] = "unknown";

        doc["temp"] = metrics.temp;
        doc["load"] = metrics.load1;
        doc["mfree"] = metrics.memFreeMb;
        doc["mtotal"] = metrics.memTotalMb;
        doc["sda"] = metrics.sdaSmart;
        doc["sdb"] = metrics.sdbSmart;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/pwr/pulse", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (pulseActive) { req->send(409, "text/plain", "Busy"); return; }
        startPulse(PIN_PWR, 300);
        req->send(200, "text/plain", "Power pulsed (300ms)");
    });

    server.on("/pwr/force", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (pulseActive) { req->send(409, "text/plain", "Busy"); return; }
        startPulse(PIN_PWR, 5000);
        req->send(200, "text/plain", "Hard power-off (5s)");
    });

    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (pulseActive) { req->send(409, "text/plain", "Busy"); return; }
        startPulse(PIN_RESET, 300);
        req->send(200, "text/plain", "Reset pulsed (300ms)");
    });

    server.on("/api/settings/mqtt", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["host"] = appCfg.mqtt_host;
        doc["port"] = appCfg.mqtt_port;
        doc["user"] = appCfg.mqtt_user;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/api/settings/mqtt", HTTP_POST, [](AsyncWebServerRequest *req) {
        String host = req->hasParam("mq_host", true) ? req->getParam("mq_host", true)->value() : "";
        String portStr = req->hasParam("mq_port", true) ? req->getParam("mq_port", true)->value() : "";
        String user = req->hasParam("mq_user", true) ? req->getParam("mq_user", true)->value() : "";
        String pass = req->hasParam("mq_pass", true) ? req->getParam("mq_pass", true)->value() : "";
        bool clearPass = req->hasParam("clear_pass", true) ? (req->getParam("clear_pass", true)->value() == "1") : false;

        host.trim();
        int port = portStr.toInt();

        if (host.length() == 0) {
            req->send(400, "text/plain", "Error: Broker IP/Host cannot be empty");
            return;
        }

        if (port < 1 || port > 65535) {
            req->send(400, "text/plain", "Error: Port must be in range 1-65535");
            return;
        }

        appCfg.mqtt_host = host;
        appCfg.mqtt_port = port;
        appCfg.mqtt_user = user;
        if (clearPass) appCfg.mqtt_pass = "";
        else if (pass.length() > 0) appCfg.mqtt_pass = pass;

        saveMqttSettings(appCfg.mqtt_host, appCfg.mqtt_port, appCfg.mqtt_user, appCfg.mqtt_pass, clearPass);

        mqttClient.disconnect();
        mqttClient.setServer(appCfg.mqtt_host.c_str(), appCfg.mqtt_port);
        req->send(200, "text/plain", "MQTT settings updated & applied!");
    });

    server.on("/api/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *req) {
        prefs.begin("ipmi_cfg", false);
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
        req->send(200, "text/plain", "Wi-Fi reset. Rebooting to AP Mode...");
        scheduleRestart(1000);
    });

    server.begin();
    Serial.println("[SERVER] Production Web Server started.");
}

// ============================================================
// Loop
// ============================================================
void loop() {
    updateLed();

    if (isApMode) {
        dnsServer.processNextRequest();
        updatePulse();
        checkAutoPower();
        updateRestart();
        delay(5);
        return;
    }

    updatePulse();
    updateTelemetryFSM();
    routeUartToNetwork();
    checkMqtt();
    checkAutoPower();
    updateRestart();

    delay(2);
}