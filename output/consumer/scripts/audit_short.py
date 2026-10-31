                                                                                    
from consumer_stats import *
import sys,subprocess,tempfile,struct,os,argparse
sys.path.insert(0,str(O/'scripts'));from campaign_records import record_path
sys.path.insert(0,str(R/'native_base/numerics'));from exact_quality import exact_metrics,controls
ap=argparse.ArgumentParser();ap.add_argument('--stored-oracles',action='store_true',help='Explicitly verify the supplied independent oracle bytes/certificates without executing the oracle');args=ap.parse_args()
controls();pairs=[];quality=[];seen=set();oracles={};callbacks=0;native_checks=[]
strongq={(t['fixture'],t['cache']):t for t in read(O/'STRONG_Q4_TASKS.json')}
strongmc={(t['fixture'],t['channels'],t['cache']):t for t in read(O/'STRONG_MC96_TASKS.json')}
for family,manifest in [('primary_native','PRIMARY_TASKS.json'),('channel_native','CHANNEL_TASKS.json')]:
 tasks=read(C/manifest);assert len(tasks)==576
 for i,t in enumerate(tasks,1):
  p=record(family,t);r,old=verify(t,p);z=r['result'];callbacks+=z['callbacks'];N=z['max_tokens'];D=z['D'];ch=t['channels'];state=t['cache'];fixture=t['fixture']
  m,d,rd,l,n=map(int,re.fullmatch(r'm(\d+)_d(\d+)_r(\d+)_l(\d+)_n(\d+)',fixture).groups());assert n==N
  if family=='primary_native':b=read(record_path('strong_q4_native',strongq[fixture,state]['name']))
  else:b=read(record_path('strong_mc_native',strongmc[fixture,ch,state]['name']))
  for baseline_kind,br in [('old_lifecycle',old),('old_canonical',b)]:
   assert br['arrays']==r['arrays'];pairs.append(dict(family=family,fixture=fixture,model=m,dataset=d,round=rd,layer=l,N=N,D=D,channels=ch,cache=state,source_block=f'd{d}_r{rd}',baseline_kind=baseline_kind,baseline=br['native_cycles']-1,candidate=r['native_cycles']-1,baseline_q0=q0(br),candidate_q0=q0(r),ratio=(br['native_cycles']-1)/(r['native_cycles']-1),old_retained=old['result']['buffered_prefill_retained_tokens'],new_retained=z['buffered_prefill_retained_tokens']))
  native_checks.append(dict(family=family,name=t['name'],callbacks=z['callbacks'],peak_scratch=z['peak_live_scratch_bytes'],physical_mac_epu_overlap_cycles=r['pipeline_audit']['physical_mac_epu_overlap_cycles'],cache_admitted_heads=z['qpipe_admitted_heads'],speculative_exp_calls=z['qpipe_spec_exp_calls'],valid_exp_calls=z['qpipe_valid_exp_calls'],recomputed_exp_calls=z['qpipe_recomputed_exp_calls'],qk_mac=z['qk_mac'],value_mac=z['value_mac']))
  key=(family,fixture,ch)
  if key not in seen:
   seen.add(key);out=(p.parent/'output.f32').read_bytes();fp=np.ascontiguousarray(np.load(resolve(t['reference']))['output'],dtype='<f4');Q,H,D=fp.shape
   if family=='primary_native':dense=(R/'output/DG_PIM_Boundary_Upgrade/results/precision_outputs'/fixture/'FP16_CONTROL.f32').read_bytes()
   else:
    root=C/'channel_oracles'/f'{fixture}__c{ch}';root.mkdir(parents=True,exist_ok=True)
    if args.stored_oracles:
     dense=(root/'dense_output.f32').read_bytes();cert=read(root/'certificate.json');assert sha(root/'dense_output.f32')==cert['dense_output.f32']
     for a,b in [('encoded_keys.f16','encoded_keys.f16'),('anchor.f32','block_anchors.f32'),('scores.f32','scores.f32'),('probability.f16','probability.f16')]:assert r['arrays'][a]==cert[b],(t['name'],a)
    else:
     with tempfile.TemporaryDirectory(prefix='consumer_oracle_',dir='/dev/shm')as td:
      subprocess.run([os.environ.get('DG_PIM_ORACLE_BINARY',str(R/'output/DG_PIM_Stream_Study/scripts/early_oracle')),str(resolve(t['input'])),str(resolve(t['raw_keys'])),'global','0','1',td,str(ch)],check=True,capture_output=True)
      td=Path(td);dense=(td/'dense_output.f32').read_bytes();(root/'dense_output.f32').write_bytes(dense);cert={x.name:sha(x)for x in td.iterdir()if x.is_file()};(root/'certificate.json').write_text(json.dumps(cert,indent=2))
      for a,b in [('encoded_keys.f16','encoded_keys.f16'),('anchor.f32','block_anchors.f32'),('scores.f32','scores.f32'),('probability.f16','probability.f16')]:assert r['arrays'][a]==cert[b],(t['name'],a)
   rb=fp.tobytes()
   for q in range(Q):
    for h in range(H):
     sl=slice((q*H+h)*D*4,(q*H+h+1)*D*4);aa=exact_metrics(out[sl],dense[sl]);bb=exact_metrics(out[sl],rb[sl]);assert aa['absolute_epsilon_pass']
     quality.append(dict(family=family,fixture=fixture,channels=ch,query=q,head=h,absolute01_pass=aa['absolute_epsilon_pass'],relative01_pass=bb['relative_epsilon_pass'],absolute_l2=aa['absolute_l2'],relative_l2=bb['relative_l2']))
  if i%96==0:print('AUDIT_CONSUMER_SHORT',family,i,576,flush=True)
summary={}
for family in ['primary_native','channel_native']:
 for state in ['existing','write']:
  for baseline in ['old_lifecycle','old_canonical']:
   rr=[r for r in pairs if r['family']==family and r['cache']==state and r['baseline_kind']==baseline]
   if family=='primary_native':summary[f'primary_{state}_{baseline}']=stats(rr)
   else:
    for ch in [1,2,4]:summary[f'channel_c{ch}_{state}_{baseline}']=stats([r for r in rr if r['channels']==ch])
qsummary={}
for family in ['primary_native','channel_native']:
 q=[r for r in quality if r['family']==family];qsummary[family]=dict(unique_query_heads=len(q),absolute01_pass=sum(r['absolute01_pass']for r in q),relative01_pass=sum(r['relative01_pass']for r in q),worst_relative_l2=max(r['relative_l2']for r in q),worst_absolute_l2=max(r['absolute_l2']for r in q))
csvwrite(C/'results/short_pairs.csv',pairs);csvwrite(C/'results/short_quality.csv',quality);csvwrite(C/'results/short_resources.csv',native_checks)
(C/'results/SHORT_AUDIT.json').write_text(json.dumps(dict(status='ALL1152_SHORT_AND_CHANNEL_NATIVE_CASES_SIX_ARRAYS_AND_INDEPENDENT_QUALITY_PASS',runs=1152,callbacks=callbacks,summary=summary,quality=qsummary,blocks='12text source blocks carry all models,layers,lengths;10000draws seed20261025;design sources reused,not independent holdout',scope='NewLazy outputs retain exactly the original absolute0.1 contract;the separate relative1% diagnostic is reported with every failure. No Dense quality is substituted for Lazy timing.'),indent=2));print('CONSUMER_SHORT_AUDIT_COMPLETE',1152,flush=True)
