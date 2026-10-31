from pathlib import Path
import json,gzip,hashlib,struct
import contextlib,tempfile,zipfile,os
R=Path(__file__).resolve().parents[3];U=R/'output/ecp'
def resolve(value):
 p=Path(value)
 if p.is_absolute()and p.is_relative_to(R):return p
 for marker in ['/output/','/native_base/','/restore/']:
  if marker in str(p):return R/marker.strip('/')/str(p).split(marker,1)[1]
 return p
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def read(p):
 p=resolve(p)
 with(gzip.open(p,'rt')if p.suffix=='.gz'else p.open())as f:return json.load(f)
def save(p,v):
 p=Path(p);p.parent.mkdir(parents=True,exist_ok=True)
 if p.suffix=='.gz':
  with gzip.open(p,'wt',compresslevel=6)as f:json.dump(v,f,separators=(',',':'),allow_nan=False)
 else:p.write_text(json.dumps(v,indent=2,allow_nan=False))
def rec(name):return U/'native'/name/'record.json.gz'
def fixture_header(p):
 with Path(p).open('rb')as f:
  assert f.read(8)==b'DGNUM006';N,H,D,Q=struct.unpack('<4I',f.read(16));lengths=struct.unpack('<'+str(Q)+'I',f.read(4*Q))
 return dict(N=N,H=H,D=D,Q=Q,L0=lengths[0])

@contextlib.contextmanager
def atomic_zip(path,**kwargs):
                                                                       
 path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
 fd,name=tempfile.mkstemp(prefix='dg_sealed_',suffix='.zip',dir='/tmp');os.close(fd);temporary=Path(name)
 try:
  with zipfile.ZipFile(temporary,'w',**kwargs)as z:yield z
  with zipfile.ZipFile(temporary)as z:assert z.testzip()is None
  payload=temporary.read_bytes();pending=path.parent/('.sealed_'+path.name)
  with pending.open('wb')as f:f.write(payload);f.flush();os.fsync(f.fileno())
  os.replace(pending,path);os.utime(path,None)
  assert sha(path)==hashlib.sha256(payload).hexdigest()
 finally:temporary.unlink(missing_ok=True)
