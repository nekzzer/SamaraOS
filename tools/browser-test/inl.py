import re,sys,urllib.request,urllib.parse
u=sys.argv[1]; out=sys.argv[2]
req=lambda x: urllib.request.urlopen(urllib.request.Request(x,headers={'User-Agent':'Mozilla/5.0 (X11; SamaraOS) Dillo/3.0.5'}),timeout=20).read().decode('utf-8','replace')
h=req(u)
def rep(m):
    tag=m.group(0)
    if 'stylesheet' not in tag.lower(): return tag
    hm=re.search(r'href\s*=\s*["\']?([^"\' >]+)',tag)
    if not hm: return tag
    try: css=req(urllib.parse.urljoin(u,hm.group(1).replace('&amp;','&')))
    except Exception as e: return tag
    return '<style>'+css+'</style>'
h=re.sub(r'<link[^>]*>',rep,h,flags=re.I)
h=re.sub(r'<script.*?</script>','',h,flags=re.I|re.S)
open(out,'w').write(h)
