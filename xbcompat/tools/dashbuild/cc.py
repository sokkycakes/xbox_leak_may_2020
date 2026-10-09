#!/usr/bin/env python3
# cc.py file.cpp [flags...]: compile one xapp source with the leak's VC7 cl under Wine.
# The C++ front end spins forever after a fatal error under Wine, so kill it once an error line appears.
import os, subprocess, sys, time
B = os.environ["XB"]
INC = ";".join(B + "\\" + d for d in [r"private\ui\xapp", r"public\xdk\inc", r"private\inc", r"private\ntos\inc",
      r"private\ntos\xapi\inc", r"public\sdk\inc", r"public\ddk\inc", r"private\inc\crypto", r"private\ui\dvd\dongle\dvdlib"])
src = sys.argv[1]; extra = sys.argv[2:]
cwd = os.environ["CC_DIR"]
out = os.environ.get("CC_OUT", "Z:" + os.environ["WORK"].replace("/", "\\") + "\\obj\\")
env = dict(os.environ, WINEDEBUG="-all", INCLUDE=os.environ.get("CC_INC", INC))
defs = "-D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_UP=1 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 " \
       "-D_WIN32_WINNT=0x0400 -DWINVER=0x0400 -D_WIN32_IE=0x0400 -DDBG=0 -DDEVL=1 -DFPO=1 -D_XBOX -DXBOX=1 -DNDEBUG -D_MT " \
       "-DUNICODE -D_UNICODE".split()
cmd = ["wine", os.environ["WORK"] + "/xb/public/mstools/vc70/cl.exe", "/nologo", "/c", "/Zel", "/Zp8", "/Gy", "/W3", "/Gz", "/GX",
       "/Oxs", "/Gs"] + defs + extra + ["/Fo" + out, src]
p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
os.set_blocking(p.stdout.fileno(), False)
buf = b""; err_at = None; t0 = time.time()
while p.poll() is None:
    try:
        d = p.stdout.read()
        if d: buf += d
    except BlockingIOError: pass
    if err_at is None and b"fatal error" in buf: err_at = time.time()
    if err_at and time.time() - err_at > 2: p.kill(); break
    if time.time() - t0 > float(os.environ.get("CC_TIMEOUT", 600)): p.kill(); buf += b"\nTIMEOUT\n"; break
    time.sleep(0.2)
try: buf += p.stdout.read() or b""
except Exception: pass
sys.stdout.write(buf.decode("latin-1").replace("\r", ""))
sys.exit(0 if (p.returncode == 0 and b"error" not in buf) else 1)
