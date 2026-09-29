#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// =====================================================
// ARDUINO BEWAESSERUNGSSYSTEM
// =====================================================
//
// PINBELEGUNG:
//
// Tanksensor     -> A0
// Bodensensor    -> A1
// Menue-Taster   -> D6
// Warn-LED       -> D7
// Relais/Pumpe   -> D8
//
// OLED SH1106:
// SDA -> A4
// SCL -> A5
// Adresse: 0x3C
//
// WICHTIG:
// Das Relais ist LOW-AKTIV.
//
// LOW  = Pumpe AN
// HIGH = Pumpe AUS
//
// =====================================================


// =====================================================
// PINS
// =====================================================

const byte TANK_PIN   = A0;
const byte SOIL_PIN   = A1;

const byte BUTTON_PIN = 6;
const byte LED_PIN    = 7;
const byte RELAY_PIN  = 8;


// =====================================================
// RELAIS-LOGIK
// =====================================================

const byte RELAY_AN  = LOW;
const byte RELAY_AUS = HIGH;


// =====================================================
// OLED
// =====================================================

Adafruit_SH1106G display(128, 64, &Wire);


// =====================================================
// BODEN-KALIBRIERUNG
// =====================================================
//
// ca. 1020 = knochentrocken
// ca. 700  = Bewaesserung starten
// ca. 350  = Ziel erreicht
//
// Die Werte fuer Start und Ziel
// koennen ueber das Menue veraendert werden.
//

const int SOIL_TROCKEN = 1020;
const int SOIL_NASS    = 350;

int SOIL_START = 700;
int SOIL_ZIEL  = 350;


// =====================================================
// TANK-KALIBRIERUNG
// =====================================================

const int TANK_MIN = 0;
const int TANK_MAX = 500;

const int TANK_LEER   = 100;
const int TANK_WASSER = 200;


// =====================================================
// BEWAESSERUNG
// =====================================================

// Einstellbar ueber das Menue

unsigned long PUMP_DAUER = 5000;

unsigned long WARTEZEIT = 20000;


// Maximal 5 Pumpenrunden pro Bewaesserung

const byte MAX_RUNDEN = 5;


// =====================================================
// SYSTEMSTATUS
// =====================================================

enum SystemStatus {

  STATUS_BEREIT,
  STATUS_TANK_LEER,
  STATUS_TROCKEN,
  STATUS_GIESSEN,
  STATUS_WARTEN,
  STATUS_ZIEL,
  STATUS_MAX_RUNDEN
};

SystemStatus aktuellerStatus = STATUS_BEREIT;


// =====================================================
// SENSORWERTE
// =====================================================

int bodenWert = 0;
int tankWert  = 0;

bool tankOK  = false;
bool pumpeAN = false;


// =====================================================
// WARN-LED
// =====================================================

bool ledStatus = false;

unsigned long letzterLEDWechsel = 0;


// =====================================================
// OLED MENUE
// =====================================================
//
// 0 = Uebersicht
// 1 = Statistik
// 2 = Rohwerte
//

byte menuSeite = 0;

const byte MENU_SEITEN = 3;


// =====================================================
// EINSTELLUNGSMENUE
// =====================================================
//
// 0 = Boden Start
// 1 = Boden Ziel
// 2 = Pumpendauer
// 3 = Wartezeit
//

bool einstellungsMenue = false;

byte einstellungsSeite = 0;

const byte EINSTELLUNGEN_ANZAHL = 4;

const unsigned long BUTTON_LANG = 1500;


// =====================================================
// TASTER
// =====================================================

bool letzterButtonWert = HIGH;
bool buttonStatus      = HIGH;

unsigned long letzteButtonAenderung = 0;

const unsigned long BUTTON_DEBOUNCE = 40;


// Fuer Kurz-/Langdruck

bool buttonGedrueckt = false;

unsigned long buttonStartZeit = 0;


// =====================================================
// STATISTIK / EEPROM
// =====================================================

const uint16_t EEPROM_MAGIC = 0xBEEF;


