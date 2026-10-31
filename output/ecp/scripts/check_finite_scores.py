



   
from common import *
import tarfile,numpy as np
rows=[]
for t in read(U/'ORIGINAL_BASE_TASKS.json')+read(U/'NEW_BASE_TASKS.json'):
 cert=read(rec(t['name']).parent/'ARRAY_CERTIFICATE.json')
 with resolve(t['input']).open('rb')as f:
  assert f.read(8)==b'DGNUM006';N,H,D,Q=struct.unpack('<4I',f.read(16));lengths=struct.unpack('<'+'I'*Q,f.read(4*Q))
 count=0;found=False;digest=hashlib.sha256()
 with tarfile.open(rec(t['name']).parent/'raw.tar.gz','r|gz')as archive:
  for member in archive:
   if member.name!='scores.f32':continue
   assert member.size==Q*H*N*4
   source=archive.extractfile(member)
   for q,L in enumerate(lengths):
    assert 0<L<=N
    for h in range(H):
     raw=source.read(4*N);assert len(raw)==4*N;digest.update(raw)
     active=np.frombuffer(raw,dtype='<u4',count=L)
     assert not np.any((active&0x7f800000)==0x7f800000),('Nonfinite active score',t['fixture'],q,h)
     count+=L
   assert not source.read(1);found=True;break
 assert found and digest.hexdigest()==cert['arrays']['scores.f32']
 rows.append(dict(fixture=t['fixture'],active_scores=count,finite_active_scores=count,score_sha256=digest.hexdigest()))
 print('FINITE_SCORE_PRECONDITION',t['fixture'],count,flush=True)
save(U/'results/FINITE_SCORE_PRECONDITION.json',dict(status='ALL22_HASH_BOUND_CAUSAL_SCORE_IMAGES_FINITE',contexts=rows,total_active_scores=sum(x['active_scores']for x in rows),scope='Valid causal score positions only; padding is excluded. All291 variants inherit the identical score image through the native six-array checks. No extra independent sample count.'))
