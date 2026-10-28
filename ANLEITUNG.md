# Gießwerk auf eurem Raspberry Pi

Webseite im Projektnetz: http://192.168.222.153:8080

Die Software liegt auf dem Pi unter `/home/admin/bewaesserung` und startet automatisch. Sie liest den vorhandenen Arduino-Code mit 9600 Baud; das Arduino-Programm wurde nicht verändert. Die Pumpe wird weiterhin ausschließlich vom Arduino gesteuert.

## Telegram aktivieren

1. In Telegram den offiziellen Bot **@BotFather** öffnen und mit `/newbot` einen eigenen Bot erstellen.
2. In eurer SSH-Sitzung auf dem Pi starten:

```bash
python3 ~/bewaesserung/telegram_einrichten.py
```

3. Den Token an der unsichtbaren Eingabe eingeben. Nicht im Chat oder in einen Shell-Befehl kopieren.
4. Eurem neuen Bot `/start` senden und der Einrichtung folgen. Eure private Chat-ID auswählen.
5. Danach `/status` senden. Der laufende Dienst übernimmt die Konfiguration automatisch. Nur der konfigurierte Chat bekommt Antworten und Warnungen.

Der Bot sendet automatisch alle fünf Minuten den aktuellen Status. Nach einem Dienststart folgt die erste Statusnachricht, sobald die ersten Messwerte eingetroffen sind. `/status` liefert jederzeit zusätzlich eine Antwort. Außerdem meldet er leeren Tank oder fehlende aktuelle Messwerte sowie die Rückkehr zum Normalzustand. Für den Bot braucht der Pi Internet. Webseitenzugriff bleibt lokal; eine Portfreigabe ist nicht eingerichtet.

## Betrieb und Fehlersuche

```bash
systemctl status bewaesserung --no-pager
journalctl -u bewaesserung -n 30 --no-pager
sudo systemctl restart bewaesserung
```

Beim Öffnen der seriellen Verbindung kann sich ein Uno neu starten. Ein Dienstneustart öffnet diese Verbindung erneut.

Gespeichert werden Messwerte ungefähr alle 10 Sekunden für maximal 30 Tage in `messwerte.sqlite3`. Der Verlauf auf der Seite zeigt die letzten 24 Stunden. Die Website erlaubt nur Lesen; jeder mit Netzwerkzugriff auf Port 8080 kann die Messwerte sehen.

Boden- und Tankprozente werden aus eurem Arduino-Code übernommen. Die Tankanzeige ist kein kalibriertes Wasservolumen. Euer Arduino sendet während des Pumpens keine vollständigen Messzeilen; daher werden die letzten Sensorwerte angezeigt. Pumpenwechsel werden separat gelesen. Nach 45 Sekunden ohne neue Messung werden Daten als veraltet markiert.

Der Telegram-Token steht nur auf dem Pi in `telegram.json`, lesbar für Benutzer admin. Die HTTP-Routen geben weder Konfiguration noch Token aus. Telegram bleibt ohne Einrichtung deaktiviert.

## Prüfstand bei Einrichtung

SSH-Anmeldung, Erkennung des Arduino Uno, Einlesen echter Messwerte, Speicherung, Status-API, Webseitenanzeige und aktivierter Autostart wurden geprüft. Telegram benötigt noch euren Token und den anschließenden echten Nachrichtentest. Ein Neustart des gesamten Pi wurde nicht durchgeführt.