struct Statistik {

  uint16_t magic;

  uint32_t giessVorgaenge;

  uint32_t pumpRunden;

  uint32_t pumpSekunden;
};


Statistik statistik;


// =====================================================
// EINSTELLUNGEN / EEPROM
// =====================================================

const uint16_t EINSTELLUNGEN_MAGIC = 0xCAFE;


struct Einstellungen {

  uint16_t magic;

  int soilStart;

  int soilZiel;

  unsigned long pumpDauer;

  unsigned long wartezeit;
};


// Die Einstellungen beginnen direkt nach der Statistik

const int EEPROM_EINSTELLUNGEN_ADRESSE = sizeof(Statistik);


Einstellungen einstellungen;


// =====================================================
// FUNKTIONSDEKLARATION
// =====================================================

void oledAktualisieren();

void einstellungenAnzeigen();

void einstellungErhoehen();

void einstellungenSpeichern();

void einstellungenLaden();


// =====================================================
// STATISTIK LADEN
// =====================================================

void statistikLaden() {

  EEPROM.get(0, statistik);


  if (statistik.magic != EEPROM_MAGIC) {

    statistik.magic = EEPROM_MAGIC;

    statistik.giessVorgaenge = 0;

    statistik.pumpRunden = 0;

    statistik.pumpSekunden = 0;


    EEPROM.put(0, statistik);
  }
}


// =====================================================
// STATISTIK SPEICHERN
// =====================================================

void statistikSpeichern() {

  EEPROM.put(0, statistik);
}


// =====================================================
// EINSTELLUNGEN LADEN
// =====================================================

void einstellungenLaden() {

  EEPROM.get(
    EEPROM_EINSTELLUNGEN_ADRESSE,
    einstellungen
  );


  // ---------------------------------------------------
  // Pruefen, ob gueltige Einstellungen vorhanden sind
  // ---------------------------------------------------

  if (
    einstellungen.magic != EINSTELLUNGEN_MAGIC ||

    einstellungen.soilStart < 25 ||
    einstellungen.soilStart > 1000 ||

    einstellungen.soilZiel < 0 ||
    einstellungen.soilZiel >= einstellungen.soilStart ||

    einstellungen.pumpDauer < 1000 ||
    einstellungen.pumpDauer > 30000 ||

    einstellungen.wartezeit < 5000 ||
    einstellungen.wartezeit > 60000
  ) {

    einstellungen.magic = EINSTELLUNGEN_MAGIC;

    einstellungen.soilStart = 700;

    einstellungen.soilZiel = 350;

    einstellungen.pumpDauer = 5000;

    einstellungen.wartezeit = 20000;


    einstellungenSpeichern();
  }


  // ---------------------------------------------------
  // Werte ins Programm uebernehmen
  // ---------------------------------------------------

  SOIL_START = einstellungen.soilStart;

  SOIL_ZIEL = einstellungen.soilZiel;

  PUMP_DAUER = einstellungen.pumpDauer;

  WARTEZEIT = einstellungen.wartezeit;
}


// =====================================================
// EINSTELLUNGEN SPEICHERN
// =====================================================

void einstellungenSpeichern() {

  einstellungen.magic = EINSTELLUNGEN_MAGIC;

  einstellungen.soilStart = SOIL_START;

  einstellungen.soilZiel = SOIL_ZIEL;

  einstellungen.pumpDauer = PUMP_DAUER;

  einstellungen.wartezeit = WARTEZEIT;


  EEPROM.put(
    EEPROM_EINSTELLUNGEN_ADRESSE,
    einstellungen
  );
}


// =====================================================
// ANALOGWERT MITTELN
// =====================================================

int mittelwert(byte pin) {

  long summe = 0;


  for (byte i = 0; i < 10; i++) {

    summe += analogRead(pin);

    delay(20);
  }


  return summe / 10;
}


// =====================================================
// BODEN MESSEN
// =====================================================

void bodenMessen() {

  bodenWert = mittelwert(SOIL_PIN);
}


