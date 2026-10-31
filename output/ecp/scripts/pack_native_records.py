



   
from common import *
import zipfile
folder=R/'output/experiment_parts';folder.mkdir(exist_ok=True);statefile=U/'NATIVE_VOLUME_STATE.json';state=read(statefile)if statefile.exists()else dict(cases={},volumes=[]);limit=45*1024*1024
tasks=read(U/'ALL_REGISTERED_TASKS.json')+read(U/'CLEAN_BUILD_CONTROLS.json');ready=[]
for t in tasks:
 p=rec(t['name'])
 if t['name']in state['cases']:
  assert sha(p)==state['cases'][t['name']]['record_sha256'];continue
 if p.exists():
  cp=p.parent/'ARRAY_CERTIFICATE.json';assert cp.exists();c=read(cp);assert sha(p)==c['record_sha256'];ready.append((t,p,cp))
chunks=[];current=[];size=0
for item in ready:
 amount=sum(p.stat().st_size+256 for p in item[1:])
 assert amount<limit
 if current and size+amount>limit:chunks.append(current);current=[];size=0
 current.append(item);size+=amount
if current:chunks.append(current)
for chunk in chunks:
 index=len(state['volumes'])+1;p=folder/f'DG_PIM_Native_Evidence_{index:02}.zip'
 assert not p.exists()
 with atomic_zip(p,compression=zipfile.ZIP_STORED)as z:
  for t,r,c in chunk:
   for member in [r,c]:z.write(member,str(member.relative_to(R)))
 with zipfile.ZipFile(p)as z:assert z.testzip()is None
 assert p.stat().st_size<=limit
 for t,r,c in chunk:state['cases'][t['name']]=dict(volume=p.name,record_sha256=sha(r),certificate_sha256=sha(c))
 state['volumes'].append(dict(path=str(p.relative_to(R)),bytes=p.stat().st_size,sha256=sha(p),cases=len(chunk)));save(statefile,state)
print('NATIVE_VOLUMES',len(state['volumes']),'committed cases',len(state['cases']),flush=True)
