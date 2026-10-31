



   
from consumer_stats import *
import sys
sys.path.insert(0,str(R/'native_base/numerics'))
from exact_quality import exact_metrics,controls
controls()
cohorts={};callbacks=0;resources=[]
for variant,manifest,family in [('full_cache','READOUT_REAL_TASKS.json','readout_real_native'),('light_pipeline','READOUT_LIGHT_TASKS.json','readout_light_native')]:
 tasks=read(C/manifest);assert len(tasks)==20;cohorts[variant]={}
 for t in tasks:
  p=record(family,t);r,old=verify(t,p);callbacks+=r['result']['callbacks'];assert r['result']['qk_readout_first']
  cohorts[variant][t['fixture'],t['configuration']]=(t,r,old,p)
  z=r['result'];n=z['max_tokens'];g=(n+15)//16
  expected_cache=(4*n+4*g+(g+7)//8)if z['qk_exact_exp_cache']else 0
  assert z['base_peak_live_scratch_bytes']-old['result']['base_peak_live_scratch_bytes']==expected_cache+64
  total=z['H']*sum(s['length']for s in z['steps'])
  assert z['exact_exp_calls']==total+z['qpipe_spec_exp_calls']-z['qpipe_valid_exp_calls']
  resources.append(dict(fixture=t['fixture'],variant=variant,producer=t['configuration'],Q=z['Q'],D=z['D'],H=z['H'],score_operands=total,exact_exp_calls=z['exact_exp_calls'],spec_exp_calls=z['qpipe_spec_exp_calls'],reuse_exp_calls=z['qpipe_valid_exp_calls'],recomputed_exp_calls=z['qpipe_recomputed_exp_calls'],wasted_exp_calls=z['qpipe_spec_exp_calls']-z['qpipe_valid_exp_calls'],cache_payload_bytes=expected_cache,charged_persistent_predictor_bytes=64,physical_mac_epu_overlap_cycles=r['pipeline_audit']['physical_mac_epu_overlap_cycles'],qpipe_epu_cycles=z['qpipe_epu_cycles'],peak_scratch=z['peak_live_scratch_bytes'],old_retained=old['result']['buffered_prefill_retained_tokens'],new_retained=z['buffered_prefill_retained_tokens']))
rows=[];checks=[];query_rows=[];heads=[]
fixtures=sorted({k[0]for k in cohorts['full_cache']})
for fixture in fixtures:
 t,fr,oldL,fp=cohorts['full_cache'][fixture,'L'];oldB=cohorts['full_cache'][fixture,'R_LOCAL'][2]
 D,H,Q=(fr['result'][x]for x in ['D','H','Q']);model=int(fixture.split('_m')[1].split('_')[0]);block='context'+fixture.split('_c')[1].split('_')[0]
 out=(fp.parent/'output.f32').read_bytes();assert out==(O/'real_oracle'/fixture/'dense_output.f32').read_bytes()
 ref=np.ascontiguousarray(np.load(resolve(t['reference']))['output'],dtype='<f4');assert ref.shape==(Q,H,D);rb=ref.tobytes()
 for q in range(Q):
  for h in range(H):
   sl=slice((q*H+h)*D*4,(q*H+h+1)*D*4);v=exact_metrics(out[sl],rb[sl]);assert v['relative_epsilon_pass']
   heads.append(dict(fixture=fixture,Q=Q,query=q,head=h,relative01_pass=v['relative_epsilon_pass'],relative_l2=v['relative_l2'],absolute_l2=v['absolute_l2'],squared_error_units=v['squared_error_units_2p298'],reference_norm_units=v['squared_reference_norm_units_2p298']))
 for variant in ['full_cache','light_pipeline','profile_policy']:
  cache=variant=='full_cache'or variant=='profile_policy'and D==128
  _,a,_,ap=cohorts['full_cache'if cache else'light_pipeline'][fixture,'L'];_,b,_,bp=cohorts['full_cache'if cache else'light_pipeline'][fixture,'R_LOCAL']
  assert a['arrays']==b['arrays']==fr['arrays']and (ap.parent/'output.f32').read_bytes()==out==(bp.parent/'output.f32').read_bytes()
  z=a['result'];S=z['buffered_prefill_retained_tokens'];columns=['RD','WR','MACAB','MACSB','MACPB','WRGB','MVSB','MVGB','SFM','SETM','SETH']
  assert S==b['result']['buffered_prefill_retained_tokens']
  assert sum(b['physical_audit']['commands'].get(k,0)-a['physical_audit']['commands'].get(k,0)for k in columns)==D*(S+Q-1)//8
  for comparison,br,cr in [('consumer_over_lifecycle',oldL,a),('combined_over_canonical',oldB,a),('producer_with_new_consumer',b,a),('consumer_over_canonical',oldB,b)]:
   row=dict(fixture=fixture,model=model,D=D,H=H,Q=Q,variant=variant,cache_enabled=cache,comparison=comparison,source_block=block,baseline=br['native_cycles']-1,candidate=cr['native_cycles']-1,ratio=(br['native_cycles']-1)/(cr['native_cycles']-1),baseline_q0=q0(br),candidate_q0=q0(cr));rows.append(row)
   meta={x:row[x]for x in ['fixture','model','D','H','Q','variant','cache_enabled','comparison','source_block']}
   for k in [4,16,64,256,1024,2048]:
    if k<=Q:checks.append(dict(**meta,checkpoint=str(k),baseline=br['result']['steps'][k-1]['end']-1,candidate=cr['result']['steps'][k-1]['end']-1,ratio=(br['result']['steps'][k-1]['end']-1)/(cr['result']['steps'][k-1]['end']-1)))
   start=Q-256;bt=br['result']['steps'][start-1]['end'];ct=cr['result']['steps'][start-1]['end'];checks.append(dict(**meta,checkpoint='last256',baseline=br['native_cycles']-bt,candidate=cr['native_cycles']-ct,ratio=(br['native_cycles']-bt)/(cr['native_cycles']-ct)))
   for q in range(start,Q):
    bd=br['result']['steps'][q]['end']-br['result']['steps'][q-1]['end'];cd=cr['result']['steps'][q]['end']-cr['result']['steps'][q-1]['end'];query_rows.append(dict(fixture=fixture,Q=Q,variant=variant,comparison=comparison,query=q,baseline=bd,candidate=cd,ratio=bd/cd))
 print('READOUT_EXACT_REAL_AUDIT',fixture,flush=True)
summary={};tails={};qsummary={}
for variant in ['full_cache','light_pipeline','profile_policy']:
 for Q in [1024,2048]:
  for comparison in ['consumer_over_lifecycle','combined_over_canonical','producer_with_new_consumer','consumer_over_canonical']:
   key=f'{variant}_Q{Q}_{comparison}';summary[key]=stats([r for r in rows if(r['variant'],r['Q'],r['comparison'])==(variant,Q,comparison)])
   tt=[dict(r,baseline_q0=1,candidate_q0=1)for r in checks if(r['variant'],r['Q'],r['comparison'],r['checkpoint'])==(variant,Q,comparison,'last256')];tails[key]=stats(tt,False);tails[key].pop('first_output_gm')
   qr=[r for r in query_rows if(r['variant'],r['Q'],r['comparison'])==(variant,Q,comparison)];ratios=np.asarray([r['ratio']for r in qr]);qsummary[key]=dict(query_retirements=len(qr),wins=sum(r['baseline']>r['candidate']for r in qr),ties=sum(r['baseline']==r['candidate']for r in qr),losses=sum(r['baseline']<r['candidate']for r in qr),minimum=float(ratios.min()),p05=float(np.quantile(ratios,.05)),median=float(np.median(ratios)),scope='Correlated observations,not independent sources or a universal/per-query floor')
quality=dict(unique_query_heads=len(heads),relative01_pass=sum(r['relative01_pass']for r in heads),worst_relative_l2=max(r['relative_l2']for r in heads),all_native_outputs_byte_identical_to_independent_dense=True);assert quality['unique_query_heads']==quality['relative01_pass']==38912
for name,rs in [('readout_variant_pairs.csv',rows),('readout_variant_checkpoints.csv',checks),('readout_tail_query_ratios.csv',query_rows),('readout_quality.csv',heads),('readout_resources.csv',resources)]:csvwrite(C/'results'/name,rs)
(C/'results/READOUT_AUDIT.json').write_text(json.dumps(dict(status='ALL40_READOUT_FIRST_NATIVE_CASES_SIX_ARRAYS_EXACT_AND_38912_HEADS_PASS',runs=40,callbacks=callbacks,quality=quality,summary=summary,tail_windows=tails,per_query_tail=qsummary,admission='Keep original consumer atQ4. Readout-first+fullcache is the measured default for the evaluated resident,single-channel long-owner profiles. Eager/light/profile alternatives remain reported. The earlier D128-only cache recipe is a transferred design-source heuristic and is not preferred for this final schedule. No independent holdout or per-context runtime oracle.',scope='Ten finite captured KV-owner contexts;teacher forced,not complete model latency. Both producer arms receive identical consumer configurations. Native rates unchanged;bank selection is an architectural extension without technology-matched silicon synthesis.'),indent=2))
print('READOUT_FULL_AUDIT_COMPLETE',40,38912,flush=True)