// =====================================================
// TANK MESSEN
// =====================================================

void tankMessen() {

  tankWert = mittelwert(TANK_PIN);


  if (tankWert < TANK_LEER) {

    tankOK = false;
  }


  else if (tankWert > TANK_WASSER) {

    tankOK = true;
  }

  // Zwischen 100 und 200:
  // vorherigen Zustand behalten
}


// =====================================================
// ALLE SENSOREN
// =====================================================

void allesMessen() {

  bodenMessen();

  tankMessen();
}


// =====================================================
// BODEN IN PROZENT
// =====================================================

int bodenProzent() {

  int prozent = map(
    bodenWert,
    SOIL_TROCKEN,
    SOIL_NASS,
    0,
    100
  );


  return constrain(prozent, 0, 100);
}


// =====================================================
// TANK IN PROZENT
// =====================================================

int tankProzent() {

  int prozent = map(
    tankWert,
    TANK_MIN,
    TANK_MAX,
    0,
    100
  );


  return constrain(prozent, 0, 100);
}


// =====================================================
// PUMPE EIN
// =====================================================

void pumpeEin() {

  digitalWrite(RELAY_PIN, RELAY_AN);

  pumpeAN = true;
}


// =====================================================
// PUMPE AUS
// =====================================================

void pumpeAus() {

  digitalWrite(RELAY_PIN, RELAY_AUS);

  pumpeAN = false;
}


// =====================================================
// WARN-LED
// =====================================================

void ledAktualisieren() {

  if (tankOK) {

    digitalWrite(LED_PIN, LOW);

    ledStatus = false;

    return;
  }


  if (millis() - letzterLEDWechsel >= 500) {

    letzterLEDWechsel = millis();

    ledStatus = !ledStatus;

    digitalWrite(LED_PIN, ledStatus);
  }
}


// =====================================================
// STATUS AUSGEBEN
// =====================================================

void statusAusgeben() {

  switch (aktuellerStatus) {

    case STATUS_BEREIT:

      display.print(F("BEREIT"));

      break;


    case STATUS_TANK_LEER:

      display.print(F("TANK LEER!"));

      break;


    case STATUS_TROCKEN:

      display.print(F("ZU TROCKEN"));

      break;


    case STATUS_GIESSEN:

      display.print(F("GIESSEN..."));

      break;


    case STATUS_WARTEN:

      display.print(F("WARTEN..."));

      break;


    case STATUS_ZIEL:

      display.print(F("ZIEL ERREICHT"));

      break;


    case STATUS_MAX_RUNDEN:

      display.print(F("MAX. RUNDEN"));

      break;
  }
}


// =====================================================
// BALKENDIAGRAMM
// =====================================================
//
// Der Balken ist ein durchgehender Balken.
//
// 0 %:
// [--------------------]
//
// 50 %:
// [##########----------]
//
// 100 %:
// [####################]
//
// Der Prozentwert steht ausserhalb des Balkens.
//

void balkenZeichnen(
  int x,
  int y,
  int breite,
  int hoehe,
  int prozent
) {

  // ---------------------------------------------------
  // Begrenzung
  // ---------------------------------------------------

  prozent = constrain(prozent, 0, 100);


  // ---------------------------------------------------
  // Rahmen
  // ---------------------------------------------------

  display.drawRect(
    x,
    y,
    breite,
    hoehe,
    SH110X_WHITE
  );


  // ---------------------------------------------------
  // Fuellung berechnen
  // ---------------------------------------------------

  int fuellBreite = map(
    prozent,
    0,
    100,
    0,
    breite - 2
  );


  // ---------------------------------------------------
  // Balken fuellen
  // ---------------------------------------------------

  if (fuellBreite > 0) {

    display.fillRect(
      x + 1,
      y + 1,
      fuellBreite,
      hoehe - 2,
      SH110X_WHITE
    );
  }
}


// =====================================================
// OLED SEITE 1
// UEBERSICHT
// =====================================================

