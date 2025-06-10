                                                                 #include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// WLAN Konfiguration
const char* ssid = "WLAN-SSID";          // WLAN
const char* password = "WLAN-PASSWORT";  //WLAN
IPAddress local_IP(192, 168, 1, 2);      //static IP
IPAddress gateway(192, 168, 1, 1);       //gateway
IPAddress subnet(255, 255, 255, 0);      //subnet

// MQTT Konfiguration
const char* mqtt_server = "192.168.3.3";  //MQTT-Server
const char* mqtt_topic = "infini";        //MQTT-Topic
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

const int NUM_BEFEHLE = sizeof(befehle) / sizeof(befehle[0]); // <--- DIESE ZEILE HIER!

// Timer-Variablen für nicht-blockierende Wartezeiten
unsigned long lastActionMillis = 0; // Für die Zeit zwischen Aktionen (Senden, Warten, etc.)
const long commandDelayMillis = 50; // Kurze Pause nach dem Senden eines Befehls, bevor Antwort erwartet wird (kann angepasst werden)
const long responseTimeoutMillis = 750; // Maximal 0.75 Sekunden auf Antwort warten (angepasst für schnellere Abfrage)
const long fullCycleDelayMillis = 500; // 1 Sekunde Pause nach komplettem Zyklus (angepasst für schnellere Abfrage)

// Deklariere currentCommandIndex HIER, damit er global sichtbar ist
int currentCommandIndex = 0;

// Zustandsmaschine für den Lese-/Sende-Prozess
enum AppState {
  STATE_IDLE_START_CYCLE,       // Startzustand, wartet auf Beginn des nächsten Zyklus
  STATE_SEND_COMMAND,           // Senden eines Befehls an den Inverter
  STATE_READ_RESPONSE,          // Zeichenweise die Antwort lesen
  STATE_PROCESS_DATA,           // Empfangene Daten verarbeiten und MQTT publishen
  STATE_WAIT_BETWEEN_COMMANDS,  // Warten auf die commandDelayMillis vor dem nächsten Befehl
  STATE_WAIT_FOR_NEXT_CYCLE     // Warten bis der nächste volle Zyklus beginnt
};
AppState currentState = STATE_IDLE_START_CYCLE;

String currentResponseBuffer = ""; // Puffer für die serielle Antwort
unsigned long responseStartTime = 0; // Zeitstempel, wann die Antwort erwartet wurde

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

  // Statische IP konfigurieren
  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);

  Serial.print("Verbinde mit WLAN...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nVerbunden! IP: " + WiFi.localIP().toString());

  // MQTT konfigurieren
  client.setServer(mqtt_server, 1883);
  reconnectMQTT();

  lastActionMillis = millis(); // Initialisiere Timer
}

