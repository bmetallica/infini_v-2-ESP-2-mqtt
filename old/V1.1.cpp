
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// EEPROM-Konfiguration (umbenannt wegen Namenskonflikt)
struct AppConfig {
  char magic;
  char ssid[32];
  char password[32];
  char mqtt_server[32];
  char mqtt_topic[32];
  bool use_static_ip;
  uint32_t local_ip;
  uint32_t gateway;
  uint32_t subnet;
};

AppConfig appConfig; // Umbenannt von config
ESP8266WebServer webServer(80); // Umbenannt von server
bool inAPMode = false;

// WLAN Konfiguration
IPAddress local_IP;
IPAddress gateway;
IPAddress subnet;

// MQTT Konfiguration
WiFiClient espClient;
PubSubClient client(espClient);

// Serial Konfiguration
SoftwareSerial inverterSerial(5, 4); // RX: D1 (GPIO5), TX: D2 (GPIO4)

// InfiniSolar-Befehle
const char* befehle[] = {
  "^P005GS",  // Allgemeine Statusdaten
  "^P005PI",  // Produktinfo
  "^P004T",   // Zeit
  "^P006MOD", // Betriebsmodus
  "^P006FWS"  // Firmware & Warnungen
};

const int NUM_BEFEHLE = sizeof(befehle) / sizeof(befehle[0]);

// Timer-Variablen
unsigned long lastActionMillis = 0;
const long commandDelayMillis = 50;
const long responseTimeoutMillis = 750;
const long fullCycleDelayMillis = 500;
int currentCommandIndex = 0;

// Zustandsmaschine
enum AppState {
  STATE_IDLE_START_CYCLE,
  STATE_SEND_COMMAND,
  STATE_READ_RESPONSE,
  STATE_PROCESS_DATA,
  STATE_WAIT_BETWEEN_COMMANDS,
  STATE_WAIT_FOR_NEXT_CYCLE
};
AppState currentState = STATE_IDLE_START_CYCLE;

String currentResponseBuffer = "";
unsigned long responseStartTime = 0;

// Aktuelle Wechselrichterwerte für Dashboard
String currentValues[50];
const char* valueNames[] = {
  "netzspannung", "netzfrequenz", "ac_ausgangsspannung", "ac_ausgangsfrequenz",
  "ac_scheinleistung", "ac_wirkleistung", "ausgangslast", "batteriespannung",
  "batterieentladestrom", "batterieladestrom", "batteriekapazitaet", "temperatur_gehaeuse",
  "mppt1_temperatur", "mppt2_temperatur", "solarleistung1", "solarleistung2",
  "solarspannung1", "solarspannung2", "ladestatus1", "ladestatus2",
  "batteriestromrichtung", "wr_stromrichtung", "netzstromrichtung", "firmware",
  "stunden", "minuten", "sekunden", "mode", "fehler", "warnungen"
};
const int NUM_VALUES = sizeof(valueNames) / sizeof(valueNames[0]);

