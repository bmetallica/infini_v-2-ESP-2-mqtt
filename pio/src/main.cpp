#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// EEPROM-Konfiguration
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
  // Neue Variablen für Delays
  uint32_t commandDelay;
  uint32_t responseTimeout;
  uint32_t fullCycleDelay;
  uint32_t mqttPublishDelay; // NEU: Verzögerung vor jedem MQTT-Publishing im GS-Befehl
};

AppConfig appConfig;
ESP8266WebServer webServer(80);
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

// Timer-Variablen - diese werden nun aus appConfig gelesen
unsigned long lastActionMillis = 0;
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
  "stunden", "minuten", "sekunden", "Modus", "Fehler_r", "Warnungen_r"
};
const int NUM_VALUES = sizeof(valueNames) / sizeof(valueNames[0]);

// HTML-Seiten
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

        <h2>Timing Einstellungen (Millisekunden)</h2>
        <div class="form-group">
          <label for="command_delay">Befehlsverzögerung (zwischen Befehlen)</label>
          <input type="number" id="command_delay" name="command_delay" value="%COMMAND_DELAY%" min="0" required>
        </div>
        <div class="form-group">
          <label for="response_timeout">Antwort-Timeout</label>
          <input type="number" id="response_timeout" name="response_timeout" value="%RESPONSE_TIMEOUT%" min="0" required>
        </div>
        <div class="form-group">
          <label for="full_cycle_delay">Voller Zyklus Verzögerung (nach letztem Befehl)</label>
          <input type="number" id="full_cycle_delay" name="full_cycle_delay" value="%FULL_CYCLE_DELAY%" min="0" required>
        </div>
        <div class="form-group">
          <label for="mqtt_publish_delay">MQTT Veröffentlichungs-Verzögerung (pro Wert nach GS-Befehl)</label>
          <input type="number" id="mqtt_publish_delay" name="mqtt_publish_delay" value="%MQTT_PUBLISH_DELAY%" min="0" required>
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
  EEPROM.begin(512); // Stellen Sie sicher, dass die Größe für alle Daten ausreicht
  
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
      if (currentMillis - lastActionMillis >= appConfig.fullCycleDelay || currentCommandIndex == 0) {
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
      } else if (currentMillis - responseStartTime > appConfig.responseTimeout) {
        currentResponseBuffer = ""; // Timeout, Buffer leeren
        currentState = STATE_WAIT_BETWEEN_COMMANDS; // Zum nächsten Befehl übergehen
        lastActionMillis = currentMillis;
      }
      break;

    case STATE_PROCESS_DATA:
      if (currentResponseBuffer.length() > 0) {
        String payload = extractPayload(currentResponseBuffer);
        if (payload.length() > 0) {
          const char* currentCmd = befehle[currentCommandIndex];
          if (String(currentCmd).endsWith("GS")) {
            // Der delay(appConfig.mqttPublishDelay) wurde von hier
            // in die parseAndSendGS() Funktion verschoben,
            // um pro einzelnem publishValue() zu verzögern.
            parseAndSendGS(payload);
          }
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
      if (currentMillis - lastActionMillis >= appConfig.commandDelay) {
          currentCommandIndex++;
          currentState = STATE_SEND_COMMAND;
          lastActionMillis = currentMillis;
      }
      break;

    case STATE_WAIT_FOR_NEXT_CYCLE:
      if (currentMillis - lastActionMillis >= appConfig.fullCycleDelay) {
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
    // Standardwerte für neue Variablen
    appConfig.commandDelay = 55;
    appConfig.responseTimeout = 850;
    appConfig.fullCycleDelay = 250;
    appConfig.mqttPublishDelay = 100; // NEUER Standardwert
    saveConfig(); // Speichern der initialen Konfiguration
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

  // Ersetzen der Delay-Variablen
  html.replace("%COMMAND_DELAY%", String(appConfig.commandDelay));
  html.replace("%RESPONSE_TIMEOUT%", String(appConfig.responseTimeout));
  html.replace("%FULL_CYCLE_DELAY%", String(appConfig.fullCycleDelay));
  html.replace("%MQTT_PUBLISH_DELAY%", String(appConfig.mqttPublishDelay)); // NEU
  
  webServer.send(200, "text/html", html);
}

void handleValues() {
  String json = "{";
  json += "\"connected\":";
  json += (currentResponseBuffer.length() > 0) ? "true" : "false"; // Basierend auf dem Bufferinhalt
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
  strncpy(appConfig.ssid, webServer.arg("ssid").c_str(), sizeof(appConfig.ssid) - 1);
  appConfig.ssid[sizeof(appConfig.ssid) - 1] = '\0';
  strncpy(appConfig.password, webServer.arg("password").c_str(), sizeof(appConfig.password) - 1);
  appConfig.password[sizeof(appConfig.password) - 1] = '\0';
  strncpy(appConfig.mqtt_server, webServer.arg("mqtt_server").c_str(), sizeof(appConfig.mqtt_server) - 1);
  appConfig.mqtt_server[sizeof(appConfig.mqtt_server) - 1] = '\0';
  strncpy(appConfig.mqtt_topic, webServer.arg("mqtt_topic").c_str(), sizeof(appConfig.mqtt_topic) - 1);
  appConfig.mqtt_topic[sizeof(appConfig.mqtt_topic) - 1] = '\0';
  
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

  // Speichern der Delay-Variablen
  appConfig.commandDelay = webServer.arg("command_delay").toInt();
  appConfig.responseTimeout = webServer.arg("response_timeout").toInt();
  appConfig.fullCycleDelay = webServer.arg("full_cycle_delay").toInt();
  appConfig.mqttPublishDelay = webServer.arg("mqtt_publish_delay").toInt(); // NEU
  
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
    
    // Überprüfen, ob die extrahierte Länge gültig ist und der Puffer groß genug für Payload + CRC + CR ist.
    // Gemäß "Infini-Solar V protocol 20170926.pdf" ist "Data length" die Länge inkl. CRC und Endzeichen, außer bei "^Tnnn".
    // Also ist die empfangene 'len' die Länge der Payload + 2 (CRC) + 1 (CR).
    // Wir wollen nur die reine Payload.
    
    // Die Rohdaten enden mit '\r'. Die angegebene Länge 'len' beinhaltet oft auch das CR und CRC.
    // Normalerweise ist die Payload (len - 3) für P-Befehle (da len CRC und CR enthält).
    // Wenn die Antwort *nicht* mit '\r' endet (z.B. bei einem verkürzten Timeout),
    // müssen wir das berücksichtigen und nur das extrahieren, was da ist.

    // Wenn die Rohdaten mit '\r' enden und die Länge stimmt (Payload + 2 CRC + 1 CR)
    if (raw.endsWith("\r") && (start + 5 + len) == raw.length()) { // len ist die Gesamtlänge inkl. CRC und CR
        return raw.substring(start + 5, start + 5 + len - 3); // Extrahiere Payload ohne CRC (2) und CR (1)
    }
    // Wenn die Rohdaten nicht mit '\r' enden oder die Länge nicht ganz stimmt (z.B. Timeout)
    // Versuche, so viel wie möglich als Payload zu interpretieren, ohne CRC
    else if ((start + 5 + len - 3) <= raw.length()) { // Payload (len - 3) sollte im Puffer sein
         return raw.substring(start + 5, start + 5 + len - 3);
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
    "", "", // Leere Einträge für fehlende Felder in Ihrem ursprünglichen Code
    "batterieentladestrom", "batterieladestrom", "batteriekapazitaet", "temperatur_gehaeuse",
    "mppt1_temperatur", "mppt2_temperatur", "solarleistung1", "solarleistung2",
    "solarspannung1", "solarspannung2", "", // Leerer Eintrag
    "ladestatus1", "ladestatus2", "batteriestromrichtung", "wr_stromrichtung", "netzstromrichtung"
  };
  
  // Die Indizes im 'felder' Array, die den 'fieldNames' entsprechen
  // Beispiel: fieldNames[0] "netzspannung" kommt von felder[0]
  // fieldNames[10] "batterieentladestrom" kommt von felder[8] (siehe RS232 communication Protocol for InfiniSolar 3K.pdf, QPIGS response)
  const int fieldMapIndices[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};

  // Liste der Indizes aus `fieldNames` die tatsächlich Werte haben
  const int publishIndices[] = {0,1,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,19,21,22,23,24,25};
  const int numPublishFields = sizeof(publishIndices) / sizeof(publishIndices[0]);

  for (int i = 0; i < numPublishFields; i++) {
    int fieldNameIdx = publishIndices[i]; // Index im `fieldNames` array
    int felderIdx = fieldMapIndices[fieldNameIdx]; // Entsprechender Index im `felder` array

    if (strlen(fieldNames[fieldNameIdx]) > 0) { // Nur wenn ein Name definiert ist
      String value;
      // Spezielle Skalierungen oder Formatierungen
      if (fieldNameIdx == 0 || fieldNameIdx == 1 || fieldNameIdx == 2 || fieldNameIdx == 3 || fieldNameIdx == 7 || 
          fieldNameIdx == 18 || fieldNameIdx == 19) { // AC In/Out, Batt-Spannung, Solar-Spannung
        value = String(felder[felderIdx].toFloat() / 10.0, 1);
      } else if (fieldNameIdx == 8 || fieldNameIdx == 9) { // Entlade-/Ladestrom
        value = String(felder[felderIdx].toFloat() * 10.0, 1);
      } else {
        value = felder[felderIdx];
      }
      
      // NEU: Delay vor JEDEM publishValue() Aufruf innerhalb von parseAndSendGS()
      if (appConfig.mqttPublishDelay > 0) {
        delay(appConfig.mqttPublishDelay);
      }
      publishValue(fieldNames[fieldNameIdx], value);
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
    if (isdigit(c)) clean += c;   // Nur Ziffern übernehmen
  }
  publishValue("mode", clean);
// Bestimme den lesbaren Modus-String
  String Modus;
  if (clean == "00") Modus = "Power On Mode";
  else if (clean == "01") Modus = "Standby Mode";
  else if (clean == "02") Modus = "Line Mode";
  else if (clean == "03") Modus = "Battery Mode";
  else if (clean == "04") Modus = "Fault Mode";
  else if (clean == "05") Modus = "Hybrid Mode";
  else Modus = "Unbekannter Modus"; // Für unbekannte Codes
 publishValue("Modus", Modus);
}

void parseAndSendFWS(String payload) {
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  int kpos = payload.indexOf("k:"); // "k:" ist ein Terminator, alles danach ist irrelevant
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
  if (index < 20) teile[index++] = payload.substring(last); // Letztes Segment hinzufügen

  String fehler = (index > 0) ? teile[0] : "N/A";
  publishValue("fehler", fehler);

  String Fehler_r;
  if (fehler == "00") {
    Fehler_r = "Kein Fehler";
  } else {
    String description;
    if (fehler == "01") description = "Fan locked";
    else if (fehler == "02") description = "Over temperature";
    else if (fehler == "03") description = "Battery voltage is too high";
    else if (fehler == "04") description = "Battery voltage is too low";
    else if (fehler == "05") description = "Output short circuited";
    else if (fehler == "06") description = "Output voltage abnormal";
    else if (fehler == "07") description = "Over load time out";
    else if (fehler == "08") description = "Bus voltage is too high";
    else if (fehler == "09") description = "Bus soft start failed";
    else if (fehler == "10") description = "PV current over";
    else if (fehler == "11") description = "PV voltage over";
    else if (fehler == "12") description = "Charge current over";
    else if (fehler == "51") description = "Over current or surge";
    else if (fehler == "52") description = "Bus voltage is too low";
    else if (fehler == "53") description = "Inverter soft start failed";
    else if (fehler == "55") description = "Over DC offset in AC output";
    else if (fehler == "56") description = "Battery disconnected";
    else if (fehler == "57") description = "Current sensor failed";
    else if (fehler == "58") description = "Output voltage is too low";
    else description = "Unbekannt";

    Fehler_r = description + " (Code: " + fehler + ")";
  }
  publishValue("Fehler_r", Fehler_r); // Lesbarer Fehler für Dashboard

  int warnIndizes[15];
  int warnCount = 0;
  for (int i = 1; i < index && i <= 15; i++) { // Warnungen beginnen ab Index 1
    if (teile[i].toInt() == 1) { // Wenn die Warnung aktiv ist (Wert 1)
      warnIndizes[warnCount++] = i; // Den Index der aktiven Warnung speichern
    }
  }

  String warnung = "0"; // Standardmäßig keine Warnung
  if (warnCount > 0) {
    // Wenn mehrere Warnungen aktiv sind, wählen wir eine zufällig aus,
    // um sie im 'warnungen' Topic zu veröffentlichen.
    // Das Dashboard zeigt dann die lesbare Form von 'Warnungen_r' an.
    int randIndex = random(warnCount);
    warnung = String(warnIndizes[randIndex]);
  }
  publishValue("warnungen", warnung);

  String Warnungen_r; // Deklariert einen String zum Speichern der finalen, formatierten Warnmeldung
  String wdescription; // Deklariert einen String zum Speichern der Textbeschreibung der Warnung

  // Überprüft den Wert der Variablen 'warnung' und weist die entsprechende Beschreibung zu
  if (warnung == "0") { // Beachten Sie, dass warnung ein String ist
      wdescription = "Keine Warnung"; 
  } else if (warnung == "1") {
      wdescription = "Battery voltage low";
  } else if (warnung == "2") {
      wdescription = "Battery voltage high";
  } else if (warnung == "3") {
      wdescription = "Battery voltage detection error";
  } else if (warnung == "4") {
      wdescription = "High temperature";
  } else if (warnung == "5") {
      wdescription = "Over load";
  } else if (warnung == "6") {
      wdescription = "Over charger current";
  } else if (warnung == "7") {
      wdescription = "Bus voltage high";
  } else if (warnung == "8") {
      wdescription = "Bus voltage low";
  } else if (warnung == "9") {
      wdescription = "PV voltage high";
  } else if (warnung == "10") {
      wdescription = "PV voltage low";
  } else if (warnung == "11") {
      wdescription = "Reserved 11";
  } else if (warnung == "12") {
      wdescription = "Reserved 12";
  } else if (warnung == "13") {
      wdescription = "Reserved 13";
  } else if (warnung == "14") {
      wdescription = "Reserved 14";
  } else if (warnung == "15") {
      wdescription = "Reserved 15";
  } else {
      // Falls der Warncode nicht in der Liste ist, wird eine generische Nachricht mit dem Code erstellt
      wdescription = "Unbekannte Warnung " + String(warnung);
  }

  // Erstellt die endgültige Warnmeldung, indem die Beschreibung und der Code kombiniert werden
  Warnungen_r = wdescription + " (Code: " + String(warnung) + ")";

  // Veröffentlicht die formatierte Warnmeldung
  publishValue("Warnungen_r", Warnungen_r);
}
