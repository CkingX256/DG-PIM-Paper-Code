                                                                         
from common import *
import numpy as np,tarfile
def decode(t):
 p=resolve(t['input']);meta=fixture_header(p);N,H,D,Q=(meta[k]for k in ['N','H','D','Q']);L0=meta['L0']
 with p.open('rb')as f:
  f.seek(24);lengths=np.frombuffer(f.read(4*Q),dtype='<u4').copy();raw=np.frombuffer(f.read(),dtype='<f2')
 assert np.array_equal(lengths,np.arange(L0,N+1,dtype=np.uint32))and len(lengths)==Q
 assert raw.size==2*N*D+Q*H*D
 v=raw[N*D:2*N*D].reshape(N,D).copy();q=raw[2*N*D:].reshape(Q,H,D).copy()
 ref=read(rec('base__'+t['fixture']));archive=rec('base__'+t['fixture']).parent/'raw.tar.gz';image=U/'numeric_images'/t['fixture']/'encoded_keys.f16'
 if image.exists():encoded=image.read_bytes()
 else:
  assert sha(archive)==ref['archive_sha256']
  with tarfile.open(archive)as tar:encoded=tar.extractfile('encoded_keys.f16').read()
 assert hashlib.sha256(encoded).hexdigest()==ref['arrays']['encoded_keys.f16']
 k=np.frombuffer(encoded,dtype='<f2').reshape(N,D).copy()
 assert all(np.isfinite(x).all()for x in [q,k,v])
 return meta,lengths,q,k,v,dict(fixture_sha256=sha(p),encoded_key_sha256=ref['arrays']['encoded_keys.f16'],query_fp16_sha256=hashlib.sha256(q.tobytes()).hexdigest(),value_fp16_sha256=hashlib.sha256(v.tobytes()).hexdigest())

if __name__=='__main__':
 import argparse
 ap=argparse.ArgumentParser();ap.add_argument('--available',action='store_true');a=ap.parse_args();rows=[]
 for t in read(U/'ORIGINAL_BASE_TASKS.json')+read(U/'NEW_BASE_TASKS.json'):
  if not rec('base__'+t['fixture']).exists():assert a.available;continue
  meta,l,q,k,v,ids=decode(t)
  assert q.shape==(meta['Q'],meta['H'],meta['D'])and k.shape==v.shape==(meta['N'],meta['D'])
  rows.append(dict(fixture=t['fixture'],**meta,**ids,queries_with_verified_causal_prefix=len(l)))
 if not a.available:assert len(rows)==22
 save(U/'results/EXTERNAL_INPUT_AUDIT.json',dict(status='PARTIAL_PROGRESS_ONLY'if a.available else'ALL22_INPUT_BYTE_AND_CAUSAL_PREFIX_CHECKS_PASS',contexts=rows,scope='CPU input/schema validation only. Neither GPU kernel execution nor GPU numerical/timing qualification has occurred.'))
 print('EXTERNAL_INPUT_CHECKS',len(rows),flush=True)