// HTML-Seiten (identisch wie vorher)
const char dashboardHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>InfiniSolar Dashboard</title>
  <style>
    :root {
      --primary: #2c3e50;
      --secondary: #3498db;
      --light: #ecf0f1;
      --dark: #34495e;
      --success: #2ecc71;
      --danger: #e74c3c;
    }
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background-color: #f5f7fa;
      color: #333;
      line-height: 1.6;
    }
    .container {
      max-width: 1200px;
      margin: 0 auto;
      padding: 20px;
    }
    header {
      background: linear-gradient(135deg, var(--primary), var(--dark));
      color: white;
      padding: 1rem 0;
      box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
      border-radius: 0 0 10px 10px;
      margin-bottom: 30px;
    }
    .header-content {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0 20px;
    }
    h1 {
      font-size: 1.8rem;
      font-weight: 500;
    }
    .status {
      display: flex;
      align-items: center;
    }
    .status-indicator {
      width: 15px;
      height: 15px;
      border-radius: 50%;
      margin-right: 10px;
    }
    .connected {
      background-color: var(--success);
      box-shadow: 0 0 10px var(--success);
    }
    .disconnected {
      background-color: var(--danger);
    }
    .btn {
      display: inline-block;
      background: var(--secondary);
      color: white;
      padding: 10px 20px;
      border: none;
      border-radius: 5px;
      cursor: pointer;
      text-decoration: none;
      font-size: 1rem;
      transition: all 0.3s ease;
      box-shadow: 0 2px 5px rgba(0, 0, 0, 0.2);
    }
    .btn:hover {
      background: #2980b9;
      transform: translateY(-2px);
      box-shadow: 0 4px 8px rgba(0, 0, 0, 0.3);
    }
    .card {
      background: white;
      border-radius: 10px;
      box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
      padding: 25px;
      margin-bottom: 30px;
    }
    h2 {
      color: var(--primary);
      margin-bottom: 20px;
      padding-bottom: 10px;
      border-bottom: 2px solid var(--light);
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin: 20px 0;
    }
    th, td {
      padding: 12px 15px;
      text-align: left;
      border-bottom: 1px solid #ddd;
    }
    th {
      background-color: var(--light);
      color: var(--dark);
      font-weight: 600;
    }
    tr:hover {
      background-color: rgba(52, 152, 219, 0.05);
    }
    .action-buttons {
      display: flex;
      gap: 15px;
      margin-top: 20px;
    }
    .btn-restart {
      background: var(--danger);
    }
    .btn-restart:hover {
      background: #c0392b;
    }
    footer {
      text-align: center;
      margin-top: 40px;
      color: #7f8c8d;
      font-size: 0.9rem;
    }
  </style>
</head>
<body>
  <header>
    <div class="header-content">
      <div class="status">
        <div id="status-indicator" class="status-indicator disconnected"></div>
        <h1>InfiniSolar Monitor %MQTT_TOPIC% </h1>
      </div>
      <a href="/config" class="btn">Konfiguration</a>
    </div>
  </header>
  
  <div class="container">
    <div class="card">
      <h2>Systemwerte</h2>
      <div class="table-container">
        <table id="values-table">
          <thead>
            <tr>
              <th>Parameter</th>
              <th>Wert</th>
            </tr>
          </thead>
          <tbody>
            <!-- Werte werden per JavaScript eingefügt -->
          </tbody>
        </table>
      </div>
      
      <div class="action-buttons">
        <button class="btn btn-restart" onclick="restartESP()">ESP Neustarten</button>
      </div>
    </div>
  </div>
  
  <footer>
    <p>InfiniSolar Monitoring System &copy; 2025 by bmetallica</p>
  </footer>

  <script>
    function updateValues() {
      fetch('/values')
        .then(response => response.json())
        .then(data => {
          // Update status indicator
          const indicator = document.getElementById('status-indicator');
          indicator.className = data.connected ? 
            'status-indicator connected' : 'status-indicator disconnected';
          
          // Update table
          const tbody = document.querySelector('#values-table tbody');
          tbody.innerHTML = '';
          
          for (const [key, value] of Object.entries(data)) {
            if (key !== 'connected') {
              const row = document.createElement('tr');
              
              const nameCell = document.createElement('td');
              nameCell.textContent = key;
              row.appendChild(nameCell);
              
              const valueCell = document.createElement('td');
              valueCell.textContent = value || 'N/A';
              row.appendChild(valueCell);
              
              tbody.appendChild(row);
            }
          }
        })
        .catch(error => {
          console.error('Fehler beim Abrufen der Daten:', error);
        });
    }
    
    function restartESP() {
      if (confirm('ESP wirklich neu starten?')) {
        fetch('/restart')
          .then(() => {
            alert('ESP wird neu gestartet...');
          });
      }
    }
    
    // Initial update and set interval
    updateValues();
    setInterval(updateValues, 2000);
  </script>
