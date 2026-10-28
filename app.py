#!/usr/bin/env python3
"""Read-only Arduino monitoring. Pump control stays on the Arduino."""
import glob
import json
import logging
import os
from pathlib import Path
import re
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlencode
from urllib.request import urlopen, Request
import serial

ROOT = Path(__file__).resolve().parent
DB = ROOT / 'messwerte.sqlite3'
LOCK = threading.Lock()
STATE = {'measurement': None, 'connected': False, 'pump': None, 'status': 'Warte auf Arduino', 'last_line_at': None}
PATTERN = re.compile(r'Boden: (\d+) \((\d+)%\) \| Tank: (\d+) \((\d+)%\) \| Wasser: (JA|NEIN) \| Pumpe: (AN|AUS)')
TELEGRAM_INTERVAL_SECONDS = 300

def parse(line):
    m = PATTERN.fullmatch(line.strip())
    if not m:
        return None
    soil, soil_pct, tank, tank_pct = map(int, m.groups()[:4])
    if not (0 <= soil <= 1023 and 0 <= tank <= 1023 and 0 <= soil_pct <= 100 and 0 <= tank_pct <= 100):
        return None
    return dict(soil_raw=soil, soil_pct=soil_pct, tank_raw=tank, tank_pct=tank_pct, water_ok=m[5] == 'JA', pump=m[6] == 'AN')

def connection():
    return sqlite3.connect(DB, timeout=10)

def initialize():
    with connection() as db:
        db.execute('PRAGMA journal_mode=WAL')
        db.execute('CREATE TABLE IF NOT EXISTS samples (ts REAL PRIMARY KEY, data TEXT NOT NULL)')

def snapshot():
    with LOCK:
        result = dict(STATE)
    m = result['measurement']
    result['age_seconds'] = round(time.time() - m['ts'], 1) if m else None
    result['stale'] = not m or result['age_seconds'] > 45 or not result['connected']
    result['telegram_configured'] = (ROOT / 'telegram.json').exists()
    return result