void oledUebersicht() {

  int boden = bodenProzent();

  int tank = tankProzent();


  // ===================================================
  // BODEN
  // ===================================================

  display.setCursor(0, 0);

  display.println(F("BODENFEUCHTE"));


  // Balken

  balkenZeichnen(
    0,
    11,
    102,
    9,
    boden
  );


  // Prozent rechts neben dem Balken

  display.setCursor(106, 11);

  display.print(boden);

  display.print(F("%"));


  // ===================================================
  // TANK
  // ===================================================

  display.setCursor(0, 25);

  display.println(F("TANK"));


  // Balken

  balkenZeichnen(
    0,
    36,
    102,
    9,
    tank
  );


  // Prozent rechts neben dem Balken

  display.setCursor(106, 36);

  display.print(tank);

  display.print(F("%"));


  // ===================================================
  // TRENNLINIE
  // ===================================================

  display.drawLine(
    0,
    49,
    127,
    49,
    SH110X_WHITE
  );


  // ===================================================
  // STATUS
  // ===================================================

  display.setCursor(0, 55);

  statusAusgeben();
}


// =====================================================
// OLED SEITE 2
// STATISTIK
// =====================================================

void oledStatistik() {

  display.setCursor(0, 0);

  display.println(F("STATISTIK"));


  display.setCursor(0, 15);

  display.print(F("Giessen: "));

  display.println(statistik.giessVorgaenge);


  display.setCursor(0, 29);

  display.print(F("Pumpen:  "));

  display.println(statistik.pumpRunden);


  display.setCursor(0, 43);

  display.print(F("Laufzeit: "));

  display.print(statistik.pumpSekunden);

  display.println(F("s"));
}


// =====================================================
// OLED SEITE 3
// ROHWERTE
// =====================================================

void oledRohwerte() {

  display.setCursor(0, 0);

  display.println(F("ROHWERTE"));


  display.setCursor(0, 15);

  display.print(F("Boden: "));

  display.println(bodenWert);


  display.setCursor(0, 29);

  display.print(F("Tank:  "));

  display.println(tankWert);


  display.setCursor(0, 43);

  display.print(F("Start:"));

  display.print(SOIL_START);

  display.print(F(" Ziel:"));

  display.println(SOIL_ZIEL);
}


// =====================================================
// OLED EINSTELLUNGEN
// =====================================================

void einstellungenAnzeigen() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setTextColor(SH110X_WHITE);


  display.setCursor(0, 0);

  display.print(F("EINSTELLUNGEN "));

  display.print(einstellungsSeite + 1);

  display.print(F("/4"));


  display.setCursor(0, 15);


  switch (einstellungsSeite) {

    // -------------------------------------------------
    // Boden Start
    // -------------------------------------------------

    case 0:

      display.println(F("Boden Start"));

      display.setCursor(0, 31);

      display.print(F("Wert: "));

      display.println(SOIL_START);

      display.setCursor(0, 47);

      display.print(F("+25"));

      display.setCursor(75, 47);

      display.print(F("Lang = weiter"));

      break;


    // -------------------------------------------------
    // Boden Ziel
    // -------------------------------------------------

    case 1:

      display.println(F("Boden Ziel"));

      display.setCursor(0, 31);

      display.print(F("Wert: "));

      display.println(SOIL_ZIEL);

      display.setCursor(0, 47);

      display.print(F("+25"));

      display.setCursor(75, 47);

      display.print(F("Lang = weiter"));

      break;


    // -------------------------------------------------
    // Pumpendauer
    // -------------------------------------------------

    case 2:

      display.println(F("Pumpendauer"));

      display.setCursor(0, 31);

      display.print(F("Wert: "));

      display.print(PUMP_DAUER / 1000);

      display.println(F(" s"));

      display.setCursor(0, 47);

      display.print(F("+1 s"));

      display.setCursor(75, 47);

      display.print(F("Lang = weiter"));

      break;


    // -------------------------------------------------
    // Wartezeit
    // -------------------------------------------------

    case 3:

      display.println(F("Wartezeit"));

      display.setCursor(0, 31);

      display.print(F("Wert: "));

      display.print(WARTEZEIT / 1000);

      display.println(F(" s"));

      display.setCursor(0, 47);

      display.print(F("+5 s"));

      display.setCursor(75, 47);

      display.print(F("Lang = speichern"));

      break;
  }


  display.display();
}


