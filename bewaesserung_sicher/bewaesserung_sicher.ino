#include <Wire.h>
#include <EEPROM.h>
#include <U8g2lib.h>
#include <avr/pgmspace.h>
#include <stdio.h>

// Arduino Uno, SH1106 128x64 an 0x3C; Relais LOW-aktiv.
// A0 Tank, A1 Boden, D6 Taster gegen GND, D7 Warn-LED, D8 Relais.
// Keine Steuerbefehle vom Pi: die Bewaesserung bleibt eigenstaendig.

const byte TANK_PIN = A0, SOIL_PIN = A1;
const byte BUTTON_PIN = 6, LED_PIN = 7, RELAY_PIN = 8;
const byte RELAY_AN = LOW, RELAY_AUS = HIGH;
// Ein 128-Byte-Seitenpuffer spart auf dem Uno 896 Byte gegenueber Vollbild.
U8G2_SH1106_128X64_NONAME_1_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
bool oledZeichnet = false;

// Kalibrierung aus eurem bisherigen Sketch; vor Ort nachmessen.
const int SOIL_TROCKEN = 1020, SOIL_NASS = 350;
const int TANK_MIN = 0, TANK_MAX = 500;
const int TANK_LEER = 100, TANK_WASSER = 200;
int SOIL_START = 700, SOIL_ZIEL = 350;
unsigned long PUMP_DAUER = 5000, WARTEZEIT = 20000;

// SICHERHEITSGRENZEN: nach Pumpenleistung und Pflanze anpassen.
const byte MAX_RUNDEN = 5;
const uint32_t STUNDE_MS = 60UL * 60UL * 1000UL;
const uint32_t MAX_PUMP_MS_PRO_STUNDE = 60UL * 1000UL;
const uint32_t PAUSE_ERFOLG_MS = 10UL * 60UL * 1000UL;
const uint32_t PAUSE_FEHLER_MS = 30UL * 60UL * 1000UL;
const uint32_t PAUSE_NACH_START_MS = 10UL * 60UL * 1000UL;
const uint32_t SENSOR_INTERVALL_MS = 10;
const uint32_t SERIAL_INTERVALL_MS = 1000;

// Codes 0..6 entsprechen den Statuswerten eures alten Sketches.
enum SystemStatus : uint8_t {
  STATUS_BEREIT, STATUS_TANK_LEER, STATUS_TROCKEN,
  STATUS_GIESSEN, STATUS_WARTEN, STATUS_ZIEL, STATUS_MAX_RUNDEN,
  STATUS_LIMIT, STATUS_STARTPAUSE, STATUS_MENUE, STATUS_OLED_FEHLER
};
SystemStatus aktuellerStatus = STATUS_STARTPAUSE;
SystemStatus pausenGrund = STATUS_STARTPAUSE;
bool oledOK = false, sensorenBereit = false;
int bodenWert = 0, tankWert = 0;
bool tankOK = false, pumpeAN = false, bewaesserungAktiv = false;
uint8_t aktuelleRunde = 0;
uint32_t pumpStart = 0, pumpLimitMs = 0, warteStart = 0;
uint16_t zyklusPumpMs = 0;
bool pauseAktiv = true;
uint32_t pauseStart = 0, pauseDauer = PAUSE_NACH_START_MS;

