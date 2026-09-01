#!/usr/bin/env python3
"""
wavchunk round-trip and mutation tests.

split->join with no processing in between must reproduce the input byte for
byte: the crossfade regions hold identical samples on both sides, so a correct
linear crossfade is the identity there. That makes an exact comparison the
right assertion rather than a tolerance, and it is what caught two real
precision bugs — the symmetric blend tail*(1-g)+cur*g (not exact in float, and
32-bit float is the format the engine emits) and decoding through float, which
cannot hold 32-bit PCM.

The MUTATION cases exist because a round-trip test that cannot fail proves
nothing: each one breaks the pipeline deliberately and asserts the check
notices.

Run:  python3 tests/test_wavchunk.py [path-to-wavchunk]
      (with no argument it compiles src/tools/wavchunk.c for the host)
"""
import struct, os, subprocess, sys, math, random, shutil
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP=os.path.join(ROOT,'build','wct'); shutil.rmtree(TMP,ignore_errors=True); os.makedirs(TMP)
if len(sys.argv)>1:
    WC=sys.argv[1]
else:
    WC=os.path.join(ROOT,'build','wavchunk-host')
    os.makedirs(os.path.join(ROOT,'build'),exist_ok=True)
    cc=os.environ.get('CC','cc')
    r=subprocess.run([cc,'-O2','-Wall','-Wextra','-Werror','-o',WC,
                      os.path.join(ROOT,'src','tools','wavchunk.c'),'-lm'],
                     capture_output=True,text=True)
    if r.returncode!=0:
        print(r.stdout+r.stderr); sys.exit(2)

def mkwav(path, frames, ch=2, bits=16, fmt=1, sr=44100):
    random.seed(frames*100+bits+ch)
    bs=bits//8; data=bytearray()
    for i in range(frames):
        for c in range(ch):
            if fmt==3: data+=struct.pack('<f', random.uniform(-0.9,0.9))
            elif bits==16: data+=struct.pack('<h', random.randint(-30000,30000))
            elif bits==24:
                v=random.randint(-8000000,8000000)&0xffffff; data+=bytes([v&0xff,(v>>8)&0xff,(v>>16)&0xff])
            elif bits==32: data+=struct.pack('<i', random.randint(-2000000000,2000000000))
            elif bits==8: data+=bytes([random.randint(0,255)])
    data=bytes(data); fb=ch*bs
    hdr=b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,fmt,ch,sr,sr*fb,fb,bits)+b'data'+struct.pack('<I',len(data))
    open(path,'wb').write(hdr+data); return data

def payload(path):
    b=open(path,'rb').read(); i=b.index(b'data'); n=struct.unpack('<I',b[i+4:i+8])[0]
    return b[i+8:i+8+n]

def run(*a):
    r=subprocess.run([WC]+[str(x) for x in a],capture_output=True,text=True)
    return r.returncode, r.stdout, r.stderr

fails=[]
def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ")+name+((" :: "+detail) if (detail and not cond) else ""))
    if not cond: fails.append(name)

CHUNK, XF = 1000, 100
cases=[]
for ch,bits,fmt in ((2,16,1),(2,32,3),(1,16,1),(2,24,1),(2,32,1),(1,8,1)):
    for frames in (500, 1000, 1001, 1100, 1101, 2000, 2500, 1050, 1249, 1250, 5000, 12345):
        cases.append((frames,ch,bits,fmt))

for frames,ch,bits,fmt in cases:
    tag=f"f{frames}_c{ch}_b{bits}_fmt{fmt}"
    d=os.path.join(TMP,tag); os.makedirs(d,exist_ok=True)
    src=os.path.join(d,'in.wav'); orig=mkwav(src,frames,ch,bits,fmt)
    rc,out,err=run('split',src,d,CHUNK,XF)
    if rc!=0: check(tag+" split", False, err.strip()); continue
    parts=[l.split()[0] for l in out.strip().splitlines()]
    lens=[int(l.split()[1]) for l in out.strip().splitlines()]
    # invariant: adjacent chunks overlap by exactly XF -> total = sum - (n-1)*XF
    check(tag+" length-invariant", sum(lens)-(len(parts)-1)*XF==frames, f"sum={sum(lens)} n={len(parts)} frames={frames} lens={lens}")
    check(tag+" no-runt", all(l>=2*XF for l in lens) or frames<2*XF, f"lens={lens}")
    joined=os.path.join(d,'out.wav')
    rc,out2,err=run('join',joined,XF,*[os.path.join(d,p) for p in parts])
    if rc!=0: check(tag+" join", False, err.strip()); continue
    check(tag+" roundtrip-exact", payload(joined)==orig,
          f"len {len(payload(joined))} vs {len(orig)}")

# info
d=os.path.join(TMP,'info'); os.makedirs(d,exist_ok=True)
mkwav(os.path.join(d,'i.wav'), 4321, 2, 16, 1)
rc,out,err=run('info',os.path.join(d,'i.wav'))
check("info", rc==0 and "frames=4321" in out and "channels=2" in out and "bits=16" in out, out+err)

# --- mutation checks: the probe must be able to fail ---
d=os.path.join(TMP,'mut'); os.makedirs(d,exist_ok=True)
src=os.path.join(d,'in.wav'); orig=mkwav(src,5000,2,16,1)
rc,out,_=run('split',src,d,CHUNK,XF)
parts=[os.path.join(d,l.split()[0]) for l in out.strip().splitlines()]
j=os.path.join(d,'bad.wav')
run('join',j,XF+7,*parts)
check("MUTATION wrong-xfade-differs", payload(j)!=orig, "wrong xfade still produced an identical file - test is blind")
run('join',j,XF,*parts[:-1])
check("MUTATION dropped-part-differs", payload(j)!=orig)
rc,_,err=run('split',src,d,100,100)
check("MUTATION rejects chunk<=2*xfade", rc!=0, "accepted a degenerate chunk size")
rc,_,err=run('join',os.path.join(d,'x.wav'),XF,os.path.join(d,'nope.wav'))
check("MUTATION missing-part-errors", rc!=0)
open(os.path.join(d,'trunc.wav'),'wb').write(open(src,'rb').read()[:20])
rc,_,err=run('info',os.path.join(d,'trunc.wav'))
check("MUTATION truncated-header-errors", rc!=0)

print()
print(("ALL PASS" if not fails else f"{len(fails)} FAILURES: "+", ".join(fails)))
sys.exit(1 if fails else 0)
