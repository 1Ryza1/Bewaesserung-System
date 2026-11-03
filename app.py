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
from urllib.parse import urlencode, parse_qs, urlsplit
from urllib.request import urlopen, Request
import serial

ROOT = Path(__file__).resolve().parent
DB = ROOT / 'messwerte.sqlite3'
LOCK = threading.Lock()
STATE = {'measurement': None, 'connected': False, 'pump': None, 'status': 'Warte auf Arduino', 'last_line_at': None, 'arduino_details': {}}
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

DETAIL_PATTERNS = {
    'status': re.compile(r'STATUS code=(\d+) runde=(\d+) max=(\d+) sperre_s=(\d+) budget_ms=(\d+) uptime_ms=(\d+)'),
    'statistik': re.compile(r'STATISTIK giessen=(\d+) runden=(\d+) sekunden=(\d+)'),
    'konfig': re.compile(r'KONFIG start=(\d+) ziel=(\d+) pump_ms=(\d+) warte_ms=(\d+) menue=(\d+)'),
}
DETAIL_KEYS = {'status': ('code', 'runde', 'max', 'sperre_s', 'budget_ms', 'uptime_ms'),
               'statistik': ('giessen', 'runden', 'sekunden'),
               'konfig': ('start', 'ziel', 'pump_ms', 'warte_ms', 'menue')}

def parse_details(line):
    for name, pattern in DETAIL_PATTERNS.items():
        match = pattern.fullmatch(line.strip())
        if match:
            return name, dict(zip(DETAIL_KEYS[name], map(int, match.groups())))
    return None

def statistics():
    with connection() as db:
        rows = db.execute('SELECT data FROM samples WHERE ts > ? ORDER BY ts', (time.time() - 86400,)).fetchall()
    values = [json.loads(row[0]) for row in rows]
    if not values:
        return {'count': 0}
    result = {'count': len(values), 'first_ts': values[0]['ts'], 'last_ts': values[-1]['ts']}
    for field in ('soil_pct', 'tank_pct'):
        numbers = [sample[field] for sample in values]
        result[field] = {'min': min(numbers), 'max': max(numbers), 'avg': round(sum(numbers) / len(numbers), 1)}
    return result

def analysis(hours=24, now=None):
    """Aggregate every stored sample; chart points are time-bucket averages.

    A gap over 60 seconds starts a new line so the chart never bridges outages.
    """
    now = time.time() if now is None else now
    start = now - hours * 3600
    bucket_seconds = hours * 3600 / 500
    fields = ('soil_pct', 'tank_pct', 'soil_raw', 'tank_raw')
    summary = {field: {'min': None, 'max': None, 'sum': 0} for field in fields}
    count = gaps = dry_count = 0
    first = last = None
    points = []
    bucket = None

    def flush():
        if bucket:
            points.append({'ts': bucket['ts_sum'] / bucket['count'],
                           'soil_pct': round(bucket['soil_sum'] / bucket['count'], 1),
                           'tank_pct': round(bucket['tank_sum'] / bucket['count'], 1),
                           'break_before': bucket['break_before']})

    with connection() as db:
        rows = db.execute('SELECT ts, data FROM samples WHERE ts >= ? AND ts <= ? ORDER BY ts', (start, now))
        for ts, data in rows:
            sample = json.loads(data)
            gap = last is not None and ts - last > 60
            if gap:
                gaps += 1
            key = int((ts - start) / bucket_seconds)
            if bucket is None or key != bucket['key'] or gap:
                flush()
                bucket = {'key': key, 'count': 0, 'ts_sum': 0, 'soil_sum': 0,
                          'tank_sum': 0, 'break_before': gap}
            bucket['count'] += 1
            bucket['ts_sum'] += ts
            bucket['soil_sum'] += sample['soil_pct']
            bucket['tank_sum'] += sample['tank_pct']
            if first is None:
                first = ts
            last = ts
            count += 1
            if sample.get('water_ok') is False:
                dry_count += 1
            for field in fields:
                value = sample[field]
                item = summary[field]
                item['sum'] += value
                item['min'] = value if item['min'] is None else min(item['min'], value)
                item['max'] = value if item['max'] is None else max(item['max'], value)
    flush()
    for item in summary.values():
        item['avg'] = round(item.pop('sum') / count, 1) if count else None
    return {'hours': hours, 'start_ts': start, 'end_ts': now, 'first_ts': first,
            'last_ts': last, 'count': count, 'gaps': gaps, 'tank_empty_samples': dry_count,
            'metrics': summary, 'points': points}

def connection():
    return sqlite3.connect(DB, timeout=10)

def initialize():
    with connection() as db:
        db.execute('PRAGMA journal_mode=WAL')
        db.execute('CREATE TABLE IF NOT EXISTS samples (ts REAL PRIMARY KEY, data TEXT NOT NULL)')

def snapshot():
    with LOCK:
        result = dict(STATE)
        result['arduino_details'] = dict(STATE['arduino_details'])
    m = result['measurement']
    result['age_seconds'] = round(time.time() - m['ts'], 1) if m else None
    result['stale'] = not m or result['age_seconds'] > 45 or not result['connected']
    result['server_time'] = time.time()
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
                    STATE.update(connected=True, status='USB verbunden', pump=None, arduino_details={})
                logging.info('Arduino verbunden')
                while True:
                    line = device.readline(512).decode('utf-8', errors='replace').strip()
                    if not line:
                        continue
                    sample = parse(line)
                    detail = parse_details(line)
                    with LOCK:
                        STATE['last_line_at'] = time.time()
                        if detail:
                            STATE['arduino_details'][detail[0]] = {'values': detail[1], 'ts': time.time()}
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
        parsed = urlsplit(self.path)
        route = parsed.path
        if route == '/':
            body = (ROOT / 'index.html').read_bytes()
            mime = 'text/html; charset=utf-8'
        elif route == '/api/analysis':
            hours = parse_qs(parsed.query).get('hours', ['24'])[0]
            if hours not in ('24', '168', '720'):
                self.send_error(400, 'Zeitraum muss 24, 168 oder 720 Stunden sein')
                return
            body = json.dumps(analysis(int(hours))).encode()
            mime = 'application/json; charset=utf-8'
        elif route in ('/api/status', '/api/history', '/api/statistics'):
            body = json.dumps(snapshot() if route == '/api/status' else statistics() if route == '/api/statistics' else history()).encode()
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