</body>
</html>
)rawliteral";

const char configHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>InfiniSolar Konfiguration</title>
  <style>
    :root {
      --primary: #2c3e50;
      --secondary: #3498db;
      --light: #ecf0f1;
      --dark: #34495e;
      --success: #2ecc71;
      --warning: #f39c12;
    }
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background-color: #f5f7fa;
      color: #333;
      line-height: 1.6;
    }
    .container {
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
    }
    header {
      background: linear-gradient(135deg, var(--primary), var(--dark));
      color: white;
      padding: 1rem 0;
      box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
      border-radius: 0 0 10px 10px;
      margin-bottom: 30px;
    }
    .header-content {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0 20px;
    }
    h1 {
      font-size: 1.8rem;
      font-weight: 500;
    }
    .btn {
      display: inline-block;
      background: var(--secondary);
      color: white;
      padding: 10px 20px;
      border: none;
      border-radius: 5px;
      cursor: pointer;
      text-decoration: none;
      font-size: 1rem;
      transition: all 0.3s ease;
      box-shadow: 0 2px 5px rgba(0, 0, 0, 0.2);
    }
    .btn:hover {
      background: #2980b9;
      transform: translateY(-2px);
      box-shadow: 0 4px 8px rgba(0, 0, 0, 0.3);
    }
    .card {
      background: white;
      border-radius: 10px;
      box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
      padding: 25px;
      margin-bottom: 30px;
    }
    h2 {
      color: var(--primary);
      margin-bottom: 20px;
      padding-bottom: 10px;
      border-bottom: 2px solid var(--light);
    }
    .form-group {
      margin-bottom: 20px;
    }
    label {
      display: block;
      margin-bottom: 8px;
      font-weight: 500;
      color: var(--dark);
    }
    input[type="text"],
    input[type="password"],
    input[type="number"] {
      width: 100%;
      padding: 12px;
      border: 1px solid #ddd;
      border-radius: 5px;
      font-size: 1rem;
      transition: border-color 0.3s;
    }
    input:focus {
      border-color: var(--secondary);
      outline: none;
      box-shadow: 0 0 0 2px rgba(52, 152, 219, 0.2);
    }
    .checkbox-group {
      display: flex;
      align-items: center;
      margin-bottom: 15px;
    }
    .checkbox-group input {
      margin-right: 10px;
    }
    .ip-group {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 15px;
    }
    .action-buttons {
      display: flex;
      gap: 15px;
      margin-top: 30px;
    }
    .btn-save {
      background: var(--success);
    }
    .btn-save:hover {
      background: #27ae60;
    }
    .btn-cancel {
      background: var(--warning);
    }
    .btn-cancel:hover {
      background: #e67e22;
    }
    footer {
      text-align: center;
      margin-top: 40px;
      color: #7f8c8d;
      font-size: 0.9rem;
    }
  </style>