// EEPROM-Strukturen und Adressen bleiben fuer alte Daten kompatibel
// (Arduino Uno/AVR). Neue Budgetdaten folgen dahinter.
const uint16_t EEPROM_MAGIC = 0xBEEF, EINSTELLUNGEN_MAGIC = 0xCAFE;
struct Statistik {
  uint16_t magic;
  uint32_t giessVorgaenge, pumpRunden, pumpSekunden;
};
struct Einstellungen {
  uint16_t magic;
  int soilStart, soilZiel;
  unsigned long pumpDauer, wartezeit;
};
Statistik statistik;
uint16_t statistikRestMs = 0;
#if defined(__AVR_ATmega328P__)
static_assert(sizeof(Statistik) == 14, "Altes Statistik-Layout muss erhalten bleiben");
static_assert(sizeof(Einstellungen) == 14, "Altes Einstellungs-Layout muss erhalten bleiben");
#endif
const int EEPROM_EINSTELLUNGEN_ADRESSE = sizeof(Statistik);
const int EEPROM_BUDGET_ADRESSE = sizeof(Statistik) + sizeof(Einstellungen);
const uint16_t BUDGET_MAGIC = 0x7A31;
struct BudgetSpeicher {
  uint16_t magic, reserviertMs, gegenprobe;
  uint8_t pauseMin, pauseGegenprobe;
};
uint16_t letztesBudget = 0xFFFF;
uint8_t sicherheitsPauseMin = PAUSE_NACH_START_MS / 60000UL;
uint8_t letzteGespeichertePause = 0xFF;

// Gesamte Pumpenzeit eines Zyklus wird konservativ bis eine Stunde
// nach dessen ENDE gezaehlt. 8 Eintraege reichen bei 10 min Mindestpause.
// Ein volles Journal sperrt weitere Zyklen, statt alte Daten zu verlieren.
struct PumpHistorie { uint32_t ende; uint16_t dauer; };
const uint8_t HISTORIE_ANZAHL = 8;
PumpHistorie historie[HISTORIE_ANZAHL];
uint8_t historieKopf = 0, historieZahl = 0;

byte menuSeite = 0, einstellungsSeite = 0;
bool einstellungsMenue = false;
bool letzterButtonWert = HIGH, buttonStatus = HIGH;
uint32_t buttonAenderung = 0, buttonStart = 0;
const uint32_t BUTTON_DEBOUNCE = 40, BUTTON_LANG = 1500;
uint32_t sensorLetzteProbe = 0, oledLetztesBild = 0, letzteLEDZeit = 0;
uint16_t bodenSumme = 0, tankSumme = 0;
uint8_t sensorProben = 0;
bool ledStatus = false;

// UART wird in kleinen Stuecken beschrieben. Ein voller Sendepuffer
// darf die Pumpenabschaltung nicht durch Serial.print() blockieren.
char serialPuffer[112];
uint8_t serialLaenge = 0, serialPosition = 0, serialPhase = 4;
uint32_t letzteSerialGruppe = 0;

uint32_t vergangen(uint32_t jetzt, uint32_t seit) { return jetzt - seit; }

void statistikSpeichern() { EEPROM.put(0, statistik); }

void einstellungenSpeichern() {
  Einstellungen e = {EINSTELLUNGEN_MAGIC, SOIL_START, SOIL_ZIEL,
                       PUMP_DAUER, WARTEZEIT};
  EEPROM.put(EEPROM_EINSTELLUNGEN_ADRESSE, e);
}

void eepromLaden() {
  EEPROM.get(0, statistik);
  if (statistik.magic != EEPROM_MAGIC) {
    statistik = {EEPROM_MAGIC, 0, 0, 0};
    statistikSpeichern();
  }
  Einstellungen e;
  EEPROM.get(EEPROM_EINSTELLUNGEN_ADRESSE, e);
  if (e.magic == EINSTELLUNGEN_MAGIC && e.soilStart >= 25 &&
      e.soilStart <= 1000 && e.soilZiel >= 0 && e.soilZiel < e.soilStart &&
      e.pumpDauer >= 1000 && e.pumpDauer <= 30000 &&
      e.wartezeit >= 5000 && e.wartezeit <= 60000) {
    SOIL_START = e.soilStart; SOIL_ZIEL = e.soilZiel;
    PUMP_DAUER = e.pumpDauer; WARTEZEIT = e.wartezeit;
  } else {
    einstellungenSpeichern();
  }
}

