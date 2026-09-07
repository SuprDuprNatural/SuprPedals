"""Render mono PCM16 bass DI through the actual local LV2 binaries and TTL presets.
Usage: python3 tools/render_time_space.py input.wav build/time-space/auditions
Outputs are private listening material; no normalization, with 10 seconds of tails.
"""
import array, ctypes as C, math, re, sys, wave
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=Path(sys.argv[1]); dest=Path(sys.argv[2]);dest.mkdir(parents=True,exist_ok=True)
with wave.open(str(source)) as w:
    assert w.getsampwidth()==2 and w.getnchannels()==1, 'Expected mono PCM16'
    sr=w.getframerate(); samples=array.array('h',w.readframes(w.getnframes()))
    if sys.byteorder!='little':samples.byteswap()
input_values=[v/32768 for v in samples]+[0.]*(sr*10)
class Descriptor(C.Structure): pass
Inst=C.CFUNCTYPE(C.c_void_p,C.POINTER(Descriptor),C.c_double,C.c_char_p,C.c_void_p)
Connect=C.CFUNCTYPE(None,C.c_void_p,C.c_uint32,C.c_void_p)
Activate=C.CFUNCTYPE(None,C.c_void_p)
Run=C.CFUNCTYPE(None,C.c_void_p,C.c_uint32)
Descriptor._fields_=[('uri',C.c_char_p),('instantiate',Inst),('connect',Connect),('activate',Activate),('run',Run),('deactivate',C.c_void_p),('cleanup',Activate),('extension',C.c_void_p)]
for effect in ['echo','space']:
    lib=C.CDLL(str(root/f'build/supr{effect}.so'));lib.lv2_descriptor.restype=C.POINTER(Descriptor)
    dp=lib.lv2_descriptor(0); d=dp.contents
    ttl=(root/f'ttl/supr{effect}.ttl').read_text()
    ports={s:int(i) for i,s in re.findall(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]+)"',ttl)}
    presets=(root/f'ttl/{effect}-presets.ttl').read_text()
    for block in presets.split('a pset:Preset')[1:]:
        label=re.search(r'rdfs:label "([^"]+)"',block)[1]
        controls={s:C.c_float(float(v)) for s,v in re.findall(r'lv2:symbol "([^"]+)" ; pset:value ([\d.]+)',block)}
        handle=d.instantiate(dp,sr,None,None); assert handle
        for s,v in controls.items():d.connect(handle,ports[s],C.byref(v))
        inp=(C.c_float*128)();out=(C.c_float*128)();d.connect(handle,0,inp);d.connect(handle,1,out);d.activate(handle)
        result=array.array('h');peak=0;clipped=0;energy=0
        for offset in range(0,len(input_values),128):
            n=min(128,len(input_values)-offset)
            for j in range(n):inp[j]=input_values[offset+j]
            d.run(handle,n)
            for j in range(n):
                v=out[j];assert math.isfinite(v);peak=max(peak,abs(v));energy+=v*v;clipped+=abs(v)>=1
                result.append(max(-32768,min(32767,round(v*32768))))
        d.cleanup(handle)
        if sys.byteorder!='little':result.byteswap()
        target=dest/(effect+'-'+label.lower().replace(' ','-')+'.wav')
        with wave.open(str(target),'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(sr);w.writeframes(result.tobytes())
        print(f'{target.name}: peak {20*math.log10(max(peak,1e-12)):.2f} dBFS; RMS {10*math.log10(max(energy/len(result),1e-24)):.2f} dBFS; clipped {clipped}')
