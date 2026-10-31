                                                                                   
from common import *
import tarfile,argparse
ap=argparse.ArgumentParser();ap.add_argument('--available',action='store_true');a=ap.parse_args();count=0
for t in read(U/'ORIGINAL_BASE_TASKS.json')+read(U/'NEW_BASE_TASKS.json'):
 rp=rec(t['name'])
 if not rp.exists():assert a.available;continue
 r=read(rp);d=U/'numeric_images'/t['fixture'];d.mkdir(parents=True,exist_ok=True);wanted={'encoded_keys.f16','anchor.f32'}
 for name in list(wanted):
  if(d/name).exists():assert sha(d/name)==r['arrays'][name];wanted.remove(name)
 if wanted:
  assert sha(rp.parent/'raw.tar.gz')==r['archive_sha256']
  with tarfile.open(rp.parent/'raw.tar.gz','r|gz')as tar:
   for member in tar:
    if member.name in wanted:
     data=tar.extractfile(member).read();assert hashlib.sha256(data).hexdigest()==r['arrays'][member.name];(d/member.name).write_bytes(data);wanted.remove(member.name)
     if not wanted:break
 assert not wanted;count+=1
if not a.available:assert count==22
save(U/'results/NUMERIC_IMAGE_MATERIALIZATION.json',dict(contexts=count,status='PARTIAL_PROGRESS_ONLY'if a.available else'ALL22_HASH_VERIFIED_ENCODED_IMAGES',scope='Canonical encoded Key and anchor bytes only; physical command logs remain separate.'))
print('NUMERIC_IMAGES',count,flush=True)