// =====================================================
// OLED AKTUALISIEREN
// =====================================================

void oledAktualisieren() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setTextColor(SH110X_WHITE);


  if (einstellungsMenue) {

    einstellungenAnzeigen();

    return;
  }


  switch (menuSeite) {

    case 0:

      oledUebersicht();

      break;


    case 1:

      oledStatistik();

      break;


    case 2:

      oledRohwerte();

      break;
  }


  display.display();
}


// =====================================================
// EINSTELLUNG ERHOEHEN
// =====================================================

void einstellungErhoehen() {

  switch (einstellungsSeite) {

    // -------------------------------------------------
    // Boden Start
    // -------------------------------------------------

    case 0:

      SOIL_START += 25;


      if (SOIL_START > 1000) {

        SOIL_START = 25;
      }


      if (SOIL_START <= SOIL_ZIEL) {

        SOIL_START = SOIL_ZIEL + 25;
      }


      if (SOIL_START > 1000) {

        SOIL_START = 1000;
      }

      break;


    // -------------------------------------------------
    // Boden Ziel
    // -------------------------------------------------

    case 1:

      SOIL_ZIEL += 25;


      if (SOIL_ZIEL > 975) {

        SOIL_ZIEL = 0;
      }


      if (SOIL_ZIEL >= SOIL_START) {

        SOIL_ZIEL = SOIL_START - 25;
      }


      if (SOIL_ZIEL < 0) {

        SOIL_ZIEL = 0;
      }

      break;


    // -------------------------------------------------
    // Pumpendauer
    // -------------------------------------------------

    case 2:

      PUMP_DAUER += 1000;


      if (PUMP_DAUER > 30000) {

        PUMP_DAUER = 1000;
      }

      break;


    // -------------------------------------------------
    // Wartezeit
    // -------------------------------------------------

    case 3:

      WARTEZEIT += 5000;


      if (WARTEZEIT > 60000) {

        WARTEZEIT = 5000;
      }

      break;
  }


  einstellungenAnzeigen();
}


// =====================================================
// TASTER
// =====================================================

void buttonAktualisieren() {

  bool aktuellerWert = digitalRead(BUTTON_PIN);


  // ---------------------------------------------------
  // Aenderung erkannt
  // ---------------------------------------------------

  if (aktuellerWert != letzterButtonWert) {

    letzteButtonAenderung = millis();

    letzterButtonWert = aktuellerWert;
  }


  // ---------------------------------------------------
  // Entprellzeit abgelaufen
  // ---------------------------------------------------

  if (
    millis() - letzteButtonAenderung >
    BUTTON_DEBOUNCE
  ) {

    if (aktuellerWert != buttonStatus) {

      buttonStatus = aktuellerWert;


      // ===============================================
      // TASTER GEDRUECKT
      // ===============================================

      if (buttonStatus == LOW) {

        buttonGedrueckt = true;

        buttonStartZeit = millis();
      }


      // ===============================================
      // TASTER LOSGELASSEN
      // ===============================================

      else {

        if (buttonGedrueckt) {

          buttonGedrueckt = false;


          unsigned long drueckDauer =
            millis() - buttonStartZeit;


          // ===========================================
          // LANGDRUCK
          // ===========================================

          if (drueckDauer >= BUTTON_LANG) {

            // -----------------------------------------
            // Normalmodus
            // -----------------------------------------

            if (!einstellungsMenue) {

              if (!pumpeAN) {

                einstellungsMenue = true;

                einstellungsSeite = 0;

                einstellungenAnzeigen();
              }
            }


            // -----------------------------------------
            // Einstellungsmenue
            // -----------------------------------------

            else {

              einstellungsSeite++;


              if (
                einstellungsSeite >=
                EINSTELLUNGEN_ANZAHL
              ) {

                einstellungenSpeichern();

                einstellungsMenue = false;

                einstellungsSeite = 0;


                oledAktualisieren();
              }

              else {

                einstellungenAnzeigen();
              }
            }
          }


          // ===========================================
          // KURZDRUCK
          // ===========================================

          else {

            // -----------------------------------------
            // Einstellungsmenue
            // -----------------------------------------

            if (einstellungsMenue) {

              einstellungErhoehen();
            }


            // -----------------------------------------
            // Normalmodus
            // -----------------------------------------

            else {

              menuSeite++;


              if (menuSeite >= MENU_SEITEN) {

                menuSeite = 0;
              }


              oledAktualisieren();
            }
          }
        }
      }
    }
  }
}