def history():
    with connection() as db:
        rows = db.execute('SELECT data FROM samples WHERE ts > ? ORDER BY ts', (time.time()-86400,)).fetchall()
    values = [json.loads(r[0]) for r in rows]
    return values[::max(1, len(values)//500)]

def reader():
    last_saved = 0
    while True:
        try:
            ports = sorted(glob.glob('/dev/serial/by-id/*Arduino*'))
            port = os.environ.get('SERIAL_PORT') or (ports[0] if ports else None)
            if not port:
                raise OSError('Arduino nicht gefunden')
            with serial.Serial(port, 9600, timeout=2, exclusive=True) as device:
                with LOCK:
                    STATE.update(connected=True, status='USB verbunden', pump=None)
                logging.info('Arduino verbunden')
                while True:
                    line = device.readline(512).decode('utf-8', errors='replace').strip()
                    if not line:
                        continue
                    sample = parse(line)
                    with LOCK:
                        STATE['last_line_at'] = time.time()
                        if sample:
                            sample['ts'] = time.time()
                            STATE['measurement'] = sample
                            STATE['pump'] = sample['pump']
                            STATE['status'] = 'Messwerte empfangen'
                        elif line == '>>> Pumpe AN':
                            STATE['pump'] = True
                        elif line in ('>>> Pumpe AUS', 'Pumpe sofort gestoppt.', 'OLED FEHLER'):
                            STATE['pump'] = False
                        if line in ('MAXIMALE RUNDEN ERREICHT', 'ZIEL ERREICHT', 'TANK LEER', 'OLED FEHLER'):
                            STATE['status'] = line
                    if sample and time.time() - last_saved >= 10:
                        with connection() as db:
                            db.execute('INSERT OR REPLACE INTO samples VALUES (?,?)', (sample['ts'], json.dumps(sample)))
                            db.execute('DELETE FROM samples WHERE ts < ?', (time.time()-30*86400,))
                        last_saved = time.time()
        except (OSError, serial.SerialException):
            with LOCK:
                STATE.update(connected=False, status='USB-Verbindung unterbrochen', pump=None)
            logging.warning('Arduino nicht erreichbar; erneuter Versuch in 5 Sekunden')
            time.sleep(5)

def status_text():
    s = snapshot()
    m = s['measurement']
    if s['stale']:
        return 'Keine aktuellen Messwerte. Bitte USB-Verbindung und Arduino prüfen.'
    pump = 'AN' if s['pump'] else 'AUS'
    water = 'vorhanden' if m['water_ok'] else 'LEER'
    return f"Bodenfeuchte: {m['soil_pct']} % (Rohwert {m['soil_raw']})\nTankanzeige: {m['tank_pct']} % · Wasser: {water}\nPumpe: {pump}\nMessung vor {s['age_seconds']:.0f} Sekunden"

def telegram_call(token, method, **params):
    request = Request('https://api.telegram.org/bot' + token + '/' + method, data=urlencode(params).encode())
    with urlopen(request, timeout=40) as response:
        body = json.load(response)
    if not body.get('ok'):
        raise RuntimeError('Telegram-Anfrage fehlgeschlagen')
    return body['result']

def telegram():
    # Configuration is local only, never served over HTTP. No automatic chat enrollment.
    offset_file = ROOT / 'telegram-offset.json'
    try:
        offset = json.loads(offset_file.read_text())
    except (OSError, ValueError):
        offset = 0
    sent_alert = None
    started_at = time.monotonic()
    next_status_at = started_at + 5
    while True:
        try:
            config = json.loads((ROOT / 'telegram.json').read_text())
            token, chat = config['token'], str(config['chat_id'])
            s = snapshot()
            if time.monotonic() >= next_status_at and (s['measurement'] or time.monotonic() - started_at >= 45):
                telegram_call(token, 'sendMessage', chat_id=chat, text='Automatischer Status · alle 5 Minuten\n\n' + status_text())
                logging.info('Automatischer Telegram-Status gesendet')
                next_status_at = time.monotonic() + TELEGRAM_INTERVAL_SECONDS
            poll_timeout = max(1, min(25, int(next_status_at - time.monotonic())))
            updates = telegram_call(token, 'getUpdates', offset=offset, timeout=poll_timeout, allowed_updates=json.dumps(['message']))
            for update in updates:
                message = update.get('message', {})
                if str(message.get('chat', {}).get('id')) == chat and message.get('text', '').split('@')[0].strip() in ('/start', '/status', '/hilfe'):
                    telegram_call(token, 'sendMessage', chat_id=chat, text=status_text())
                offset = update['update_id'] + 1
                offset_file.write_text(json.dumps(offset))
            s = snapshot()
            if not s['measurement'] and time.monotonic() - started_at < 45:
                continue
            alert = 'Keine aktuellen Arduino-Messwerte.' if s['stale'] else ('Tank leer – bitte Wasser nachfüllen.' if not s['measurement']['water_ok'] else None)
            if alert != sent_alert:
                if alert:
                    telegram_call(token, 'sendMessage', chat_id=chat, text=alert)
                elif sent_alert:
                    telegram_call(token, 'sendMessage', chat_id=chat, text='Anlage meldet wieder aktuelle Werte und Wasser im Tank.')
                sent_alert = alert
        except FileNotFoundError:
            time.sleep(10)
        except Exception:
            # Do not log exceptions: Telegram URLs contain the secret bot token.
            logging.warning('Telegram derzeit nicht verfügbar; Konfiguration oder Internet prüfen')
            time.sleep(15)

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        route = self.path.split('?')[0]
        if route == '/':
            body = (ROOT / 'index.html').read_bytes()
            mime = 'text/html; charset=utf-8'
        elif route in ('/api/status', '/api/history'):
            body = json.dumps(snapshot() if route == '/api/status' else history()).encode()
            mime = 'application/json; charset=utf-8'
        elif route == '/favicon.ico':
            self.send_response(204)
            self.end_headers()
            return
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_):
        pass

if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    initialize()
    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=telegram, daemon=True).start()
    ThreadingHTTPServer(('0.0.0.0', 8080), Handler).serve_forever()
