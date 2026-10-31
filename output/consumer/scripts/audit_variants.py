                                                                                     
from consumer_stats import *
full_tasks=read(C/'REAL_DENSE_TASKS.json');light_tasks=read(C/'PIPELINE_ONLY_TASKS.json');assert len(light_tasks)==20
full={(t['fixture'],t['configuration']):(t,read(record('real_dense_native',t)))for t in full_tasks};light={};rows=[];checks=[];query_rows=[];callbacks_new=0
reused={r['name']for r in read(C/'PIPELINE_ONLY_REGISTRATION.json')['reuse']}
for t in light_tasks:
 p=record('pipeline_only_native',t);r,old=verify(t,p);light[t['fixture'],t['configuration']]=(t,r)
 if t['name']not in reused:callbacks_new+=r['result']['callbacks']
for fixture in sorted({t['fixture']for t in full_tasks}):
 fr=full[fixture,'L'][1];D,H,Q=(fr['result'][x]for x in ['D','H','Q']);model=int(fixture.split('_m')[1].split('_')[0]);oldL=read(full[fixture,'L'][0]['expected_record']);oldB=read(full[fixture,'R_LOCAL'][0]['expected_record'])
 for variant in ['full_cache','light_pipeline','profile_policy']:
  use_cache=variant=='full_cache'or variant=='profile_policy'and D==128
  source=full if use_cache else light;a=source[fixture,'L'][1];b=source[fixture,'R_LOCAL'][1]
  assert a['arrays']==b['arrays']==fr['arrays']
  for comparison,br,cr in [('consumer_over_lifecycle',oldL,a),('combined_over_canonical',oldB,a),('producer_with_new_consumer',b,a)]:
   row=dict(fixture=fixture,model=model,D=D,H=H,Q=Q,variant=variant,cache_enabled=use_cache,comparison=comparison,source_block='context'+fixture.split('_c')[1].split('_')[0],baseline=br['native_cycles']-1,candidate=cr['native_cycles']-1,ratio=(br['native_cycles']-1)/(cr['native_cycles']-1),baseline_q0=q0(br),candidate_q0=q0(cr));rows.append(row)
   for k in [4,16,64,256,1024,2048]:
    if k<=Q:checks.append(dict(**{x:row[x]for x in ['fixture','model','D','H','Q','variant','cache_enabled','comparison','source_block']},checkpoint=str(k),baseline=br['result']['steps'][k-1]['end']-1,candidate=cr['result']['steps'][k-1]['end']-1,ratio=(br['result']['steps'][k-1]['end']-1)/(cr['result']['steps'][k-1]['end']-1)))
   start=Q-256;bt=br['result']['steps'][start-1]['end'];ct=cr['result']['steps'][start-1]['end'];checks.append(dict(**{x:row[x]for x in ['fixture','model','D','H','Q','variant','cache_enabled','comparison','source_block']},checkpoint='last256',baseline=br['native_cycles']-bt,candidate=cr['native_cycles']-ct,ratio=(br['native_cycles']-bt)/(cr['native_cycles']-ct)))
   for q in range(start,Q):
    bd=br['result']['steps'][q]['end']-br['result']['steps'][q-1]['end'];cd=cr['result']['steps'][q]['end']-cr['result']['steps'][q-1]['end'];query_rows.append(dict(fixture=fixture,Q=Q,variant=variant,comparison=comparison,query=q,baseline=bd,candidate=cd,ratio=bd/cd))
summary={};tails={};qsummary={}
for variant in ['full_cache','light_pipeline','profile_policy']:
 for Q in [1024,2048]:
  for comparison in ['consumer_over_lifecycle','combined_over_canonical','producer_with_new_consumer']:
   key=f'{variant}_Q{Q}_{comparison}';rr=[r for r in rows if(r['variant'],r['Q'],r['comparison'])==(variant,Q,comparison)];summary[key]=stats(rr)
   tt=[dict(r,baseline_q0=1,candidate_q0=1)for r in checks if(r['variant'],r['Q'],r['comparison'],r['checkpoint'])==(variant,Q,comparison,'last256')];tails[key]=stats(tt,False);tails[key].pop('first_output_gm')
   qr=[r for r in query_rows if(r['variant'],r['Q'],r['comparison'])==(variant,Q,comparison)];ratios=np.asarray([r['ratio']for r in qr]);qsummary[key]=dict(query_retirements=len(qr),wins=sum(r['baseline']>r['candidate']for r in qr),ties=sum(r['baseline']==r['candidate']for r in qr),losses=sum(r['baseline']<r['candidate']for r in qr),minimum=float(ratios.min()),p05=float(np.quantile(ratios,.05)),median=float(np.median(ratios)),scope='Correlated per-query observations,not independent samples or population quantiles')
decision=dict(status='MEASURED_PROFILE_RECOMMENDATION_NOT_INDEPENDENT_HOLDOUT',rule='Keep original serial consumer atQ4. For the reported realQ1024/Q2048 single-channel profiles,enable phase-reused priority pipeline. Use exact exponent cache for the evaluated D128/Qwen3 owner;use lighter pipeline for D32/D64 profiles. Other profiles default to the measured reference until qualified.',source_reuse='Rule is chosen on the same captured design sources. D128 appears in one model family;dimension and model effects are not disentangled. All variants,including losses,are reported.',quality='The rule uses only configured horizon and owner profile,not reference outputs. All candidate numerical images match the same frozen reference;quality cannot be improved or weakened by this choice.')
(C/'results/ADMISSION_DECISION.json').write_text(json.dumps(decision,indent=2))
for name,rs in [('variant_pairs.csv',rows),('variant_checkpoints.csv',checks),('tail_query_ratios.csv',query_rows)]:csvwrite(C/'results'/name,rs)
(C/'results/VARIANT_AUDIT.json').write_text(json.dumps(dict(status='ALL20_LIGHTER_PIPELINE_CASES_AND_THREE_EXPLICIT_VARIANTS_CHECKED',placements=20,new_unique_executions=16,reused_cases=4,additional_unique_callbacks=callbacks_new,summary=summary,tail_windows=tails,per_query_tail=qsummary,quality='Every output and all six arrays exactly equal the already independently checked frozenDense;38912unique heads,not multiplied by variants or producer arms.',admission=decision),indent=2));print('COMPLETE_VARIANT_AUDIT',20,16,flush=True)