void budgetSpeichern(uint32_t reserviert) {
  uint16_t wert = (uint16_t)min(reserviert, MAX_PUMP_MS_PRO_STUNDE);
  if (wert == letztesBudget && sicherheitsPauseMin == letzteGespeichertePause) return;
  BudgetSpeicher b = {BUDGET_MAGIC, wert, (uint16_t)~wert,
    sicherheitsPauseMin, (uint8_t)~sicherheitsPauseMin};
  // Gegenprobe macht unterbrochene Schreibvorgaenge erkennbar.
  EEPROM.put(EEPROM_BUDGET_ADRESSE, b);
  letztesBudget = wert;
  letzteGespeichertePause = sicherheitsPauseMin;
}

void historieBereinigen(uint32_t jetzt) {
  while (historieZahl && vergangen(jetzt, historie[historieKopf].ende) >= STUNDE_MS) {
    historieKopf = (historieKopf + 1) % HISTORIE_ANZAHL;
    historieZahl--;
  }
}

uint32_t pumpVerbrauch(uint32_t jetzt) {
  historieBereinigen(jetzt);
  uint32_t summe = zyklusPumpMs;
  for (uint8_t i = 0; i < historieZahl; i++) {
    summe += historie[(historieKopf + i) % HISTORIE_ANZAHL].dauer;
  }
  if (pumpeAN) summe += vergangen(jetzt, pumpStart);
  return summe;
}

uint32_t pumpRest(uint32_t jetzt) {
  uint32_t verbrauch = pumpVerbrauch(jetzt);
  return verbrauch >= MAX_PUMP_MS_PRO_STUNDE ? 0 : MAX_PUMP_MS_PRO_STUNDE - verbrauch;
}

void budgetLaden(uint32_t jetzt) {
  BudgetSpeicher b;
  EEPROM.get(EEPROM_BUDGET_ADRESSE, b);
  uint16_t wert;
  if (b.magic != BUDGET_MAGIC) {
    // Neue Installation: keine alten Budgetdaten vorhanden.
    wert = 0;
  } else if (b.reserviertMs > MAX_PUMP_MS_PRO_STUNDE ||
             b.gegenprobe != (uint16_t)~b.reserviertMs ||
             b.pauseGegenprobe != (uint8_t)~b.pauseMin ||
             b.pauseMin < 1 || b.pauseMin > 120) {
    // Unplausibler Datensatz: fuer eine Stunde kein neues Pumpbudget.
    wert = (uint16_t)MAX_PUMP_MS_PRO_STUNDE;
    sicherheitsPauseMin = PAUSE_FEHLER_MS / 60000UL;
  } else {
    wert = b.reserviertMs;
    sicherheitsPauseMin = max(b.pauseMin, (uint8_t)(PAUSE_NACH_START_MS / 60000UL));
  }
  if (wert) {
    historie[0] = {jetzt, wert};
    historieZahl = 1;
  }
  budgetSpeichern(wert);
}

uint32_t pauseRest(uint32_t jetzt) {
  if (!pauseAktiv) return 0;
  uint32_t zeit = vergangen(jetzt, pauseStart);
  if (zeit >= pauseDauer) {
    pauseAktiv = false;
    sicherheitsPauseMin = PAUSE_NACH_START_MS / 60000UL;
    budgetSpeichern(pumpVerbrauch(jetzt));
    return 0;
  }
  return pauseDauer - zeit;
}

void pauseSetzen(uint32_t jetzt, uint32_t dauer, SystemStatus grund) {
  pauseAktiv = true; pauseStart = jetzt; pauseDauer = dauer; pausenGrund = grund;
}

void pumpeStoppen(uint32_t jetzt) {
  digitalWrite(RELAY_PIN, RELAY_AUS);  // Zuerst physisch ausschalten.
  if (!pumpeAN) return;
  uint32_t laufzeit = vergangen(jetzt, pumpStart);
  pumpeAN = false;
  zyklusPumpMs = (uint16_t)min((uint32_t)zyklusPumpMs + laufzeit,
                              MAX_PUMP_MS_PRO_STUNDE);
  statistik.pumpRunden++;
  uint32_t statistikZeit = laufzeit + statistikRestMs;
  statistik.pumpSekunden += statistikZeit / 1000UL;
  statistikRestMs = statistikZeit % 1000UL;
  budgetSpeichern(pumpVerbrauch(jetzt));
}