void loop() {
  // Immer als erstes in loop() ausführen, um MQTT-Verbindung aktiv zu halten!
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  unsigned long currentMillis = millis();

  switch (currentState) {
    case STATE_IDLE_START_CYCLE:
      // Dieser Zustand stellt sicher, dass wir am Anfang eines *neuen* Zyklus sind
      if (currentMillis - lastActionMillis >= fullCycleDelayMillis || currentCommandIndex == 0) {
        currentCommandIndex = 0; // Sicherstellen, dass wir von vorne beginnen
        Serial.println("\nStarte neuen Abfragezyklus.");
        currentState = STATE_SEND_COMMAND;
        lastActionMillis = currentMillis; // Timer für den nächsten Schritt zurücksetzen
      }
      break;

    case STATE_SEND_COMMAND:
      if (currentCommandIndex < NUM_BEFEHLE) {
        const char* cmd = befehle[currentCommandIndex];
        Serial.print("Sende: ");
        Serial.println(cmd);

        uint16_t crc = crc16_xmodem((uint8_t*)cmd, strlen(cmd));
        inverterSerial.print(cmd);
        inverterSerial.write((crc >> 8) & 0xFF);
        inverterSerial.write(crc & 0xFF);
        inverterSerial.write('\r');

        currentResponseBuffer = ""; // Puffer leeren für neue Antwort
        responseStartTime = currentMillis; // Startzeit für Timeout setzen
        currentState = STATE_READ_RESPONSE; // Zum Lesen der Antwort übergehen
        lastActionMillis = currentMillis; // Timer für den nächsten Schritt zurücksetzen
      } else {
        // Alle Befehle in diesem Zyklus gesendet
        currentState = STATE_WAIT_FOR_NEXT_CYCLE;
        lastActionMillis = currentMillis; // Timer für die volle Zykluspause setzen
        Serial.println("Alle Befehle gesendet. Warte " + String(fullCycleDelayMillis / 1000) + " Sekunden für den nächsten Zyklus...\n");
      }
      break;

    case STATE_READ_RESPONSE:
      // Prüfe auf verfügbare serielle Daten ODER auf Timeout
      if (inverterSerial.available()) {
        char c = inverterSerial.read();
        currentResponseBuffer += c;
        if (c == '\r') { // Antwort endet normalerweise mit '\r'
          currentState = STATE_PROCESS_DATA; // Gehe zur Verarbeitung über
          lastActionMillis = currentMillis; // Timer für den nächsten Schritt zurücksetzen
          // Optional: Serial.print("Antwort (komplett): "); Serial.println(currentResponseBuffer);
        }
      } else if (currentMillis - responseStartTime > responseTimeoutMillis) {
        Serial.println("Timeout beim Lesen der Antwort für Befehl: " + String(befehle[currentCommandIndex]));
        currentResponseBuffer = ""; // Puffer leeren
        currentState = STATE_WAIT_BETWEEN_COMMANDS; // Gehe zur Pause zwischen Befehlen
        lastActionMillis = currentMillis; // Timer für den nächsten Schritt zurücksetzen
      }
      break;

    case STATE_PROCESS_DATA:
      // Daten verarbeiten und publishen
      if (currentResponseBuffer.length() > 0) {
        String payload = extractPayload(currentResponseBuffer);
        if (payload.length() > 0) {
          // Serial.print("Nutzdaten: "); Serial.println(payload); // Optional: Reduziert Serial Output

          // Finden Sie den Befehl, der gerade gesendet wurde
          const char* currentCmd = befehle[currentCommandIndex];

          // Je nach Befehl unterschiedliche Verarbeitung
          if (String(currentCmd).endsWith("GS")) parseAndSendGS(payload);
          else if (String(currentCmd).endsWith("PI")) parseAndSendPI(payload);
          else if (String(currentCmd).endsWith("T")) parseAndSendT(payload);
          else if (String(currentCmd).endsWith("MOD")) parseAndSendMOD(payload);
          else if (String(currentCmd).endsWith("FWS")) parseAndSendFWS(payload);
        } else {
          Serial.println("Keine gültige Nutzdatenantwort erkannt für Befehl: " + String(befehle[currentCommandIndex]));
        }
      }
      Serial.println("-------------------------------\n");
      // Nach der Verarbeitung zum Wartezustand wechseln
      currentState = STATE_WAIT_BETWEEN_COMMANDS;
      lastActionMillis = currentMillis; // Timer für die Pause zwischen Befehlen setzen
      break;

    case STATE_WAIT_BETWEEN_COMMANDS:
      // Warte kurz, bevor der nächste Befehl gesendet wird
      if (currentMillis - lastActionMillis >= commandDelayMillis) {
          currentCommandIndex++; // Nächsten Befehl vorbereiten
          currentState = STATE_SEND_COMMAND; // Gehe zum Senden des nächsten Befehls
          lastActionMillis = currentMillis; // Timer für den nächsten Schritt zurücksetzen
      }
      break;

    case STATE_WAIT_FOR_NEXT_CYCLE:
      if (currentMillis - lastActionMillis >= fullCycleDelayMillis) {
        currentState = STATE_IDLE_START_CYCLE; // Beginne einen neuen Zyklus
      }
      break;
  }
}

// MQTT-Verbindung herstellen
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Verbinde MQTT...");
    if (client.connect("InfiniClient")) {
      Serial.println("verbunden!");
    } else {
      Serial.print("Fehler: ");
      Serial.print(client.state());
      Serial.println(" - Neuer Versuch in 2s");
      delay(2000); // Dieser delay ist hier akzeptabel, da er nur bei Verbindungsverlust auftritt
    }
  }
}

// Extrahiere Payload aus ^Dnnn Antwort
String extractPayload(const String& raw) {
  int start = raw.indexOf("^D");
  if (start != -1 && raw.length() >= start + 5) {
    String lenStr = raw.substring(start + 2, start + 5);
    int len = lenStr.toInt();
    // Prüfe, ob die Länge Sinn ergibt und nicht zu einem Überlauf führt
    if (len > 0 && (start + 5 + len - 1) < raw.length()) { // Korrigiert: <= zu <, da substring endIndex exklusive ist
      return raw.substring(start + 5, start + 5 + len); // Korrigiert: endIndex muss die Länge des Payloads umfassen
    } else if (len > 0 && (start + 5 + len -1) == raw.length() -1 && raw.endsWith("\r")) { // Fall für das letzte Zeichen (CR)
      return raw.substring(start + 5, start + 5 + len -1); // Ohne CR
    }
  }
  return "";
}


// HILFSFUNKTION FÜR MQTT-PUBLISH - Sendet direkt an ein Topic
void publishValue(const String& field, const String& value) {
  String topic = String(mqtt_topic) + "/" + field;
  if (client.publish(topic.c_str(), value.c_str())) {
    // Serial.print("Publish: "); Serial.print(topic); Serial.print(" -> "); Serial.println(value); // Optional: Reduziert Serial Output
  } else {
    Serial.print("FEHLER beim Publish von ");
    Serial.print(topic);
    Serial.print(": Client State ");
    Serial.println(client.state());
  }
}


