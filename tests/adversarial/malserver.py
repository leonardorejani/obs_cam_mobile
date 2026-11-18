import socket, struct, sys, time, threading, json
sc = sys.argv[1]; port = int(sys.argv[2]); dur = float(sys.argv[3]) if len(sys.argv) > 3 else 8
OK = b'{"ok":true,"v":1,"aparelho":"mal","w":1920,"h":1080,"fps":30,"codec":"h264"}\n'
def pk(t, tempo, d): return struct.pack(">BQI", t, tempo, len(d)) + d
SPS = bytes.fromhex("0000000167640028acd940780227e5c05a808080a000000300200000078c0000000168ebecb22c")
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(8); s.settimeout(0.5)
conns = 0
def readline(c, n=5000):
    b = b""
    c.settimeout(5)
    while not b.endswith(b"\n") and len(b) < n:
        d = c.recv(1)
        if not d: break
        b += d
    return b
def handle(c, idx):
    try:
        readline(c)
        if sc == "semfim":                       # resposta sem \n, depois silencio
            c.sendall(b'{"ok":true,"v":1'); time.sleep(8)
        elif sc == "enorme":                     # linha de handshake de resposta gigante
            c.sendall(b"x"*10000 + b"\n"); time.sleep(2)
        elif sc == "zeros":                      # w=h=fps=0 e depois quadros
            c.sendall(b'{"ok":true,"v":1,"w":0,"h":0,"fps":0}\n'); c.sendall(pk(1,1000,SPS)); time.sleep(1)
            for i in range(30): c.sendall(pk(2,1000+i*33333,b"\x01"+b"\x00\x00\x00\x01\x65"+b"\xff"*50)); time.sleep(0.03)
        elif sc == "nan":
            c.sendall(b'{"ok":true,"v":1,"w":-5,"h":1e999,"fps":-1}\n'); time.sleep(2)
        elif sc == "extra":                      # campo extra aninhado (forward compat)
            c.sendall(b'{"ok":true,"v":1,"aparelho":"x","w":1920,"h":1080,"fps":30,"codec":"h264","lentes":[{"id":"wide"}]}\n'); time.sleep(2)
        elif sc == "quadros_hostis":
            c.sendall(OK); c.sendall(pk(1,1000,b"")); c.sendall(pk(2,1000,b"")); c.sendall(pk(2,1001,b"\x01"))
            c.sendall(pk(1,1002,SPS)); 
            import os
            for i in range(200): c.sendall(pk(2,1000+i*33333,bytes([i&1])+os.urandom(2000)))
            c.sendall(pk(3,5,b"\xff\xfe\x00{")); c.sendall(pk(10,5,b"abc")); c.sendall(pk(10,5,b"")); c.sendall(pk(99,5,b"zz"))
            time.sleep(3)
        elif sc == "gigante":                    # tamanho 8MiB+1
            c.sendall(OK); time.sleep(0.3); c.sendall(struct.pack(">BQI", 2, 1, 8*1024*1024+1)); time.sleep(3)
        elif sc == "desconhecido8m":             # tipo desconhecido de 8MiB e depois pacote bom
            c.sendall(OK); c.sendall(struct.pack(">BQI", 77, 1, 8*1024*1024)); c.sendall(b"\x00"*(8*1024*1024)); c.sendall(pk(3,1,b'{"fps":1}')); time.sleep(3)
        elif sc == "corta":                      # pacote cortado no meio e fecha
            c.sendall(OK); c.sendall(pk(2,1,b"\x01"+b"\x00"*1000)[:500]); time.sleep(0.2)
        elif sc == "pongfalso":                  # pong com eco absurdo, fora de ordem, relogio voltando
            c.sendall(OK)
            c.settimeout(0.05); t=0
            end=time.time()+dur; i=0
            while time.time()<end:
                try: d=c.recv(4096)
                except socket.timeout: d=b"x"
                if not d: break
                i+=1
                eco=[0, 2**64-1, 2**63, 12345, 1][i%5]
                c.sendall(pk(10, [0,2**64-1,10**18,5][i%4], struct.pack(">Q", eco)))
                time.sleep(0.2)
        elif sc == "lento":                      # manda 1 byte por vez
            for b in OK: c.sendall(bytes([b])); time.sleep(0.05)
            data = pk(3,1,b'{"fps":1}')
            for _ in range(5):
                for b in data: c.sendall(bytes([b])); time.sleep(0.005)
            time.sleep(1)
        elif sc == "silencio":                   # aceita handshake e nao manda nada
            c.sendall(OK); time.sleep(dur)
        elif sc == "ocupado":
            c.sendall(b'{"ok":false,"erro":"ocupado"}\n'); time.sleep(0.1)
        elif sc == "versao_nova":
            c.sendall(b'{"ok":false,"erro":"versao","v_suportadas":[2]}\n'); time.sleep(0.1)
        elif sc == "versao_vazia":
            c.sendall(b'{"ok":false,"erro":"versao"}\n'); time.sleep(0.1)
    except Exception as e:
        pass
    finally:
        c.close()
end = time.time() + dur + 2
while time.time() < end:
    try: c, _ = s.accept()
    except socket.timeout: continue
    conns += 1
    threading.Thread(target=handle, args=(c, conns), daemon=True).start()
print("conexoes", conns)