</head>
<body>
  <header>
    <div class="header-content">
      <h1>Systemkonfiguration</h1>
      <a href="/" class="btn">Dashboard</a>
    </div>
  </header>
  
  <div class="container">
    <div class="card">
      <h2>Netzwerkkonfiguration</h2>
      <form id="config-form" action="/save" method="post">
        <div class="form-group">
          <label for="ssid">WLAN SSID</label>
          <input type="text" id="ssid" name="ssid" value="%SSID%" required>
        </div>
        
        <div class="form-group">
          <label for="password">WLAN Passwort</label>
          <input type="password" id="password" name="password" value="%PASSWORD%">
        </div>
        
        <div class="form-group">
          <label for="mqtt_server">MQTT Server</label>
          <input type="text" id="mqtt_server" name="mqtt_server" value="%MQTT_SERVER%" required>
        </div>
        
        <div class="form-group">
          <label for="mqtt_topic">MQTT Topic</label>
          <input type="text" id="mqtt_topic" name="mqtt_topic" value="%MQTT_TOPIC%" required>
        </div>
        
        <div class="checkbox-group">
          <input type="checkbox" id="use_static_ip" name="use_static_ip" %STATIC_IP_CHECKED%>
          <label for="use_static_ip">Statische IP verwenden</label>
        </div>
        
        <div class="ip-group">
          <div class="form-group">
            <label for="ip">IP-Adresse</label>
            <input type="text" id="ip" name="ip" value="%IP%" pattern="\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}">
          </div>
          
          <div class="form-group">
            <label for="gateway">Gateway</label>
            <input type="text" id="gateway" name="gateway" value="%GATEWAY%" pattern="\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}">
          </div>
          
          <div class="form-group">
            <label for="subnet">Subnetzmaske</label>
            <input type="text" id="subnet" name="subnet" value="%SUBNET%" pattern="\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}">
          </div>
        </div>
        
        <div class="action-buttons">
          <button type="submit" class="btn btn-save">Speichern & Neustarten</button>
          <a href="/" class="btn btn-cancel">Abbrechen</a>
        </div>
      </form>
    </div>
  </div>
  
  <footer>
    <p>InfiniSolar Monitoring System &copy; 2025 by bmetallica</p>
  </footer>
  
  <script>
    // Aktiviert IP-Felder basierend auf Checkbox
    const staticIpCheckbox = document.getElementById('use_static_ip');
    const ipFields = document.querySelectorAll('.ip-group input');
    
    function updateIpFields() {
      ipFields.forEach(field => {
        field.disabled = !staticIpCheckbox.checked;
      });
    }
    
    staticIpCheckbox.addEventListener('change', updateIpFields);
    document.addEventListener('DOMContentLoaded', updateIpFields);
  </script>
</body>
</html>
)rawliteral";

// Vorwärtsdeklarationen
void loadConfig();
void saveConfig();
void startAPMode();
void connectToWiFi();
void setupWebServer();
void handleDashboard();
void handleConfigPage();
void handleValues();
void handleSaveConfig();
void handleRestart();
void reconnectMQTT();
String extractPayload(const String& raw);
void publishValue(const String& field, const String& value);
void parseAndSendGS(String payload);
void parseAndSendPI(String payload);
void parseAndSendT(String payload);
void parseAndSendMOD(String payload);
void parseAndSendFWS(String payload);

// CRC16/XMODEM Berechnung
uint16_t crc16_xmodem(const uint8_t *data, uint16_t length) {
  uint16_t crc = 0;
  while (length--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
  }
  return crc;
}

void setup() {
  Serial.begin(115200);
  inverterSerial.begin(2400);
  EEPROM.begin(512);
  
  loadConfig();
  
  if (strlen(appConfig.ssid) > 0) {
    connectToWiFi();
  } else {
    startAPMode();
  }

  setupWebServer();
  client.setServer(appConfig.mqtt_server, 1883);
  lastActionMillis = millis();
}