void zyklusBeenden(uint32_t jetzt, SystemStatus grund, bool erfolg) {
  pumpeStoppen(jetzt);
  if (zyklusPumpMs) {
    historieBereinigen(jetzt);
    if (historieZahl < HISTORIE_ANZAHL) {
      uint8_t index = (historieKopf + historieZahl) % HISTORIE_ANZAHL;
      historie[index] = {jetzt, zyklusPumpMs};
      historieZahl++;
    }
    zyklusPumpMs = 0;
  }
  bewaesserungAktiv = false;
  aktuellerStatus = grund;
  pauseSetzen(jetzt, erfolg ? PAUSE_ERFOLG_MS : PAUSE_FEHLER_MS, grund);
  sicherheitsPauseMin = (erfolg ? PAUSE_ERFOLG_MS : PAUSE_FEHLER_MS) / 60000UL;
  budgetSpeichern(pumpVerbrauch(jetzt));
  statistikSpeichern();
}

bool pumpRundeStarten(uint32_t jetzt) {
  uint32_t rest = pumpRest(jetzt);
  if (!tankOK || !sensorenBereit || !oledOK || einstellungsMenue) return false;
  if (rest < 1000 || historieZahl >= HISTORIE_ANZAHL) {
    zyklusBeenden(jetzt, STATUS_LIMIT, false);
    return false;
  }
  pumpLimitMs = min((uint32_t)PUMP_DAUER, rest);
  // Vor dem Einschalten reservieren. Ein Reset darf das Budget nicht loeschen.
  sicherheitsPauseMin = PAUSE_FEHLER_MS / 60000UL;
  budgetSpeichern(pumpVerbrauch(jetzt) + pumpLimitMs);
  aktuelleRunde++;
  pumpStart = (uint32_t)millis();
  digitalWrite(RELAY_PIN, RELAY_AN);
  pumpeAN = true;
  aktuellerStatus = STATUS_GIESSEN;
  return true;
}

void sensorenAktualisieren(uint32_t jetzt) {
  if (vergangen(jetzt, sensorLetzteProbe) < SENSOR_INTERVALL_MS) return;
  sensorLetzteProbe = jetzt;
  bodenSumme += analogRead(SOIL_PIN);
  tankSumme += analogRead(TANK_PIN);
  if (++sensorProben < 10) return;
  bodenWert = bodenSumme / 10;
  tankWert = tankSumme / 10;
  bodenSumme = tankSumme = 0;
  sensorProben = 0;
  sensorenBereit = true;
  if (tankWert < TANK_LEER) tankOK = false;
  else if (tankWert > TANK_WASSER) tankOK = true;
}

int bodenProzent() {
  return constrain(map(bodenWert, SOIL_TROCKEN, SOIL_NASS, 0, 100), 0, 100);
}
int tankProzent() {
  return constrain(map(tankWert, TANK_MIN, TANK_MAX, 0, 100), 0, 100);
}

