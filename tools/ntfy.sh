#!/bin/sh
# Talk to the phone through ntfy.sh (no account): topic in ~/.config/samara-ntfy-topic
#   tools/ntfy.sh msg "text"      tools/ntfy.sh pic file.png "text"      tools/ntfy.sh read
T=$(cat ~/.config/samara-ntfy-topic)
case "$1" in
msg) curl -s -m 20 -H "Title: SamaraOS" -d "$2" "https://ntfy.sh/$T" >/dev/null ;;
pic) curl -s -m 60 -T "$2" -H "Filename: $(basename "$2")" -H "Title: SamaraOS" -H "Message: $3" "https://ntfy.sh/$T" >/dev/null ;;
read)   # what came without our title = written by hand in the app
    curl -s -m 20 "https://ntfy.sh/$T/json?poll=1&since=${2:-12h}" | python3 -c '
import sys, json
for l in sys.stdin:
    m = json.loads(l)
    if m.get("event") == "message" and m.get("title") != "SamaraOS": print(m["time"], m.get("message", ""))' ;;
esac
