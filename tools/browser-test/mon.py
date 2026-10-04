import socket,sys,time,os
def cmd(c):
    s=socket.socket(socket.AF_UNIX); s.connect(os.environ.get('QMON','/tmp/samara-qmon')); s.recv(4096)
    s.send((c+'\n').encode()); time.sleep(0.3); r=s.recv(65536); s.close(); return r.decode(errors='replace')
KM={';':'semicolon','|':'shift-backslash','"':'shift-apostrophe',"'":'apostrophe',',':'comma','>':'shift-dot','<':'shift-comma','*':'shift-8','(':'shift-9',')':'shift-0','$':'shift-4','~':'shift-grave_accent','#':'shift-3','!':'shift-1','+':'shift-equal',' ':'spc','\n':'ret','.':'dot','/':'slash',':':'shift-semicolon','-':'minus','=':'equal','_':'shift-minus','?':'shift-slash','&':'shift-7'}
def typ(t):
    for ch in t:
        k=KM.get(ch)
        if not k:
            k = ('shift-'+ch.lower()) if ch.isupper() else ch
        cmd('sendkey '+k); time.sleep(0.03)
if __name__=='__main__':
    a=sys.argv[1]
    if a=='type': typ(sys.argv[2].replace('\\n','\n'))
    elif a=='shot':
        cmd('screendump /tmp/int-shot.ppm'); time.sleep(0.5)
        import subprocess; subprocess.run(['magick','/tmp/int-shot.ppm',sys.argv[2] if len(sys.argv)>2 else '/tmp/samara-shot.png'])
    else: print(cmd(' '.join(sys.argv[1:])))