void steuerungAktualisieren(uint32_t jetzt) {
  // Diese Pruefungen laufen vor Display und serieller Ausgabe.
  if (!oledOK) {
    pumpeStoppen(jetzt); aktuellerStatus = STATUS_OLED_FEHLER; return;
  }
  if (!sensorenBereit) return;
  if (!tankOK) {
    if (bewaesserungAktiv) zyklusBeenden(jetzt, STATUS_TANK_LEER, false);
    else pumpeStoppen(jetzt);
    aktuellerStatus = STATUS_TANK_LEER;
    return;
  }
  if (einstellungsMenue) {
    pumpeStoppen(jetzt); aktuellerStatus = STATUS_MENUE; return;
  }
  if (bewaesserungAktiv) {
    if (pumpeAN) {
      if (pumpVerbrauch(jetzt) >= MAX_PUMP_MS_PRO_STUNDE) {
        zyklusBeenden(jetzt, STATUS_LIMIT, false);
      } else if (vergangen(jetzt, pumpStart) >= pumpLimitMs) {
        pumpeStoppen(jetzt);
        warteStart = (uint32_t)millis();
        aktuellerStatus = STATUS_WARTEN;
      }
      return;
    }
    if (vergangen(jetzt, warteStart) < (uint32_t)WARTEZEIT) return;
    if (bodenWert <= SOIL_ZIEL) {
      zyklusBeenden(jetzt, STATUS_ZIEL, true);
    } else if (aktuelleRunde >= MAX_RUNDEN) {
      zyklusBeenden(jetzt, STATUS_MAX_RUNDEN, false);
    } else {
      pumpRundeStarten(jetzt);
    }
    return;
  }
  if (pauseRest(jetzt)) { aktuellerStatus = pausenGrund; return; }
  if (pumpRest(jetzt) < 1000 || historieZahl >= HISTORIE_ANZAHL) {
    aktuellerStatus = STATUS_LIMIT; return;
  }
  aktuellerStatus = STATUS_BEREIT;
  if (bodenWert >= SOIL_START) {
    bewaesserungAktiv = true;
    zyklusPumpMs = 0; aktuelleRunde = 0;
    statistik.giessVorgaenge++;
    pumpRundeStarten(jetzt);
  }
}

void einstellungErhoehen() {
  switch (einstellungsSeite) {
    case 0:
      SOIL_START += 25;
      if (SOIL_START > 1000) SOIL_START = SOIL_ZIEL + 25;
      if (SOIL_START > 1000) SOIL_START = 1000;
      break;
    case 1:
      SOIL_ZIEL += 25;
      if (SOIL_ZIEL >= SOIL_START) SOIL_ZIEL = 0;
      break;
    case 2:
      PUMP_DAUER += 1000;
      if (PUMP_DAUER > 30000) PUMP_DAUER = 1000;
      break;
    case 3:
      WARTEZEIT += 5000;
      if (WARTEZEIT > 60000) WARTEZEIT = 5000;
      break;
  }
}

void buttonAktualisieren(uint32_t jetzt) {
  bool wert = digitalRead(BUTTON_PIN);
  if (wert != letzterButtonWert) {
    letzterButtonWert = wert; buttonAenderung = jetzt;
  }
  if (vergangen(jetzt, buttonAenderung) < BUTTON_DEBOUNCE || wert == buttonStatus) return;
  buttonStatus = wert;
  if (wert == LOW) { buttonStart = jetzt; return; }
  bool lang = vergangen(jetzt, buttonStart) >= BUTTON_LANG;
  if (lang) {
    if (!einstellungsMenue) {
      if (pumpeAN) return;
      if (bewaesserungAktiv) zyklusBeenden(jetzt, STATUS_MENUE, false);
      einstellungsMenue = true; einstellungsSeite = 0;
    } else if (++einstellungsSeite >= 4) {
      einstellungenSpeichern(); einstellungsMenue = false; einstellungsSeite = 0;
    }
  } else if (einstellungsMenue) einstellungErhoehen();
  else menuSeite = (menuSeite + 1) % 3;
}

void statusAnzeigen() {
  switch (aktuellerStatus) {
    case STATUS_BEREIT: display.print(F("BEREIT")); break;
    case STATUS_TANK_LEER: display.print(F("TANK LEER!")); break;
    case STATUS_TROCKEN: display.print(F("ZU TROCKEN")); break;
    case STATUS_GIESSEN: display.print(F("GIESSEN ")); display.print(aktuelleRunde); display.print(F("/5")); break;
    case STATUS_WARTEN: display.print(F("EINZIEHEN LASSEN")); break;
    case STATUS_ZIEL: display.print(F("ZIEL / PAUSE")); break;
    case STATUS_MAX_RUNDEN: display.print(F("5 RUNDEN / SPERRE")); break;
    case STATUS_LIMIT: display.print(F("STUNDENLIMIT")); break;
    case STATUS_STARTPAUSE: display.print(F("STARTPAUSE")); break;
    case STATUS_MENUE: display.print(F("MENUE / PUMPE AUS")); break;
    case STATUS_OLED_FEHLER: display.print(F("OLED FEHLER")); break;
  }
}

