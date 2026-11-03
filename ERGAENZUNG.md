# Ergänzung: Statistik und OLED-Verlauf

Diese Änderung basiert auf dem Stand von `main` zum Zeitpunkt der Bearbeitung. Sie wurde noch nicht auf den Raspberry Pi oder Arduino übertragen. Die Pi-Dateien sind `app.py` und `index.html`; der ergänzte Arduino-Sketch ist `sketch_sep22a.ino`.

## Auf der Webseite

Die Statistik zeigt Anzahl, Minimum, Maximum und Durchschnitt der gespeicherten Boden- und Tankprozentwerte sowie die durchschnittlichen Rohwerte. Wählbar sind die letzten 24 Stunden, sieben Tage oder 30 Tage. Das Diagramm zeigt beide Sensoren, fasst Werte für die Darstellung zeitlich zusammen und unterbricht die Kurven bei Lücken von mehr als 60 Sekunden. Die Zahl der Messungen mit leer gemeldetem Tank ist keine Zeit- oder Volumenmessung. Der Mittelwert bezieht sich auf die vorhandenen Einzelmessungen, nicht auf die Zeit; Messlücken werden nicht aufgefüllt. Die Tank-Prozentwerte sind kein gemessenes Wasservolumen. Messwerte werden bereits etwa alle zehn Sekunden gespeichert.

Falls der separat vorhandene Sicherheitssketch `bewaesserung_sicher` auf dem Arduino läuft, zeigt die Webseite auch seine seriellen Status- und Statistikdaten an. Diese Angaben sind mit dem alten Sketch nicht verfügbar. Bedeutung der Statuscodes: `bewaesserung_sicher/ANLEITUNG.md`. Der Sicherheitssketch wurde für diese Ergänzung nicht geändert und besitzt deshalb noch keine Verlaufsseite.

## OLED und Taster

Im bisherigen Sketch schaltet ein kurzer Druck nach dem Loslassen zwischen Übersicht, Statistik, Rohwerten und der neuen Verlaufsgrafik weiter. Ein langer Druck von mindestens 1,5 Sekunden öffnet das vorhandene Einstellungsmenü; dort verändert ein kurzer Druck die Einstellung und ein langer Druck wechselt weiter bzw. speichert. Einstellungen beeinflussen die Bewässerung und sollten nur bewusst verändert werden.

Die Grafik hält bis zu 60 Bodenfeuchte-Messungen im Arduino-Arbeitsspeicher, ungefähr eine pro Minute. Nach einem Neustart beginnt sie leer; während langer Messpausen fehlen Punkte und die Kurve verbindet die benachbarten vorhandenen Werte. Sie wird nicht aus der Pi-Datenbank gespeist. Auf dem Bildschirm wächst sie von links nach rechts.

Der Sketch erwartet einen Taster zwischen D6 und GND und nutzt `INPUT_PULLUP`; gedrückt muss D6 LOW sein. Der bisherige Code enthält bereits eine Entprellung und die Seitenschaltung. Ein defekter Taster oder eine falsche Verdrahtung kann durch diese Änderung nicht behoben werden. Die tatsächliche Fehlerursache ist noch nicht festgestellt.

## Inbetriebnahme

1. Vor Änderungen die bestehenden Dateien sichern. Für die Pi-Webseite `app.py` und `index.html` auf den Pi in `/home/admin/bewaesserung` übernehmen und `sudo systemctl restart bewaesserung` ausführen. Die Datenbank und Telegram-Konfiguration nicht überschreiben. Danach `systemctl status bewaesserung --no-pager` und die Webseite prüfen.
2. Die OLED-Ergänzung gilt nur für `sketch_sep22a.ino`. Zuerst prüfen, welcher Sketch tatsächlich auf dem Uno läuft. Nicht den Sicherheitssketch durch den alten Sketch ersetzen, falls dessen Schutzfunktionen benötigt werden.
3. Vor Upload Pumpenversorgung trennen. Bei Upload über den Pi den Dienst mit `sudo systemctl stop bewaesserung` stoppen, damit die serielle Schnittstelle frei ist. Sketch in der Arduino IDE als Uno-Projekt mit den vorhandenen Adafruit-GFX- und Adafruit-SH110X-Bibliotheken kompilieren und erst danach aufspielen. Den seriellen Monitor schließen und den Dienst wieder mit `sudo systemctl start bewaesserung` starten.
4. Mit getrennter Pumpe OLED, Taster und Sensoranzeige überprüfen. Einmal kurz drücken und loslassen; viermal durch die Seiten schalten. Wenn keine Reaktion erfolgt, Anschluss D6 gegen GND und die Tasterkontakte prüfen. Bei Arbeiten an der Verdrahtung die Stromversorgung trennen. Erst nach erfolgreicher Prüfung einen beaufsichtigten Pumpentest durchführen.

## Prüfstand

Die Python-Auswertung, Messlücken, Parser und HTTP-API wurden mit Beispieldaten und einer temporären Datenbank geprüft. Die JavaScript-Syntax wurde geprüft; ein vollständiger Test am Pi und im Browser steht aus. Eine Arduino-Kompilierung und ein Test am echten Aufbau waren hier nicht möglich. Die Pumpenlogik wurde nicht absichtlich verändert, aber der geänderte Sketch muss vor Einsatz kompiliert und am Aufbau geprüft werden.
