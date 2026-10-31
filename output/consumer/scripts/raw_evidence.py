                                                                          
from pathlib import Path
from contextlib import contextmanager
import hashlib,json,zipfile
R=Path(__file__).resolve().parents[3]
_checked={}
def digest(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def location(case):
 case=Path(case);raw=case/'raw.tar.gz'
 if raw.exists():return dict(kind='case_archive',archive_bytes=raw.stat().st_size,path=raw)
 p=case/'RAW_LOCATION.json';assert p.exists(),('Raw evidence unavailable;extract its catalogued volume',case)
 z=json.loads(p.read_text());assert z['kind']=='VERIFIED_CONSUMER_RAW_VOLUME'
 v=R/z['volume'];assert v.is_file(),('Required raw volume missing',v)
 key=(str(v),v.stat().st_mtime_ns,v.stat().st_size,z['volume_sha256'])
 if key not in _checked:assert digest(v)==z['volume_sha256'];_checked[key]=True
 return dict(z,path=v)
@contextmanager
def open_archive(case):
 z=location(case)
 if z['kind']=='case_archive':
  with z['path'].open('rb')as f:yield f
 else:
  with zipfile.ZipFile(z['path'])as v:
   info=v.getinfo(z['member']);assert info.file_size==z['archive_bytes']
   with v.open(info)as f:yield f
def verify_archive(case,expected_sha):
 z=location(case);h=hashlib.sha256();n=0
 with open_archive(case)as f:
  while b:=f.read(1048576):h.update(b);n+=len(b)
 assert h.hexdigest()==expected_sha and n==z['archive_bytes'],case
 return z