void textCursor(int x, int y) { display.setCursor(x, y + 7); }

void balken(int y, int wert) {
  display.drawFrame(0, y, 90, 9);
  int breite = map(constrain(wert, 0, 100), 0, 100, 0, 88);
  if (breite) display.drawBox(1, y + 1, breite, 7);
  textCursor(96, y); display.print(wert); display.print(F("%"));
}

void oledAktualisieren(uint32_t jetzt) {
  if (!oledOK) return;
  if (!oledZeichnet) {
    if (vergangen(jetzt, oledLetztesBild) < 250) return;
    Wire.beginTransmission(0x3C);
    if (Wire.endTransmission() != 0) { oledOK = false; return; }
    oledLetztesBild = jetzt;
    display.firstPage();
    oledZeichnet = true;
  }
  display.setFont(u8g2_font_5x7_tf);
  display.setDrawColor(1);
  textCursor(0, 0);
  if (einstellungsMenue) {
    display.print(F("EINSTELLUNGEN ")); display.print(einstellungsSeite + 1); display.print(F("/4"));
    textCursor(0, 18);
    switch (einstellungsSeite) {
      case 0: display.print(F("Boden Start: ")); display.print(SOIL_START); break;
      case 1: display.print(F("Boden Ziel: ")); display.print(SOIL_ZIEL); break;
      case 2: display.print(F("Pumpen: ")); display.print(PUMP_DAUER / 1000); display.print(F(" s")); break;
      case 3: display.print(F("Warten: ")); display.print(WARTEZEIT / 1000); display.print(F(" s")); break;
    }
    textCursor(0, 36); display.print(F("Kurz = erhoehen"));
    textCursor(0, 52);
    display.print(einstellungsSeite == 3 ? F("Lang = speichern") : F("Lang = weiter"));
  } else if (menuSeite == 1) {
    display.println(F("STATISTIK"));
    textCursor(0, 15); display.print(F("Giessen: ")); display.print(statistik.giessVorgaenge);
    textCursor(0, 29); display.print(F("Runden: ")); display.print(statistik.pumpRunden);
    textCursor(0, 43); display.print(F("Pumpe: ")); display.print(statistik.pumpSekunden); display.print(F("s"));
    textCursor(0, 55); display.print(F("Rest/h: ")); display.print(pumpRest(jetzt) / 1000); display.print(F("s"));
  } else if (menuSeite == 2) {
    display.println(F("ROHWERTE"));
    textCursor(0, 15); display.print(F("Boden: ")); display.print(bodenWert);
    textCursor(0, 29); display.print(F("Tank: ")); display.print(tankWert);
    textCursor(0, 43); display.print(F("Start:")); display.print(SOIL_START); display.print(F(" Ziel:")); display.print(SOIL_ZIEL);
    textCursor(0, 55); display.print(F("Sperre: ")); display.print((pauseRest(jetzt) + 999) / 1000); display.print(F("s"));
  } else {
    display.print(F("BODENFEUCHTE")); balken(11, bodenProzent());
    textCursor(0, 25); display.print(F("TANKANZEIGE")); balken(36, tankProzent());
    display.drawLine(0, 49, 127, 49);
    textCursor(0, 55); statusAnzeigen();
  }
  // Pro loop nur eine Displayseite; dazwischen wird die Pumpe geprueft.
  oledZeichnet = display.nextPage();
}

void ledAktualisieren(uint32_t jetzt) {
  bool warnung = !tankOK || aktuellerStatus == STATUS_LIMIT ||
    aktuellerStatus == STATUS_MAX_RUNDEN || aktuellerStatus == STATUS_OLED_FEHLER;
  if (!warnung) { digitalWrite(LED_PIN, LOW); ledStatus = false; return; }
  if (vergangen(jetzt, letzteLEDZeit) >= 500) {
    letzteLEDZeit = jetzt; ledStatus = !ledStatus; digitalWrite(LED_PIN, ledStatus);
  }
}