// =====================================================
// WARTEFUNKTION
// =====================================================

void wartenMitBedienung(unsigned long dauer) {

  unsigned long start = millis();


  while (millis() - start < dauer) {

    buttonAktualisieren();

    ledAktualisieren();

    delay(5);
  }
}


// =====================================================
// SERIELLE AUSGABE
// =====================================================

void seriell() {

  Serial.print(F("Boden: "));

  Serial.print(bodenWert);

  Serial.print(F(" ("));

  Serial.print(bodenProzent());

  Serial.print(F("%)"));


  Serial.print(F(" | Tank: "));

  Serial.print(tankWert);

  Serial.print(F(" ("));

  Serial.print(tankProzent());

  Serial.print(F("%)"));


  Serial.print(F(" | Wasser: "));


  if (tankOK) {

    Serial.print(F("JA"));
  }

  else {

    Serial.print(F("NEIN"));
  }


  Serial.print(F(" | Pumpe: "));


  if (pumpeAN) {

    Serial.println(F("AN"));
  }

  else {

    Serial.println(F("AUS"));
  }
}


// =====================================================
// EINE PUMPENRUNDE
// =====================================================

bool giessen() {

  // ---------------------------------------------------
  // Sicherheitscheck
  // ---------------------------------------------------

  tankMessen();


  if (!tankOK) {

    pumpeAus();

    aktuellerStatus = STATUS_TANK_LEER;

    oledAktualisieren();


    Serial.println(F("Tank leer -> Pumpe gesperrt"));


    return false;
  }


  // ---------------------------------------------------
  // Pumpe starten
  // ---------------------------------------------------

  Serial.println();

  Serial.println(F(">>> Pumpe AN"));


  aktuellerStatus = STATUS_GIESSEN;


  pumpeEin();

  oledAktualisieren();


  unsigned long start = millis();

  unsigned long letzteTankMessung = 0;


  // ---------------------------------------------------
  // Pumpen
  // ---------------------------------------------------

  while (millis() - start < PUMP_DAUER) {

    buttonAktualisieren();

    ledAktualisieren();


    // Tank alle 250 ms kontrollieren

    if (millis() - letzteTankMessung >= 250) {

      letzteTankMessung = millis();


      tankMessen();


      oledAktualisieren();


      // Tank waehrend des Pumpens leer geworden

      if (!tankOK) {

        unsigned long laufzeit =
          millis() - start;


        pumpeAus();


        statistik.pumpRunden++;

        statistik.pumpSekunden +=
          (laufzeit + 999) / 1000;


        aktuellerStatus = STATUS_TANK_LEER;


        Serial.println(F("TANK LEER"));

        Serial.println(F("Pumpe sofort gestoppt."));


        oledAktualisieren();


        return false;
      }
    }
  }


  // ---------------------------------------------------
  // Pumpe stoppen
  // ---------------------------------------------------

  pumpeAus();


  statistik.pumpRunden++;

  statistik.pumpSekunden +=
    PUMP_DAUER / 1000;


  Serial.println(F(">>> Pumpe AUS"));


  return true;
}


// =====================================================
// NACH DEM GIESSEN WARTEN
// =====================================================