void loop() {
  webServer.handleClient();
  
  if (inAPMode) {
    return;
  }
  
  if (!client.connected() && WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
  }
  client.loop();

  unsigned long currentMillis = millis();

  switch (currentState) {
    case STATE_IDLE_START_CYCLE:
      if (currentMillis - lastActionMillis >= fullCycleDelayMillis || currentCommandIndex == 0) {
        currentCommandIndex = 0;
        currentState = STATE_SEND_COMMAND;
        lastActionMillis = currentMillis;
      }
      break;

    case STATE_SEND_COMMAND:
      if (currentCommandIndex < NUM_BEFEHLE) {
        const char* cmd = befehle[currentCommandIndex];
        uint16_t crc = crc16_xmodem((uint8_t*)cmd, strlen(cmd));
        inverterSerial.print(cmd);
        inverterSerial.write((crc >> 8) & 0xFF);
        inverterSerial.write(crc & 0xFF);
        inverterSerial.write('\r');

        currentResponseBuffer = "";
        responseStartTime = currentMillis;
        currentState = STATE_READ_RESPONSE;
        lastActionMillis = currentMillis;
      } else {
        currentState = STATE_WAIT_FOR_NEXT_CYCLE;
        lastActionMillis = currentMillis;
      }
      break;

    case STATE_READ_RESPONSE:
      if (inverterSerial.available()) {
        char c = inverterSerial.read();
        currentResponseBuffer += c;
        if (c == '\r') {
          currentState = STATE_PROCESS_DATA;
          lastActionMillis = currentMillis;
        }
      } else if (currentMillis - responseStartTime > responseTimeoutMillis) {
        currentResponseBuffer = "";
        currentState = STATE_WAIT_BETWEEN_COMMANDS;
        lastActionMillis = currentMillis;
      }
      break;

    case STATE_PROCESS_DATA:
      if (currentResponseBuffer.length() > 0) {
        String payload = extractPayload(currentResponseBuffer);
        if (payload.length() > 0) {
          const char* currentCmd = befehle[currentCommandIndex];
          if (String(currentCmd).endsWith("GS")) parseAndSendGS(payload);
          else if (String(currentCmd).endsWith("PI")) parseAndSendPI(payload);
          else if (String(currentCmd).endsWith("T")) parseAndSendT(payload);
          else if (String(currentCmd).endsWith("MOD")) parseAndSendMOD(payload);
          else if (String(currentCmd).endsWith("FWS")) parseAndSendFWS(payload);
        }
      }
      currentState = STATE_WAIT_BETWEEN_COMMANDS;
      lastActionMillis = currentMillis;
      break;

    case STATE_WAIT_BETWEEN_COMMANDS:
      if (currentMillis - lastActionMillis >= commandDelayMillis) {
          currentCommandIndex++;
          currentState = STATE_SEND_COMMAND;
          lastActionMillis = currentMillis;
      }
      break;

    case STATE_WAIT_FOR_NEXT_CYCLE:
      if (currentMillis - lastActionMillis >= fullCycleDelayMillis) {
        currentState = STATE_IDLE_START_CYCLE;
      }
      break;
  }
}

void loadConfig() {
  EEPROM.get(0, appConfig);
  
  if (appConfig.magic != 0xAE) {
    memset(&appConfig, 0, sizeof(appConfig));
    appConfig.magic = 0xAE;
    strcpy(appConfig.mqtt_server, "192.168.3.3");
    strcpy(appConfig.mqtt_topic, "infini");
  }
}

void saveConfig() {
  EEPROM.put(0, appConfig);
  EEPROM.commit();
}

void startAPMode() {
  inAPMode = true;
  Serial.println("Starte AP-Modus...");
  WiFi.softAP("InfiniConfig", "");
  Serial.println("AP gestartet: InfiniConfig");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
}

void connectToWiFi() {
  Serial.print("Verbinde mit WLAN: ");
  Serial.println(appConfig.ssid);
  
  if (appConfig.use_static_ip) {
    local_IP = appConfig.local_ip;
    gateway = appConfig.gateway;
    subnet = appConfig.subnet;
    WiFi.config(local_IP, gateway, subnet);
  }
  
  WiFi.begin(appConfig.ssid, appConfig.password);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nVerbunden! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nKonnte nicht verbinden! Starte AP-Modus...");
    startAPMode();
  }
}

void setupWebServer() {
  webServer.on("/", handleDashboard);
  webServer.on("/config", handleConfigPage);
  webServer.on("/values", handleValues);
  webServer.on("/save", HTTP_POST, handleSaveConfig);
  webServer.on("/restart", handleRestart);
  webServer.begin();
  Serial.println("Webserver gestartet");
}

