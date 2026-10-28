# Arduino-Sketch für eure Bewässerungsanlage

Die neue Datei ist `bewaesserung_sicher/bewaesserung_sicher.ino`. Sie wurde für euren Arduino Uno mit SH1106-OLED (128×64, I²C-Adresse 0x3C) erstellt. Sie ist noch nicht auf den Arduino hochgeladen.

## Änderungen

- Maximal fünf Pumpenrunden je Bewässerung. Wird das Feuchteziel danach nicht erreicht, folgen 30 Minuten Sperrzeit.
- Nach erfolgreicher Bewässerung folgen zehn Minuten Pause.
- Nach einem normalen Neustart folgen mindestens zehn Minuten Startpause. War eine Bewässerung unterbrochen oder eine Fehlersperre gespeichert, gelten mindestens 30 Minuten.
- Maximal 60 Sekunden Pumpenlaufzeit im zurückliegenden Zeitfenster von 60 Minuten. Bei weniger Restbudget wird eine Pumpenrunde gekürzt. Unter einer Sekunde Restbudget wird keine neue Runde gestartet.
- Sensoren und Taster werden auch während des Pumpens und Einziehens weiter verarbeitet. Die Messung mittelt zehn Proben über ungefähr 100 ms.
- Ein als leer erkannter Tank schaltet die Pumpe ab. Die vorhandene Hysterese bleibt erhalten: unter 100 = leer, über 200 = Wasser vorhanden.
- Im Einstellungsmenü bleibt die Pumpe aus. Das Öffnen während einer Einziehpause beendet die laufende Bewässerung und setzt die Fehlersperre.
- Ein nicht erreichbares OLED sperrt die Bewässerung. Ein erkanntes OLED-Problem bleibt bis zum Neustart gesperrt.
- Pumpbudget und eine Neustart-Sperrinformation werden im EEPROM gespeichert. Eine laufende Pumpenrunde wird vor dem Einschalten reserviert.
- Serielle Messwerte kommen ungefähr jede Sekunde mit 9600 Baud, auch während des Gießens. Die Ausgabe wird ohne blockierendes Warten auf freien UART-Speicher gesendet.
- Der OLED-Treiber nutzt einen kleinen Seitenpuffer. Übersichtsseite, Statistik, Rohwerte und Tastenmenü bleiben erhalten.

Das Stundenbudget zählt die gesamte Pumpzeit eines Zyklus bis eine Stunde nach dessen Ende. Das ist etwas strenger als eine exakte gleitende Einzelmessung. Nach einem Neustart werden gespeicherte Pumpzeiten vorsichtshalber nochmals eine Stunde berücksichtigt, weil der Uno ohne Echtzeituhr nicht weiß, wie lange er ausgeschaltet war. Ein Neustart gibt das Pumpbudget somit nicht einfach frei. Sperrzeiten können dadurch länger ausfallen.

## Grenzwerte anpassen

Oben im Sketch stehen:

```cpp
const uint32_t MAX_PUMP_MS_PRO_STUNDE = 60UL * 1000UL;
const uint32_t PAUSE_ERFOLG_MS = 10UL * 60UL * 1000UL;
const uint32_t PAUSE_FEHLER_MS = 30UL * 60UL * 1000UL;
const uint32_t PAUSE_NACH_START_MS = 10UL * 60UL * 1000UL;
```

Die 60 Sekunden sind ein vorsichtiger Startwert, keine auf eure Pumpe oder Pflanze abgestimmte Wassermenge. Pumpenfördermenge messen und anschließend die Grenze passend festlegen. Sperrzeiten in ganzen Minuten lassen, maximal 120 Minuten; das gespeicherte Pumpbudget unterstützt maximal 60.000 ms. Bei einer Änderung des Sicherheitskonzepts auch Speicherformat und Prüfungen anpassen.

Die vorhandenen Kalibrierwerte wurden übernommen. Boden- und Tanksensor müsst ihr am echten Aufbau kalibrieren. Der Tank-Prozentwert beschreibt die umgerechnete Sensoranzeige und kein gemessenes Wasservolumen.

## Verdrahtung

| Funktion | Arduino Uno |
|---|---|
| Tanksensor | A0 |
| Bodensensor | A1 |
| Menütaster gegen GND | D6, INPUT_PULLUP |
| Warn-LED | D7 |
| LOW-aktives Relais | D8 |
| OLED SDA / SCL | A4 / A5 |
| OLED-Adresse | 0x3C |

