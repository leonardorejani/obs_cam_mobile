import socket, struct, time, subprocess, sys, os, signal
BIN="/tmp/fmcam-asan/tools/"
AM="/tmp/fmcam-asan/amostra.h264"
PORT=5300
def start(extra=()):
    global PORT; PORT+=1
    p=subprocess.Popen([BIN+"iphone-falso","--arquivo",AM,"--porta",str(PORT),"--silencioso",*extra],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))
    p.stdout.readline(); return p,PORT
def conn(port):
    s=socket.create_connection(("127.0.0.1",port)); return s
def readline(s,to=6):
    s.settimeout(to); b=b""
    try:
        while not b.endswith(b"\n"):
            d=s.recv(1)
            if not d: return b,"EOF"
            b+=d
    except socket.timeout: return b,"timeout"
    except ConnectionResetError: return b,"RESET"
    return b,"ok"
def pk(t,tempo,d): return struct.pack(">BQI",t,tempo,len(d))+d
HS=b'{"v":1,"cliente":"atk"}\n'
def stop(p):
    p.terminate(); 
    try: out=p.communicate(timeout=5)[1].decode()
    except Exception: p.kill(); out=""
    if "ERROR" in out or "runtime error" in out: print("   !!! SANITIZER:", out[:600])
# a) mudo
p,po=start(); s=conn(po); t=time.time(); r=readline(s); print("a) cliente mudo ->",r,"apos %.1fs"%(time.time()-t)); s.close(); stop(p)
# b) handshake e silencio
p,po=start(); s=conn(po); s.sendall(HS); r=readline(s); t=time.time(); s.settimeout(8)
try:
    while True:
        d=s.recv(65536)
        if not d: break
    print("b) silencio apos handshake: fechado apos %.1fs (doc 5s)"%(time.time()-t))
except Exception as e: print("b)",e)
s.close(); stop(p)
# c) extra aninhado
p,po=start(); s=conn(po); s.sendall(b'{"v":1,"extra":{"a":1}}\n'); print("c) handshake com campo extra aninhado ->",readline(s)[0][:80]); s.close()
s=conn(po); s.sendall(b'{"v":1,"lista":["a","b"]}\n'); print("   handshake com array de strings ->",readline(s)[0][:80]); s.close()
s=conn(po); s.sendall(b'{"v":1,"lista":[1,2],"x":null,"y":true}\n'); print("   handshake com array numeros/null/bool ->",readline(s)[0][:60]); s.close()
# comando com campo extra
s=conn(po); s.sendall(HS); readline(s); s.sendall(pk(4,1,b'{"cmd":"keyframe","opts":{"x":1}}')); time.sleep(0.3); s.close(); stop(p)
# d) reconexao rapida 50x (fecha e reabre imediatamente)
p,po=start(); ok=oc=outro=0
for i in range(50):
    s=conn(po); s.sendall(HS); l,m=readline(s,3)
    if b'"ok":true' in l: ok+=1
    elif b'ocupado' in l: oc+=1
    else: outro+=1
    s.close()
print("d) reconexao imediata 50x: ok=%d ocupado=%d outro=%d"%(ok,oc,outro))
time.sleep(0.2)
# e) dois clientes
a=conn(po); a.sendall(HS); readline(a); b=conn(po); b.sendall(HS); print("e) segundo cliente ->",readline(b)[0][:60]); b.close(); a.close()
# f) cliente que fecha no meio de pacote enviado por ele (pacote cortado) e tamanho gigante
time.sleep(0.2); a=conn(po); a.sendall(HS); readline(a); a.sendall(struct.pack(">BQI",9,1,8*1024*1024+1)); a.settimeout(3)
try:
    d=a.recv(1<<20)
    t=time.time()
    while d: d=a.recv(1<<20)
    print("f) tamanho gigante do cliente -> falso fechou")
except Exception as e: print("f) falso NAO fechou:",e)
a.close()
time.sleep(0.2)
# g) cliente lento: nao le por 12 s (so ping), depois le
a=conn(po); a.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096); a.sendall(HS); 
t0=time.time(); alive=True
while time.time()-t0<12:
    try: a.sendall(pk(9,1,b""))
    except Exception: alive=False; break
    time.sleep(1)
a.settimeout(1); tot=0; t1=time.time()
try:
    while time.time()-t1<4:
        d=a.recv(1<<20)
        if not d: break
        tot+=len(d)
except Exception: pass
print("g) cliente lento 12s: vivo=%s, ao voltar a ler em 4s recebeu %.1f MB (acumulou ~12s*~1.2MB/s=14MB se nao descarta)"%(alive,tot/1e6))
a.close(); stop(p)
# h) matar o falso no meio
p,po=start()
a=conn(po); a.sendall(HS); readline(a); a.settimeout(2); a.recv(100000); p.kill(); p.wait()
try:
    d=a.recv(1<<20)
    while d: d=a.recv(1<<20)
    print("h) falso morto -> cliente viu EOF limpo")
except Exception as e: print("h) falso morto ->",type(e).__name__)