void handleDashboard() {
  String html = FPSTR(dashboardHTML);
  html.replace("%MQTT_TOPIC%", appConfig.mqtt_topic);
  webServer.send(200, "text/html", html);
}

void handleConfigPage() {
  String html = FPSTR(configHTML);
  
  html.replace("%SSID%", appConfig.ssid);
  html.replace("%PASSWORD%", appConfig.password);
  html.replace("%MQTT_SERVER%", appConfig.mqtt_server);
  html.replace("%MQTT_TOPIC%", appConfig.mqtt_topic);
  html.replace("%STATIC_IP_CHECKED%", appConfig.use_static_ip ? "checked" : "");
  
  if (appConfig.use_static_ip) {
    html.replace("%IP%", IPAddress(appConfig.local_ip).toString());
    html.replace("%GATEWAY%", IPAddress(appConfig.gateway).toString());
    html.replace("%SUBNET%", IPAddress(appConfig.subnet).toString());
  } else {
    html.replace("%IP%", "");
    html.replace("%GATEWAY%", "");
    html.replace("%SUBNET%", "");
  }
  
  webServer.send(200, "text/html", html);
}

void handleValues() {
  String json = "{";
  json += "\"connected\":";
  json += (currentResponseBuffer.length() > 0) ? "true" : "false";
  json += ",";
  
  for (int i = 0; i < NUM_VALUES; i++) {
    json += "\"" + String(valueNames[i]) + "\":";
    json += "\"" + currentValues[i] + "\"";
    if (i < NUM_VALUES - 1) json += ",";
  }
  
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleSaveConfig() {
  strncpy(appConfig.ssid, webServer.arg("ssid").c_str(), sizeof(appConfig.ssid));
  strncpy(appConfig.password, webServer.arg("password").c_str(), sizeof(appConfig.password));
  strncpy(appConfig.mqtt_server, webServer.arg("mqtt_server").c_str(), sizeof(appConfig.mqtt_server));
  strncpy(appConfig.mqtt_topic, webServer.arg("mqtt_topic").c_str(), sizeof(appConfig.mqtt_topic));
  
  appConfig.use_static_ip = webServer.hasArg("use_static_ip");
  
  if (appConfig.use_static_ip) {
    IPAddress ip, gw, sn;
    ip.fromString(webServer.arg("ip"));
    gw.fromString(webServer.arg("gateway"));
    sn.fromString(webServer.arg("subnet"));
    
    appConfig.local_ip = ip;
    appConfig.gateway = gw;
    appConfig.subnet = sn;
  }
  
  saveConfig();
  webServer.send(200, "text/plain", "Konfiguration gespeichert. Starte neu...");
  delay(1000);
  ESP.restart();
}

void handleRestart() {
  webServer.send(200, "text/plain", "ESP startet neu...");
  delay(1000);
  ESP.restart();
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Verbinde MQTT...");
    if (client.connect("InfiniClient")) {
      Serial.println("verbunden!");
    } else {
      Serial.print("Fehler: ");
      Serial.print(client.state());
      Serial.println(" - Neuer Versuch in 2s");
      delay(2000);
    }
  }
}

String extractPayload(const String& raw) {
  int start = raw.indexOf("^D");
  if (start != -1 && raw.length() >= static_cast<unsigned int>(start + 5)) {
    String lenStr = raw.substring(start + 2, start + 5);
    unsigned int len = lenStr.toInt();
    
    if (len > 0 && (start + 5 + static_cast<int>(len) - 1) < static_cast<int>(raw.length())) {
      return raw.substring(start + 5, start + 5 + len);
    } else if (len > 0 && (start + 5 + static_cast<int>(len) -1) == static_cast<int>(raw.length()) -1 && raw.endsWith("\r")) {
      return raw.substring(start + 5, start + 5 + len -1);
    }
  }
  return "";
}