Die Relaislogik bleibt LOW = Pumpe an, HIGH = Pumpe aus. Beim Programmstart wird zuerst HIGH gesetzt, bevor D8 als Ausgang aktiviert wird. Während Bootloader und Reset kann der Sketch den Pin noch nicht steuern; ein geeigneter Hardware-Pull-up ist bei einem LOW-aktiven Relais zu prüfen.

## Später aufspielen

1. Pumpenversorgung für den Upload trennen und den bisherigen Sketch aufbewahren.
2. Den Ordner `bewaesserung_sicher` vollständig entpacken und darin `bewaesserung_sicher.ino` mit der Arduino IDE öffnen.
3. Im Bibliotheksverwalter **U8g2 von oliver** installieren. Die bisherigen Adafruit-OLED-Bibliotheken werden für diesen Sketch nicht benötigt.
4. Board **Arduino Uno** und den richtigen USB-Port auswählen.
5. Wenn der Arduino am Pi bleibt: Dort vor einem Upload oder dem Öffnen eines seriellen Monitors `sudo systemctl stop bewaesserung` ausführen, damit der USB-Port frei ist. Der hier bereitgestellte Sketch wird durch diesen Hinweis noch nicht aufgespielt.
6. Sketch zunächst überprüfen/kompilieren, danach hochladen.
7. OLED, Tankerkennung, Taster und serielle Ausgabe zunächst mit getrennter Pumpe prüfen. Die Startpause ist beabsichtigt. Anschließend einen beaufsichtigten Pumpentest machen.
8. Nach dem Test den seriellen Monitor schließen, Arduino am Pi anschließen und dort `sudo systemctl start bewaesserung` ausführen. Das Öffnen der seriellen Verbindung kann den Uno erneut starten und löst damit die Startpause aus.

## Zusammenarbeit mit Webseite und Telegram

Die bisherigen Messzeilen bleiben unverändert, beispielsweise:

```text
Boden: 700 (47%) | Tank: 250 (50%) | Wasser: JA | Pumpe: AUS
```

Damit können das bereits laufende Pi-Programm, die Webseite und der Telegram-Bot weiterhin Messwerte lesen. Zusätzlich sendet der Sketch:

```text
STATUS code=4 runde=1 max=5 sperre_s=0 budget_ms=55000 uptime_ms=625000
STATISTIK giessen=1 runden=1 sekunden=5
KONFIG start=700 ziel=350 pump_ms=5000 warte_ms=20000 menue=0
```

Diese zusätzlichen Informationen werden vom bisherigen Pi-Programm noch nicht ausgewertet. Um Sperrgrund, Statistik oder Pumpereignisse auf der Webseite anzuzeigen und über Telegram zu melden, muss dessen Auswertung später erweitert werden. Die Pumpensteuerung bleibt vollständig auf dem Arduino; es gibt keine Fernsteuerung über USB.

| Statuscode | Bedeutung |
|---|---|
| 0 | Bereit |
| 1 | Tank leer |
| 2 | Trocken, reservierter Statuscode |
| 3 | Pumpe läuft |
| 4 | Wasser einziehen lassen |
| 5 | Ziel erreicht, anschließend Pause |
| 6 | Fünf erfolglose Runden, anschließend Sperre |
| 7 | Stundenbudget oder Historienkapazität ausgeschöpft |
| 8 | Startpause |
| 9 | Einstellungsmenü |
| 10 | OLED nicht erreichbar |

`sperre_s` gibt die verbleibende Start-/Zykluspause an; beim Stundenlimit gilt zusätzlich das verbleibende Pumpbudget. `budget_ms` ist die noch verfügbare Pumpenlaufzeit, nicht die bereits verbrauchte Zeit. `uptime_ms` läuft nach ungefähr 49 Tagen über; die Zeitvergleiche berücksichtigen diesen Überlauf.

## Prüfung und Grenzen

Der Sketch wurde für **arduino:avr:uno** übersetzt und anhand simulierter Sensoren, Uhr, EEPROM, Taster und UART geprüft. Die Übersetzung prüft auch die alten EEPROM-Strukturgrößen auf dem ATmega328P. Alte Einstellungen und Zähler bleiben kompatibel.

Die Simulation ersetzt keinen Test von Relais, Pumpe, OLED und Sensoren an eurer Anlage. Die Abschaltung erfolgt softwareseitig in der nächsten Hauptschleife; sie ist keine unabhängige Hardwareabschaltung. Millisekunden können durch Übertragungen und Programmlaufzeit hinzukommen.

Verwendete Displaytechnik: [U8g2-Seitenpuffer](https://github.com/olikraus/u8g2/wiki/u8g2reference#firstpage).
