# Infini V Serie Wechselrichter MQTT-Bridge mit ESP8266 (ESP D1 mini)

Dieses Projekt ermöglicht das Auslesen eines **Infini V Serien Wechselrichters** über die **RS232-Schnittstelle** mittels eines **ESP D1 mini**. 
Die erfassten Werte werden an einen **MQTT-Server** übertragen und können anschließend z. B. mit **Node-RED** in eine Datenbank wie **Solaranzeige** ([https://solaranzeige.de](https://solaranzeige.de)) oder andere Systeme weitergeleitet werden.

> ⚠️ Ein Node-RED-Flow (!BETA!) zur Weiterleitung in die InfluxDB der Solaranzeige ist unter NodeRed/flows.json zu finden.

---
## 🚀 Updates <br>
<b></b>v1.5</b> <br>
++ MQTT Authentifikation<br>
++ Disable MQTT<br>
++ Einheiten auf dem Dashboard<br><br>
<b></b>v1.4</b> <br>
++ Klartexte im Webinterface (Mode, Fehler und Warnungen) <br>
++ Timeings in der Config anpassbar <br>
~~ Batterieentladestrom Faktor korrigiert <br>
~~ Eindeutige MQTT-ID <br>

---

## 🛠️ Benötigte Hardware

- ESP D1 mini (ESP8266)
- MAX3232 RS232-zu-TTL-Konverter (3.3V, DB9 männlich)  (meiner benötigt 5V)
- Infini RS232-Adapterkabel (im Lieferumfang des Wechselrichters enthalten)
- Micro-USB-Kabel
- USB-Netzteil

---

## 🔌 Verkabelung

| ESP D1 mini Pin | MAX3232 Pin |
|------------------|--------------|
| D1               | RX           |
| D2               | TX           |
| GND              | GND          |
| 5V               | VCC          |

---

## ⚙️ Konfiguration

- Eine zum flashen bereite firmware für den ESP-D1_mini findet Ihr unter **bin_D1mini/infini_v-2-ESP-2-mqtt_V1.5.bin**
- Nach dem flashen öffnet der ESP ein Wlan **"InfiniConfig"** damit verbinden, zur Konfiguration kommt man dann mit **http://192.168.4.1/config**

---

## 📤 MQTT-Daten

Folgende Messwerte werden regelmäßig über MQTT bereitgestellt:

- `netzspannung`  
- `netzfrequenz`  
- `ac_ausgangsspannung`  
- `ac_ausgangsfrequenz`  
- `ac_scheinleistung`  
- `ac_wirkleistung`  
- `ausgangslast`  
- `batteriespannung`  
- `batterieentladestrom`  
- `batterieladestrom`  
- `batteriekapazitaet`  
- `temperatur_gehaeuse`  
- `mppt1_temperatur`  
- `mppt2_temperatur`  
- `solarleistung1`  
- `solarleistung2`  
- `solarspannung1`  
- `solarspannung2`  
- `ladestatus1`  
- `ladestatus2`  
- `batteriestromrichtung`  
- `wr_stromrichtung`  
- `netzstromrichtung`  
- `firmware`  
- `stunden`  
- `minuten`  
- `sekunden`  
- `mode`  
- `fehler`  
- `warnungen`  

---

## 🧩 Kompatibilität

- Getestet mit **Infini V Serie**
- Ausgelegt für die Integration mit **MQTT-basierten Automatisierungssystemen**
- Geeignet für den Einsatz mit **Node-RED** und **Solaranzeige**

---

## 📌 Hinweise

- In der Node-RED-Flow zur Anbindung an Solaranzeige (flows.json) wird vom "AUS Wächter" ein Modus 10 gesetzt.
  Dieser existiert im Infini nicht und wird bei mir für den Zustand "AUS" (wenn für mehr als 10sek keine MQTT Werte mehr ankommen) verwendet.
