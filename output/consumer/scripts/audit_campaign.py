                                                                                           
from consumer_stats import *
from raw_evidence import verify_archive
build=read(C/'FINAL_BUILD.json');assert sha(C/'build_final/bin/libramulator.so')==build['library_sha256']
for p,h in build['source_sha256'].items():assert sha(R/p)==h,p
groups=[('primary_native','PRIMARY_TASKS.json'),('channel_native','CHANNEL_TASKS.json'),('real_dense_native','REAL_DENSE_TASKS.json'),('ablation_native','ABLATION_TASKS.json'),('sensitivity_native','SENSITIVITY_TASKS.json'),('disabled_native','DISABLED_TASKS.json'),('pipeline_only_native','PIPELINE_ONLY_TASKS.json')]
rows=[];seen={};callbacks=0;placements=0;duplicates=[]
for family,manifest in groups:
 for t in read(C/manifest):
  p=record(family,t);r,old=verify(t,p);assert r['identities']['library']==build['library_sha256'];assert r['identities']['input']==old['identities']['input']and r['identities']['raw_keys']==old['identities']['raw_keys']
  identity=hashlib.sha256(json.dumps(t,sort_keys=True,separators=(',',':')).encode()).hexdigest();placements+=1
  if identity in seen:
   previous=seen[identity];assert r['archive_sha256']==previous['archive_sha256'];duplicates.append(dict(family=family,name=t['name'],record=str(p.relative_to(R)),canonical_record=previous['record']));continue
  raw=verify_archive(p.parent,r['archive_sha256']);z=r['result'];D,H,Q,N=(z[k]for k in ['D','H','Q','max_tokens']);L0=z['steps'][0]['length'];S=z['buffered_prefill_retained_tokens'];c=t['channels'];assert N==L0+Q-1
  nk=H*((D+63)//64)*sum((L0+q+(32 if z['token_packed']else 16)-1)//(32 if z['token_packed']else 16)for q in range(Q));assert nk==z['qk_mac']
  ns=8*D*(L0-S)//32;floor=max((N*D+7)//8,(ns+c-1)//c)+(nk+c-1)//c;assert r['native_cycles']-1>=floor
  source=z['initial_source_write_commands']+z['append_source_write_commands']+z['encoder_raw_encode_read_commands']+z['encoder_raw_mean_read_commands'];assert source>=ns
  if z['lifecycle_source_spill']and z['append_source_ingress_mode']=='forward':assert source==ns
  total=H*sum(x['length']for x in z['steps']);assert z['exact_exp_calls']==total+z['qpipe_spec_exp_calls']-z['qpipe_valid_exp_calls']
  for n,v in r['unchanged_array_references'].items():assert v['sha256']==r['arrays'][n]==old['arrays'][n]
  row=dict(case_id=identity,family=family,name=t['name'],fixture=t['fixture'],record=str(p.relative_to(R)),archive=str((p.parent/'raw.tar.gz').relative_to(R)),archive_sha256=r['archive_sha256'],archive_bytes=raw['archive_bytes'],callbacks=z['callbacks'],native_elapsed=r['native_cycles']-1,issue_floor=floor,six_arrays_exact=True,peak_scratch=z['peak_live_scratch_bytes']);seen[identity]=row;rows.append(row);callbacks+=z['callbacks']
  if len(rows)%200==0:print('EXTENSION_CAMPAIGN_AUDIT',len(rows),1220,flush=True)
assert len(rows)==1220 and placements==1224 and len(duplicates)==4
result=dict(status='ALL1220_UNIQUE_REGISTERED_EXTENSION_CASES_AUDITED',unique_cases=1220,scientific_cases=1208,disabled_controls=12,placements=placements,reused_placements=4,callbacks=callbacks,archive_bytes=sum(r['archive_bytes']for r in rows),native_issue_floor_pass=1220,six_array_matches=1220,peak_scratch=max(r['peak_scratch']for r in rows),rows=rows,duplicate_placements=duplicates,scope='Existing cases are not new source samples. Four native synthetic references/candidates and four invalid configuration controls are additional protocol tests,not workload cohorts.')
(C/'results/FINAL_CAMPAIGN_AUDIT.json').write_text(json.dumps(result,indent=2));print('FINAL_EXTENSION_AUDIT_COMPLETE',1220,callbacks,flush=True)
