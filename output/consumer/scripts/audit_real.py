                                                                               
from consumer_stats import *
import sys
sys.path.insert(0,str(R/'native_base/numerics'));from exact_quality import exact_metrics,controls
controls();tasks=read(C/'REAL_DENSE_TASKS.json');assert len(tasks)==20
runs={};callbacks=0;rows=[];checks=[];heads=[];resources=[]
for t in tasks:
 p=record('real_dense_native',t);r,old=verify(t,p);callbacks+=r['result']['callbacks'];runs[t['fixture'],t['configuration']]=(t,r,old,p)
 fixtures=None
for fixture in sorted({t['fixture']for t in tasks}):
 t,a,old,p=runs[fixture,'L'];_,b,oldb,pb=runs[fixture,'R_LOCAL'];z=a['result'];Q,H,D=(z[k]for k in ['Q','H','D']);assert a['arrays']==b['arrays']==old['arrays']==oldb['arrays']
 assert z['buffered_prefill_retained_tokens']==b['result']['buffered_prefill_retained_tokens'];S=z['buffered_prefill_retained_tokens'];columns=['RD','WR','MACAB','MACSB','MACPB','WRGB','MVSB','MVGB','SFM','SETM','SETH'];assert sum(b['physical_audit']['commands'].get(k,0)-a['physical_audit']['commands'].get(k,0)for k in columns)==D*(S+Q-1)//8
 for label,br,cr in [('consumer_over_lifecycle',old,a),('combined_over_canonical',oldb,a),('producer_with_new_consumer',b,a),('consumer_over_canonical',oldb,b)]:
  row=dict(fixture=fixture,model=int(fixture.split('_m')[1].split('_')[0]),Q=Q,H=H,D=D,L0=z['steps'][0]['length'],source_block='context'+fixture.split('_c')[1].split('_')[0],comparison=label,baseline=br['native_cycles']-1,candidate=cr['native_cycles']-1,ratio=(br['native_cycles']-1)/(cr['native_cycles']-1),baseline_q0=q0(br),candidate_q0=q0(cr));rows.append(row)
  for k in [4,16,64,256,1024,2048]:
   if k<=Q:checks.append(dict(**{x:row[x]for x in ['fixture','model','Q','H','D','L0','source_block','comparison']},checkpoint=k,baseline=br['result']['steps'][k-1]['end']-1,candidate=cr['result']['steps'][k-1]['end']-1,ratio=(br['result']['steps'][k-1]['end']-1)/(cr['result']['steps'][k-1]['end']-1)))
                                                                            
  start=Q-256;bt=br['result']['steps'][start-1]['end'];ct=cr['result']['steps'][start-1]['end'];checks.append(dict(**{x:row[x]for x in ['fixture','model','Q','H','D','L0','source_block','comparison']},checkpoint='last256',baseline=br['native_cycles']-bt,candidate=cr['native_cycles']-ct,ratio=(br['native_cycles']-bt)/(cr['native_cycles']-ct)))
 out=(p.parent/'output.f32').read_bytes();dense=(O/'real_oracle'/fixture/'dense_output.f32').read_bytes();assert out==dense==(pb.parent/'output.f32').read_bytes()
 ref=np.ascontiguousarray(np.load(resolve(t['reference']))['output'],dtype='<f4');assert ref.shape==(Q,H,D);rb=ref.tobytes()
 for q in range(Q):
  for h in range(H):
   sl=slice((q*H+h)*D*4,(q*H+h+1)*D*4);v=exact_metrics(out[sl],rb[sl]);assert v['relative_epsilon_pass'];heads.append(dict(fixture=fixture,Q=Q,query=q,head=h,relative01_pass=v['relative_epsilon_pass'],relative_l2=v['relative_l2'],absolute_l2=v['absolute_l2'],squared_error_units=v['squared_error_units_2p298'],reference_norm_units=v['squared_reference_norm_units_2p298']))
 for producer,r in [('L',a),('R_LOCAL',b)]:
  zz=r['result'];total=H*sum(x['length']for x in zz['steps']);resources.append(dict(fixture=fixture,producer=producer,Q=Q,H=H,D=D,unique_score_operands=total,cache_admitted_heads=zz['qpipe_admitted_heads'],speculative_exp_calls=zz['qpipe_spec_exp_calls'],valid_exp_calls=zz['qpipe_valid_exp_calls'],recomputed_exp_calls=zz['qpipe_recomputed_exp_calls'],wasted_exp_calls=zz['qpipe_spec_exp_calls']-zz['qpipe_valid_exp_calls'],reuse_fraction=zz['qpipe_valid_exp_calls']/total,physical_mac_epu_overlap_cycles=r['pipeline_audit']['physical_mac_epu_overlap_cycles'],qpipe_epu_cycles=zz['qpipe_epu_cycles'],peak_scratch=zz['peak_live_scratch_bytes'],cache_buffer_bytes=zz['qpipe_additional_payload_bytes'],predictor_bytes=zz['qpipe_predictor_bytes'],old_retained=old['result']['buffered_prefill_retained_tokens'],new_retained=zz['buffered_prefill_retained_tokens']))
 print('AUDIT_EXACT_CONSUMER_REAL',fixture,Q,flush=True)
summary={}
for Q in [1024,2048]:
 for label in ['consumer_over_lifecycle','combined_over_canonical','producer_with_new_consumer','consumer_over_canonical']:
  summary[f'Q{Q}_{label}']=stats([r for r in rows if r['Q']==Q and r['comparison']==label])
for model in range(4):
 for label in ['consumer_over_lifecycle','combined_over_canonical','producer_with_new_consumer']:
  summary[f'Q1024_m{model}_{label}']=stats([r for r in rows if r['Q']==1024 and r['model']==model and r['comparison']==label],False)
tails={}
for Q in [1024,2048]:
 for label in ['consumer_over_lifecycle','combined_over_canonical']:
  rr=[dict(r,baseline_q0=1,candidate_q0=1)for r in checks if r['Q']==Q and r['comparison']==label and r['checkpoint']=='last256'];tails[f'Q{Q}_{label}']=stats(rr,False);tails[f'Q{Q}_{label}'].pop('first_output_gm')
for name,rs in [('real_pairs.csv',rows),('real_checkpoints.csv',checks),('real_quality.csv',heads),('real_resources.csv',resources)]:csvwrite(C/'results'/name,rs)
q=dict(unique_query_heads=len(heads),relative01_pass=sum(r['relative01_pass']for r in heads),worst_relative_l2=max(r['relative_l2']for r in heads),all_native_outputs_byte_identical_to_independent_dense=True)
assert q['unique_query_heads']==38912 and q['relative01_pass']==38912
(C/'results/REAL_AUDIT.json').write_text(json.dumps(dict(status='ALL20_REAL_DENSE_NATIVE_RUNS_SIX_ARRAYS_EXACT_AND_ALL38912_HEADS_PASS_RELATIVE1PERCENT',runs=20,callbacks=callbacks,summary=summary,tail256=tails,quality=q,uncertainty='Two text slices per horizon;block bootstrap carries models together and is descriptive only,not population CI. Last256windows are within-trace incremental time,not new independent samples.',scope='Real captured postRoPE KV owner. Dense timing only. Frozen fullmean producer unchanged. All storage,predictor/setup,fallback,recompute,refresh/native commands charged; no projected2x.'),indent=2));print('REAL_CONSUMER_AUDIT_COMPLETE',20,38912,flush=True)