void publishValue(const String& field, const String& value) {
  String topic = String(appConfig.mqtt_topic) + "/" + field;
  client.publish(topic.c_str(), value.c_str());
  
  for (int i = 0; i < NUM_VALUES; i++) {
    if (field == valueNames[i]) {
      currentValues[i] = value;
      break;
    }
  }
}

void parseAndSendGS(String payload) {
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  String felder[30];
  int index = 0, last = 0;

  for (unsigned int i = 0; i <= payload.length(); i++) {
    if (i == payload.length() || payload[i] == ',') {
      felder[index++] = payload.substring(last, i);
      last = i + 1;
    }
  }

  if (index < 26) return;

  const char* fieldNames[] = {
    "netzspannung", "netzfrequenz", "ac_ausgangsspannung", "ac_ausgangsfrequenz",
    "ac_scheinleistung", "ac_wirkleistung", "ausgangslast", "batteriespannung",
    "", "", 
    "batterieentladestrom", "batterieladestrom", "batteriekapazitaet", "temperatur_gehaeuse",
    "mppt1_temperatur", "mppt2_temperatur", "solarleistung1", "solarleistung2",
    "solarspannung1", "solarspannung2", "",
    "ladestatus1", "ladestatus2", "batteriestromrichtung", "wr_stromrichtung", "netzstromrichtung"
  };
  
  const int relevantFields[] = {0,1,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,19,21,22,23,24,25};
  const int numFields = sizeof(relevantFields) / sizeof(relevantFields[0]);
  
  for (int i = 0; i < numFields; i++) {
    int idx = relevantFields[i];
    if (strlen(fieldNames[idx]) > 0) {
      String value;
      if (idx == 0 || idx == 1 || idx == 2 || idx == 3 || idx == 7 || idx == 10 || idx == 18 || idx == 19) {
        value = String(felder[idx].toFloat() / 10.0, 1);
      } else {
        value = felder[idx];
      }
      publishValue(fieldNames[idx], value);
    }
  }
}

void parseAndSendPI(String payload) {
  String cleanf = "";
  for (unsigned int i = 0; i < payload.length(); i++) {
    char c = payload[i];
    if (isPrintable(c)) cleanf += c;
  }
  publishValue("firmware", cleanf);
}

void parseAndSendT(String payload) {
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  String clean = "";
  for (unsigned int i = 0; i < payload.length(); i++) {
    char c = payload[i];
    if (isDigit(c)) clean += c;
  }

  if (clean.length() >= 14) {
    publishValue("stunden", clean.substring(8, 10));
    publishValue("minuten", clean.substring(10, 12));
    publishValue("sekunden", clean.substring(12, 14));
  }
}

void parseAndSendMOD(String payload) {
  String clean = "";
  for (unsigned int i = 0; i < payload.length(); i++) {
    char c = payload[i];
    if (isPrintable(c)) clean += c;
  }
  publishValue("mode", clean);
}

void parseAndSendFWS(String payload) {
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  int kpos = payload.indexOf("k:");
  if (kpos != -1) {
    payload = payload.substring(0, kpos);
  }

  String teile[20];
  int index = 0, last = 0;
  for (int i = 0; i < static_cast<int>(payload.length()); i++) {
    if (payload[i] == ',') {
      teile[index++] = payload.substring(last, i);
      last = i + 1;
      if (index >= 20) break;
    }
  }
  if (index < 20) teile[index++] = payload.substring(last);

  String fehler = (index > 0) ? teile[0] : "N/A";
  publishValue("fehler", fehler);

  int warnIndizes[15];
  int warnCount = 0;
  for (int i = 1; i < index && i <= 15; i++) {
    if (teile[i].toInt() == 1) {
      warnIndizes[warnCount++] = i;
    }
  }

  String warnung = "0";
  if (warnCount == 1) {
    warnung = String(warnIndizes[0]);
  } else if (warnCount > 1) {
    int randIndex = random(warnCount);
    warnung = String(warnIndizes[randIndex]);
  }

  publishValue("warnungen", warnung);
}
