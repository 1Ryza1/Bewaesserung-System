#!/usr/bin/env python3
import getpass
import json
import os
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.parse import urlencode

root = Path(__file__).resolve().parent
print('Bot bei @BotFather mit /newbot erstellen. Token bleibt auf diesem Pi.')
token = getpass.getpass('Bot-Token (unsichtbare Eingabe): ').strip()
def call(method, **params):
    req = Request('https://api.telegram.org/bot'+token+'/'+method, data=urlencode(params).encode())
    with urlopen(req, timeout=40) as response:
        body = json.load(response)
    if not body.get('ok'):
        raise RuntimeError()
    return body['result']
try:
    me = call('getMe')
    print('Bot: @'+me['username'])
    input('Öffne diesen Bot in Telegram, sende /start und drücke hier Enter. ')
    updates = call('getUpdates', timeout=1)
    chats = {}
    for u in updates:
        msg = u.get('message', {})
        chat = msg.get('chat', {})
        if chat.get('type') == 'private':
            chats[str(chat['id'])] = chat.get('first_name', 'Privater Chat')
    for cid, name in chats.items():
        print(cid, name)
    chat_id = input('Deine oben angezeigte Chat-ID: ').strip()
    if chat_id not in chats:
        raise ValueError('Chat nicht in den empfangenen Nachrichten enthalten. Erneut /start senden und Einrichtung wiederholen.')
    path = root/'telegram.json'
    fd = os.open(path, os.O_WRONLY|os.O_CREAT|os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump({'token':token, 'chat_id':chat_id}, f)
    path.chmod(0o600)
    print('Gespeichert. Der Dienst aktiviert den Bot automatisch in spätestens 15 Sekunden.')
except Exception:
    print('Einrichtung fehlgeschlagen. Token, Internetverbindung und /start-Nachricht prüfen. Keine Zugangsdaten wurden ausgegeben.')
    raise SystemExit(1)
