import socket, struct, sys, time, threading, os
sc=sys.argv[1]; path=sys.argv[2]; dur=float(sys.argv[3]); fport=int(sys.argv[4]) if len(sys.argv)>4 else 0
HEAD='<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0">\n<dict>\n'; TAIL='</dict>\n</plist>\n'
def msg(x,tag=1,tipo=8,ver=1,tam=None):
    b=x if isinstance(x,bytes) else x.encode()
    return struct.pack("<IIII", 16+len(b) if tam is None else tam, ver, tipo, tag)+b
def result(n): return HEAD+'<key>MessageType</key><string>Result</string><key>Number</key><integer>%s</integer>\n'%n+TAIL
def devlist(items): 
    s=HEAD+'<key>DeviceList</key><array>'
    for (i,u,ct) in items: s+='<dict><key>DeviceID</key><integer>%d</integer><key>MessageType</key><string>Attached</string><key>Properties</key><dict><key>ConnectionType</key><string>%s</string><key>DeviceID</key><integer>%d</integer><key>SerialNumber</key><string>%s</string></dict></dict>'%(i,ct,i,u)
    return s+'</array>'+TAIL
def rd(c):
    h=b""
    while len(h)<16:
        d=c.recv(16-len(h)); 
        if not d: return None
        h+=d
    tam=struct.unpack("<I",h[:4])[0]; b=b""
    while len(b)<tam-16:
        d=c.recv(tam-16-len(b)); 
        if not d: break
        b+=d
    return b.decode(errors="replace")
def tunnel(c):
    d=socket.create_connection(("127.0.0.1",fport))
    def a():
        try:
            while True:
                x=c.recv(65536)
                if not x: break
                d.sendall(x)
        except Exception: pass
        try: d.shutdown(socket.SHUT_WR)
        except Exception: pass
    threading.Thread(target=a,daemon=True).start()
    try:
        while True:
            x=d.recv(65536)
            if not x: break
            c.sendall(x)
    except Exception: pass
n=0
def handle(c):
    global n
    try:
        x=rd(c)
        if x is None: return
        n+=1
        if "ListDevices" in x:
            if sc=="malformado": c.sendall(msg("<plist><dict><key>DeviceList</key><array><dict><key>DeviceID</key>"))
            elif sc=="vazio": c.sendall(msg(devlist([])))
            elif sc=="semprop": c.sendall(msg(HEAD+'<key>DeviceList</key><array><dict><key>DeviceID</key><integer>1</integer></dict></array>'+TAIL))
            elif sc=="tamlixo": c.sendall(struct.pack("<IIII",0xFFFFFFFF,1,8,1)+b"zzz")
            elif sc=="tammenor": c.sendall(struct.pack("<IIII",4,1,8,1))
            elif sc=="tiponao8": c.sendall(msg(devlist([(1,"U1","USB")]),tipo=3))
            elif sc=="fatiado":
                m=msg(devlist([(1,"U1","USB")]))
                for i in range(0,len(m)): c.sendall(m[i:i+1]); 
            elif sc=="silencio": time.sleep(dur)
            elif sc=="cortado": m=msg(devlist([(1,"U1","USB")])); c.sendall(m[:len(m)//2])
            elif sc=="rede": c.sendall(msg(devlist([(1,"U1","Network")])))
            else: c.sendall(msg(devlist([(1,"U1","USB"),(2,"U2","Network")])))
        elif "Connect" in x:
            if sc.startswith("res"):
                c.sendall(msg(result(sc[3:])))
            elif sc=="connectmalformado": c.sendall(msg("<plist><dict>lixo"))
            elif sc=="connectsilencio": time.sleep(dur)
            else:
                c.sendall(msg(result(0))); tunnel(c)
        elif "Listen" in x:
            c.sendall(msg(result(0))); c.sendall(msg(HEAD+'<key>MessageType</key><string>Attached</string><key>DeviceID</key><integer>7</integer><key>Properties</key><dict><key>ConnectionType</key><string>USB</string><key>DeviceID</key><integer>7</integer><key>SerialNumber</key><string>U1</string></dict>'+TAIL))
            time.sleep(2); c.sendall(msg(HEAD+'<key>MessageType</key><string>Detached</string><key>DeviceID</key><integer>7</integer>'+TAIL)); time.sleep(dur)
    except Exception as e: pass
    finally: c.close()
try: os.unlink(path)
except: pass
s=socket.socket(socket.AF_UNIX); s.bind(path); s.listen(8); s.settimeout(0.5)
end=time.time()+dur+2
while time.time()<end:
    try: c,_=s.accept()
    except socket.timeout: continue
    threading.Thread(target=handle,args=(c,),daemon=True).start()
print("pedidos",n)
