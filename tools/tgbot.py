#!/usr/bin/env python3
# Telegram relay: status texts and screenshots out, your replies in.
# Nothing you send is executed: messages just land in the inbox file,
# and whoever is working on the OS reads them from there.
#
#   token:  ~/.config/samara-tgbot/token   (from @BotFather, /newbot)
#   run:    python3 tools/tgbot.py &
#   pair:   the bot prints a code, send it to the bot once
#   out:    python3 tools/tgbot.py send "text"   /  photo file.png "caption"
#   in:     ~/.config/samara-tgbot/inbox.txt
import json, os, sys, time, random, urllib.request, urllib.parse

D = os.path.expanduser('~/.config/samara-tgbot')
TOKEN = open(os.path.join(D, 'token')).read().strip()
OWNER = os.path.join(D, 'owner')
INBOX = os.path.join(D, 'inbox.txt')
API = 'https://api.telegram.org/bot' + TOKEN + '/'

def call(m, data=None, files=None):
    if files:                                   # multipart by hand, no requests module here
        b = '----samara%d' % random.randint(0, 1 << 30)
        body = b''
        for k, v in (data or {}).items():
            body += ('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (b, k, v)).encode()
        for k, (name, blob) in files.items():
            body += ('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\n\r\n' % (b, k, name)).encode() + blob + b'\r\n'
        body += ('--%s--\r\n' % b).encode()
        req = urllib.request.Request(API + m, body, {'Content-Type': 'multipart/form-data; boundary=' + b})
    else:
        req = urllib.request.Request(API + m, urllib.parse.urlencode(data or {}).encode())
    return json.load(urllib.request.urlopen(req, timeout=70))

def owner():
    return open(OWNER).read().strip() if os.path.exists(OWNER) else None

if len(sys.argv) > 1:
    o = owner()
    if not o: sys.exit('not paired yet')
    if sys.argv[1] == 'send':
        call('sendMessage', {'chat_id': o, 'text': ' '.join(sys.argv[2:])[:4000]})
    elif sys.argv[1] == 'photo':
        cap = ' '.join(sys.argv[3:])[:1000]
        call('sendPhoto', {'chat_id': o, 'caption': cap}, {'photo': (os.path.basename(sys.argv[2]), open(sys.argv[2], 'rb').read())})
    sys.exit(0)

code = '%06d' % random.randint(0, 999999)
if not owner(): print('pair code:', code, flush=True)
off = 0
while True:
    try:
        r = call('getUpdates', {'offset': off, 'timeout': 60})
        for u in r.get('result', []):
            off = u['update_id'] + 1
            m = u.get('message') or {}
            chat, text = str(m.get('chat', {}).get('id', '')), m.get('text', '')
            if not owner():
                if text.strip() == code:
                    open(OWNER, 'w').write(chat)
                    call('sendMessage', {'chat_id': chat, 'text': 'paired, write here'})
                continue
            if chat != owner(): continue             # strangers are ignored
            with open(INBOX, 'a') as f:
                f.write(time.strftime('%H:%M ') + text + '\n')
            call('sendMessage', {'chat_id': chat, 'text': 'got it'})
    except Exception as e:
        print('tgbot:', e, flush=True)
        time.sleep(5)