void parseAndSendGS(String payload) {
  // Serial.println("\nVerarbeite GS-Daten:"); // Optional: Reduziert Serial Output
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  String felder[30];
  int index = 0, last = 0;

  for (int i = 0; i <= payload.length(); i++) {
    if (i == payload.length() || payload[i] == ',') {
      felder[index++] = payload.substring(last, i);
      last = i + 1;
    }
  }

  if (index < 26) {
    Serial.println("Antwort unvollständig für GS-Daten.");
    return;
  }

  // Feldnamen für MQTT-Topics
  const char* fieldNames[] = {
    "netzspannung", "netzfrequenz", "ac_ausgangsspannung", "ac_ausgangsfrequenz",
    "ac_scheinleistung", "ac_wirkleistung", "ausgangslast", "batteriespannung",
    "", "",  // Platzhalter für 8,9
    "batterieentladestrom", "batterieladestrom", "batteriekapazitaet", "temperatur_gehaeuse",
    "mppt1_temperatur", "mppt2_temperatur", "solarleistung1", "solarleistung2",
    "solarspannung1", "solarspannung2", "",  // Platzhalter für 20
    "ladestatus1", "ladestatus2", "batteriestromrichtung", "wr_stromrichtung", "netzstromrichtung"
  };
  // Nur relevante Felder senden
  const int relevantFields[] = {0,1,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,19,21,22,23,24,25};
  const int numFields = sizeof(relevantFields) / sizeof(relevantFields[0]);
  for (int i = 0; i < numFields; i++) {
    int idx = relevantFields[i];
    if (fieldNames[idx][0] != '\0') { // Nur wenn Feldname existiert
      String value;
      // Spezielle Formatierungen für bestimmte Felder
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
  // Serial.println("\nVerarbeite PI-Daten:"); // Optional: Reduziert Serial Output
  String cleanf = "";
  for (unsigned int i = 0; i < payload.length(); i++) {
    char c = payload[i];
    if (isPrintable(c)) {
      cleanf += c;
    }
  }
  publishValue("firmware", cleanf);
}

void parseAndSendT(String payload) {
  // Serial.println("\nVerarbeite T-Daten:"); // Optional: Reduziert Serial Output
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  // Entferne alle nicht-numerischen Zeichen (z. B. ESC)
  String clean = "";
  for (char c : payload) {
    if (isDigit(c)) clean += c;
  }

  if (clean.length() >= 14) {
    String stunde  = clean.substring(8, 10);
    String minute  = clean.substring(10, 12);
    String sekunde = clean.substring(12, 14);

    publishValue("stunden", stunde);
    publishValue("minuten", minute);
    publishValue("sekunden", sekunde);
  } else {
    publishValue("zeit", "ungültig");
  }
}

void parseAndSendMOD(String payload) {
  // Serial.println("\nVerarbeite MOD-Daten:"); // Optional: Reduziert Serial Output
  // Nur druckbare Zeichen herausfiltern
  String clean = "";
  for (unsigned int i = 0; i < payload.length(); i++) {
    char c = payload[i];
    if (isPrintable(c)) {
      clean += c;
    }
  }

  // Modus-Zuordnung (basierend auf bekannter Liste)
  String readable;
  if (clean == "00") readable = "Power On Mode";
  else if (clean == "01") readable = "Standby Mode";
  else if (clean == "02") readable = "Line Mode";
  else if (clean == "03") readable = "Battery Mode";
  else if (clean == "04") readable = "Fault Mode";
  else if (clean == "05") readable = "Hybrid Mode";
  else readable = "Unbekannter Modus (" + clean + ")";

  publishValue("mode", clean); // Veröffentliche den Rohwert, da die Umwandlung nur zur Lesbarkeit dient
}

void parseAndSendFWS(String payload) {
  // Serial.println("\nVerarbeite FWS-Daten:"); // Optional: Reduziert Serial Output
  // Klammern entfernen
  if (payload.startsWith("(") && payload.endsWith(")")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  // "k:" und andere nicht-zahlen entfernen
  int kpos = payload.indexOf("k:");
  if (kpos != -1) {
    payload = payload.substring(0, kpos);
  }

  // Zerlege CSV
  String teile[20];
  int index = 0, last = 0;
  for (int i = 0; i < payload.length(); i++) {
    if (payload[i] == ',') {
      teile[index++] = payload.substring(last, i);
      last = i + 1;
    }
    // Sicherstellen, dass das Array nicht überläuft
    if (index >= 20) {
        Serial.println("Warnung: 'teile' Array-Grenze erreicht in FWS-Parsing.");
        break;
    }
  }
  if (index < 20) { // Nur hinzufügen, wenn noch Platz ist
      teile[index++] = payload.substring(last); // letzter Teil
  }

  // Fehlercode extrahieren
  String fehler = (index > 0) ? teile[0] : "N/A"; // Sicherstellen, dass teile[0] existiert
  publishValue("fehler", fehler);
  // Warnungen zählen (wie PHP)
  int warnungen = 0;
  for (int i = 1; i < index && i < 17; i++) {
    if (teile[i].toInt() == 1) {
      warnungen++;
    }
  }
  publishValue("warnungen", String(warnungen));
}