bool wartenNachGiessen() {

  aktuellerStatus = STATUS_WARTEN;


  oledAktualisieren();


  Serial.println(F("Warten..."));


  unsigned long start = millis();

  unsigned long letzteMessung = 0;


  while (millis() - start < WARTEZEIT) {

    buttonAktualisieren();

    ledAktualisieren();


    // jede Sekunde neu messen

    if (millis() - letzteMessung >= 1000) {

      letzteMessung = millis();


      allesMessen();


      seriell();

      oledAktualisieren();


      // Tank inzwischen leer

      if (!tankOK) {

        pumpeAus();


        aktuellerStatus = STATUS_TANK_LEER;


        oledAktualisieren();


        return false;
      }
    }
  }


  return true;
}


// =====================================================
// KOMPLETTE BEWAESSERUNG
// =====================================================

void bewaessern() {

  Serial.println();

  Serial.println(F("=========================="));

  Serial.println(F("BEWAESSERUNG GESTARTET"));

  Serial.println(F("=========================="));


  bool giessVorgangGezaehlt = false;


  for (
    byte runde = 1;
    runde <= MAX_RUNDEN;
    runde++
  ) {

    allesMessen();


    Serial.print(F("Runde "));

    Serial.print(runde);

    Serial.print(F("/"));

    Serial.println(MAX_RUNDEN);


    seriell();


    // -------------------------------------------------
    // Boden bereits feucht genug
    // -------------------------------------------------

    if (bodenWert <= SOIL_ZIEL) {

      pumpeAus();


      aktuellerStatus = STATUS_ZIEL;


      oledAktualisieren();


      if (giessVorgangGezaehlt) {

        statistikSpeichern();
      }


      return;
    }


    // -------------------------------------------------
    // Tank leer
    // -------------------------------------------------

    if (!tankOK) {

      pumpeAus();


      aktuellerStatus = STATUS_TANK_LEER;


      oledAktualisieren();


      if (giessVorgangGezaehlt) {

        statistikSpeichern();
      }


      return;
    }


    // -------------------------------------------------
    // Erster Pumpvorgang
    // -------------------------------------------------

    if (!giessVorgangGezaehlt) {

      statistik.giessVorgaenge++;

      giessVorgangGezaehlt = true;
    }


    // -------------------------------------------------
    // Giessen
    // -------------------------------------------------

    if (!giessen()) {

      statistikSpeichern();

      return;
    }


    // -------------------------------------------------
    // Warten
    // -------------------------------------------------

    if (!wartenNachGiessen()) {

      statistikSpeichern();

      return;
    }


    // -------------------------------------------------
    // erneut messen
    // -------------------------------------------------

    allesMessen();


    Serial.print(F("Nach Giessen: "));

    Serial.print(bodenWert);

    Serial.print(F(" / "));

    Serial.print(bodenProzent());

    Serial.println(F("%"));


    // -------------------------------------------------
    // Ziel erreicht
    // -------------------------------------------------

    if (bodenWert <= SOIL_ZIEL) {

      aktuellerStatus = STATUS_ZIEL;


      Serial.println(F("ZIEL ERREICHT"));


      oledAktualisieren();


      statistikSpeichern();


      return;
    }


    Serial.println(F("Noch zu trocken."));
  }


  // ===================================================
  // MAXIMALE RUNDEN
  // ===================================================

  pumpeAus();


  aktuellerStatus = STATUS_MAX_RUNDEN;


  oledAktualisieren();


  statistikSpeichern();


  Serial.println(F("MAXIMALE RUNDEN ERREICHT"));
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(9600);


  // ===================================================
  // RELAIS SICHER AUSSCHALTEN
  // ===================================================

  digitalWrite(RELAY_PIN, RELAY_AUS);

  pinMode(RELAY_PIN, OUTPUT);


  pumpeAN = false;


  // ---------------------------------------------------
  // andere Pins
  // ---------------------------------------------------

  pinMode(LED_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);


  digitalWrite(LED_PIN, LOW);


  // ---------------------------------------------------
  // Statistik laden
  // ---------------------------------------------------

  statistikLaden();


  // ---------------------------------------------------
  // Einstellungen laden
  // ---------------------------------------------------

  einstellungenLaden();


  // ---------------------------------------------------
  // I2C
  // ---------------------------------------------------

  Wire.begin();

  delay(500);


  Serial.println(F("OLED starten..."));


  // ---------------------------------------------------
  // OLED starten
  // ---------------------------------------------------

  if (!display.begin(0x3C, true)) {

    Serial.println(F("OLED FEHLER"));


    pumpeAus();


    while (true) {

      delay(1000);
    }
  }


  Serial.println(F("OLED OK"));


  // ---------------------------------------------------
  // Startanzeige
  // ---------------------------------------------------

  display.clearDisplay();

  display.setTextColor(SH110X_WHITE);

  display.setTextSize(2);

  display.setCursor(25, 20);

  display.println(F("START"));

  display.display();


  delay(1500);


  // ---------------------------------------------------
  // Erste Sensorwerte
  // ---------------------------------------------------

  allesMessen();


  if (!tankOK) {

    aktuellerStatus = STATUS_TANK_LEER;
  }

  else {

    aktuellerStatus = STATUS_BEREIT;
  }


  oledAktualisieren();


  // ---------------------------------------------------
  // Statistik seriell
  // ---------------------------------------------------

  Serial.println();

  Serial.println(F("STATISTIK"));


  Serial.print(F("Giessvorgaenge: "));

  Serial.println(statistik.giessVorgaenge);


  Serial.print(F("Pumpenrunden: "));

  Serial.println(statistik.pumpRunden);


  Serial.print(F("Pumpenlaufzeit: "));

  Serial.print(statistik.pumpSekunden);

  Serial.println(F(" Sekunden"));


  // ---------------------------------------------------
  // Einstellungen seriell
  // ---------------------------------------------------

  Serial.println();

  Serial.println(F("EINSTELLUNGEN"));


  Serial.print(F("Boden Start: "));

  Serial.println(SOIL_START);


  Serial.print(F("Boden Ziel: "));

  Serial.println(SOIL_ZIEL);


  Serial.print(F("Pumpendauer: "));

  Serial.print(PUMP_DAUER / 1000);

  Serial.println(F(" Sekunden"));


  Serial.print(F("Wartezeit: "));

  Serial.print(WARTEZEIT / 1000);

  Serial.println(F(" Sekunden"));
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  // ---------------------------------------------------
  // Einstellungsmenue
  // ---------------------------------------------------

  if (einstellungsMenue) {

    pumpeAus();

    buttonAktualisieren();

    ledAktualisieren();

    delay(5);

    return;
  }


  // ---------------------------------------------------
  // Sensoren messen
  // ---------------------------------------------------

  allesMessen();


  // ---------------------------------------------------
  // Taster und LED
  // ---------------------------------------------------

  buttonAktualisieren();

  ledAktualisieren();


  // ---------------------------------------------------
  // Serial
  // ---------------------------------------------------

  seriell();


  // ===================================================
  // TANK LEER
  // ===================================================

  if (!tankOK) {

    pumpeAus();


    aktuellerStatus = STATUS_TANK_LEER;


    oledAktualisieren();


    wartenMitBedienung(500);


    return;
  }


  // ===================================================
  // BODEN ZU TROCKEN
  // ===================================================

  if (bodenWert >= SOIL_START) {

    aktuellerStatus = STATUS_TROCKEN;


    oledAktualisieren();


    Serial.println(F("Boden zu trocken!"));

    Serial.println(F("Bewässerung startet."));


    wartenMitBedienung(1000);


    bewaessern();


    allesMessen();


    wartenMitBedienung(2000);


    return;
  }


  // ===================================================
  // NORMALZUSTAND
  // ===================================================

  pumpeAus();


  aktuellerStatus = STATUS_BEREIT;


  oledAktualisieren();


  wartenMitBedienung(1000);
}
