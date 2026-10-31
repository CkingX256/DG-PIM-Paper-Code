                                                                            
from consumer_stats import *
import tarfile,subprocess,shutil
from raw_evidence import open_archive,verify_archive
import argparse
ap=argparse.ArgumentParser();ap.add_argument('--fresh',action='store_true');a=ap.parse_args()
out=C/'results/MAC_CHAIN_AUDIT.json';old=read(out)if out.exists()and not a.fresh else{};done={r['archive_sha256']:r for r in old.get('rows',[])}
for p in sorted(C.glob('*_native/*/record.json.gz'))+sorted(C.glob('adversarial_reference/*/record.json.gz')):
 r=read(p);h=r['archive_sha256']
 if h in done:continue
 verify_archive(p.parent,h)
 process=subprocess.Popen([str(C/'scripts/audit_mac_order'),str(r['result']['D'])],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 try:
  with open_archive(p.parent)as archive,tarfile.open(fileobj=archive,mode='r|gz')as tar:
   for item in tar:
    if item.name=='events.tsv':shutil.copyfileobj(tar.extractfile(item),process.stdin);break
   else:raise AssertionError('events missing')
  process.stdin.close();stdout=process.stdout.read();stderr=process.stderr.read();assert process.wait()==0,stderr.decode();z=json.loads(stdout);assert z['callbacks']==r['result']['qk_mac']
 except BaseException:
  process.kill();process.wait();raise
 done[h]=dict(archive_sha256=h,record=str(p.relative_to(R)),groups=z['groups'],callbacks=z['callbacks'],status=z['status'])
 if len(done)%100==0:
  out.write_text(json.dumps(dict(status='AUDIT_IN_PROGRESS',rows=list(done.values())),indent=2));print('MAC_CHAIN_AUDIT',len(done),flush=True)
out.write_text(json.dumps(dict(status='ALL_AVAILABLE_COMPLETED_ARCHIVES_PASS_ACTUAL_QK_FMA_TILE_ORDER',archives=len(done),callbacks=sum(r['callbacks']for r in done.values()),scope='Protocol check only;repeated schedules/producers do not add workload sources. Exact archive hashes permit checkpointed resume.',rows=list(done.values())),indent=2));print('MAC_CHAIN_AUDIT_COMPLETE',len(done),flush=True)
