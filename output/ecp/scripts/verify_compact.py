                                                                                 
from common import *
import argparse,shutil
ap=argparse.ArgumentParser();ap.add_argument('--materialize',action='store_true',help='Recreate hash-identical duplicate numerical files');a=ap.parse_args()
manifest=read(U/'SOURCE_BUNDLE_MANIFEST.json');checked=0
for entry in manifest['files']:
 p=R/entry['path'];assert p.is_file()and p.stat().st_size==entry['bytes']and sha(p)==entry['sha256'],('Package byte mismatch',entry['path']);checked+=1
for x in read(U/'DEDUPLICATED_NUMERIC_FILES.json'):
 source=R/x['canonical'];target=R/x['path'];assert sha(source)==x['sha256']
 if target.exists():assert sha(target)==x['sha256']
 elif a.materialize:target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
reg=read(U/'REGISTRATION.json');tasks=read(U/'ALL_REGISTERED_TASKS.json');assert len(tasks)==291
for key,h in read(U/'PROTOCOL.json')['source'].items():assert sha(U/key)==h
callbacks=0
for t in tasks:
 r=read(rec(t['name']));base=read(rec('base__'+t['fixture']).parent/'ARRAY_CERTIFICATE.json');z=r['result'];assert r['task']==t and r['arrays']==base['arrays']
 assert r['physical_audit']['status']=='PASS'and r['physical_audit']['events']==z['submitted']==z['callbacks']
 assert sum(p['cycles']for p in z['phases'])+1==z['actor_end_cycle']
 assert z['peak_live_scratch_bytes']<=t['capacity']and r['pipeline_audit']['physical_readout_epu_overlap_cycles']==0
 assert r['identities']['input']==sha(resolve(t['input']))and r['identities']['raw_keys']==sha(resolve(t['raw_keys']))
 callbacks+=z['callbacks']
 del r,z,base
release=read(U/'results/RELEASE_AUDIT.json');assert callbacks==release['callbacks']and release['registered_executions']==291
save(U/'results/COMPACT_VERIFICATION.json',dict(status='COMPACT_BYTES_AND291_RECORDED_ATTESTATIONS_VERIFIED',files_checked=checked,registered_cases=291,callbacks=callbacks,duplicates_materialized=a.materialize,scope='This verifies package bytes and committed audit attestations. Full native command/event archives are separate. Fresh physical verification requires a native replay or the original raw archives; run make_replay_manifest.py without overwriting evidence. Regenerate independent numerical oracles with audit_quality.py --recompute.'))
print('COMPACT_PACKAGE_VERIFIED',checked,'files,291 recorded cases; raw replay remains explicit',flush=True)