void seriellAktualisieren(uint32_t jetzt) {
  if (!sensorenBereit) return;
  if (serialPosition < serialLaenge) {
    int platz = Serial.availableForWrite();
    uint8_t rest = serialLaenge - serialPosition;
    uint8_t anzahl = (uint8_t)min(platz, (int)rest);
    if (anzahl) {
      Serial.write((const uint8_t*)serialPuffer + serialPosition, anzahl);
      serialPosition += anzahl;
    }
    return;
  }
  if (serialPhase >= 4) {
    if (vergangen(jetzt, letzteSerialGruppe) < SERIAL_INTERVALL_MS) return;
    letzteSerialGruppe = jetzt; serialPhase = 0;
  }
  int laenge = 0;
  switch (serialPhase++) {
    case 0:
      // Diese Zeile bleibt mit dem bereits laufenden Pi-Programm kompatibel.
      laenge = snprintf_P(serialPuffer, sizeof(serialPuffer),
        PSTR("Boden: %d (%d%%) | Tank: %d (%d%%) | Wasser: %S | Pumpe: %S\n"),
        bodenWert, bodenProzent(), tankWert, tankProzent(),
        tankOK ? PSTR("JA") : PSTR("NEIN"), pumpeAN ? PSTR("AN") : PSTR("AUS"));
      break;
    case 1:
      laenge = snprintf_P(serialPuffer, sizeof(serialPuffer),
        PSTR("STATUS code=%u runde=%u max=%u sperre_s=%lu budget_ms=%lu uptime_ms=%lu\n"),
        (unsigned)aktuellerStatus, (unsigned)aktuelleRunde, (unsigned)MAX_RUNDEN,
        (unsigned long)((pauseRest(jetzt) + 999) / 1000),
        (unsigned long)pumpRest(jetzt), (unsigned long)jetzt);
      break;
    case 2:
      laenge = snprintf_P(serialPuffer, sizeof(serialPuffer),
        PSTR("STATISTIK giessen=%lu runden=%lu sekunden=%lu\n"),
        (unsigned long)statistik.giessVorgaenge, (unsigned long)statistik.pumpRunden,
        (unsigned long)statistik.pumpSekunden);
      break;
    case 3:
      laenge = snprintf_P(serialPuffer, sizeof(serialPuffer),
        PSTR("KONFIG start=%d ziel=%d pump_ms=%lu warte_ms=%lu menue=%u\n"),
        SOIL_START, SOIL_ZIEL, PUMP_DAUER, WARTEZEIT, (unsigned)einstellungsMenue);
      break;
  }
  serialPosition = 0;
  serialLaenge = laenge > 0 && laenge < (int)sizeof(serialPuffer) ? (uint8_t)laenge : 0;
}

void setup() {
  digitalWrite(RELAY_PIN, RELAY_AUS);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(9600);
  eepromLaden();
  uint32_t jetzt = (uint32_t)millis();
  budgetLaden(jetzt);
  pauseSetzen(jetzt, (uint32_t)sicherheitsPauseMin * 60000UL, STATUS_STARTPAUSE);
  Wire.begin();
#ifdef WIRE_HAS_TIMEOUT
  Wire.setWireTimeout(25000, true);
#endif
  Wire.beginTransmission(0x3C);
  oledOK = Wire.endTransmission() == 0;
  if (oledOK) {
    display.setI2CAddress(0x3C * 2);
    display.begin();
#ifdef WIRE_HAS_TIMEOUT
    Wire.setWireTimeout(25000, true);
#endif
  }
  if (!oledOK) aktuellerStatus = STATUS_OLED_FEHLER;
}

void loop() {
  steuerungAktualisieren((uint32_t)millis());
  sensorenAktualisieren((uint32_t)millis());
  buttonAktualisieren((uint32_t)millis());
  steuerungAktualisieren((uint32_t)millis());
  ledAktualisieren((uint32_t)millis());
  seriellAktualisieren((uint32_t)millis());
  oledAktualisieren((uint32_t)millis());
  steuerungAktualisieren((uint32_t)millis());
}
