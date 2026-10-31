


   
from common import *
from case_metrics import metric
import math,csv,re,numpy as np,argparse,itertools
ap=argparse.ArgumentParser();ap.add_argument('--available',action='store_true');args=ap.parse_args()
reg=read(U/'REGISTRATION.json');old=reg['old_contexts'];new=reg['new_contexts'];out=U/'results';out.mkdir(exist_ok=True)
def csvsave(name,rows):
 if not rows:return
 fields=list(dict.fromkeys(k for r in rows for k in r))
 with(out/name).open('w',newline='')as f:
  w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
def gm(v):return math.exp(sum(math.log(x)for x in v)/len(v))if v else None
def pair(b,c):
 b,c=metric(b),metric(c);assert b['fixture']==c['fixture']and b['arrays']==c['arrays']
 bc,cc=b['counters'],c['counters'];Q=bc['Q'];assert Q==cc['Q']
 return dict(fixture=b['fixture'],population=b['population'],Q=Q,D=bc['D'],H=bc['H'],
  baseline=b['name'],candidate=c['name'],baseline_cycles=b['elapsed_cycles'],candidate_cycles=c['elapsed_cycles'],
  speedup=b['elapsed_cycles']/c['elapsed_cycles'],first_output=b['first_output_elapsed']/c['first_output_elapsed'],
  first_retirement=(b['ends'][0]-1)/(c['ends'][0]-1),
  baseline_retained=bc['buffered_prefill_retained_tokens'],candidate_retained=cc['buffered_prefill_retained_tokens'],
  baseline_peak=bc['peak_live_scratch_bytes'],candidate_peak=cc['peak_live_scratch_bytes'],
  callback_baseline=b['callbacks'],callback_candidate=c['callbacks'],
  mac_epu_overlap=c['pipeline']['physical_mac_epu_overlap_cycles'],
  readout_epu_overlap=c['pipeline']['physical_readout_epu_overlap_cycles'],
  spec_exp=cc['qpipe_spec_exp_calls'],reused_exp=cc['qpipe_valid_exp_calls'],
  recomputed_exp=cc['qpipe_recomputed_exp_calls'],invalidations=cc['qpipe_invalidations'],
  exact_exp_baseline=bc['exact_exp_calls'],exact_exp_candidate=cc['exact_exp_calls'],
  source_write_baseline=bc['initial_source_write_bytes']+bc['append_source_write_bytes'],
  source_write_candidate=cc['initial_source_write_bytes']+cc['append_source_write_bytes'])
def exists(*names):return all(rec(n).exists()for n in names)
def stats(rows,bootstrap=False):
 if not rows:return None
 s=[r['speedup']for r in rows];v=dict(n=len(rows),gm=gm(s),cycle_sum=sum(r['baseline_cycles']for r in rows)/sum(r['candidate_cycles']for r in rows),minimum=min(s),maximum=max(s),wins=sum(x>1 for x in s),ties=sum(x==1 for x in s),losses=sum(x<1 for x in s),first_output_gm=gm([r['first_output']for r in rows]))
 if bootstrap:
  blocks=sorted(set(int(re.search(r'_c(\d+)_',r['fixture'])[1])for r in rows))
                                                                             
                                                                              
  logs=np.array([np.mean([math.log(r['speedup'])for r in rows if int(re.search(r'_c(\d+)_',r['fixture'])[1])==b])for b in blocks]);rng=np.random.default_rng(20261026);draw=np.exp(logs[rng.integers(0,len(blocks),(20000,len(blocks)))].mean(axis=1))
  exhaustive=np.exp(np.asarray([logs[list(ix)].mean()for ix in itertools.product(range(len(blocks)),repeat=len(blocks))]))
  v.update(source_blocks=len(blocks),bootstrap_seed=20261026,bootstrap_replicates=20000,block_percentile95=[float(x)for x in np.quantile(draw,[.025,.975])],exact_block_resampling_combinations=len(exhaustive),exact_resampling_percentile95=[float(x)for x in np.quantile(exhaustive,[.025,.975])],leave_one_text_block_out_gm=[float(np.exp(np.delete(logs,i).mean()))if len(blocks)>1 else None for i in range(len(blocks))],leave_one_model_out_gm={str(m):gm([r['speedup']for r in rows if int(re.search(r'_m(\d+)_',r['fixture'])[1])!=m])for m in sorted({int(re.search(r'_m(\d+)_',r['fixture'])[1])for r in rows})},bootstrap_scope='resampled character slices carrying their captured models; small four-block design sample, not population coverage or per-query guarantee')
 return v

abl=[]
for arm in reg['ablation_arms']:
 for f in old:
  b='base__'+f;c=b if arm=='serial'else arm+'__'+f
  if not exists(b,c):
   assert args.available,('Missing ablation',c);continue
  abl.append(dict(arm=arm,**pair(b,c)))
csvsave('ablation_contexts.csv',abl)
abls=[]
for arm in reg['ablation_arms']:
 for Q in [1024,2048]:
  rr=[r for r in abl if r['arm']==arm and r['Q']==Q]
  s=stats(rr)
  if s:abls.append(dict(arm=arm,Q=Q,**s))
csvsave('ablation_summary.csv',abls)
shadow=[]
for f in old:
 b='priority_shadow_cache__'+f;c='full__'+f
 if exists(b,c):
  r=pair(b,c);bm,cm=metric(b),metric(c)
  assert r['baseline_retained']==r['candidate_retained']
  assert bm['counters']['base_peak_live_scratch_bytes']==cm['counters']['base_peak_live_scratch_bytes']
  shadow.append(r)
csvsave('capacity_matched_cache.csv',shadow)
incremental=[];incremental_summary=[]
for f in old:
 for label,arm in [('cache_over_phase_only','pipeline'),('cache_over_priority','priority')]:
  b=arm+'__'+f;c='full__'+f
  if exists(b,c):incremental.append(dict(comparison=label,**pair(b,c)))
csvsave('incremental_cache_contexts.csv',incremental)
for label in ['cache_over_phase_only','cache_over_priority']:
 for Q in [1024,2048]:
  s=stats([r for r in incremental if r['comparison']==label and r['Q']==Q])
  if s:incremental_summary.append(dict(comparison=label,Q=Q,gm_area_or_average_power_budget=s['gm']-1,all_context_area_or_average_power_budget=s['minimum']-1,**s))
csvsave('incremental_cache_budgets.csv',incremental_summary)

sens=[]
for p in read(U/'SENSITIVITY_PAIRS.json'):
 if not exists(p['baseline'],p['candidate']):
  assert args.available,('Missing sensitivity',p);continue
 r=pair(p['baseline'],p['candidate']);bm,cm=metric(p['baseline']),metric(p['candidate'])
 assert bm['capacity']==cm['capacity']
 sens.append(dict(point=p['point'],resource_scope=p['scope'],**r))
csvsave('sensitivity_contexts.csv',sens)
sensum=[]
for point in sorted(set(p['point']for p in sens)):
 for Q in [1024,2048]:
  s=stats([r for r in sens if r['point']==point and r['Q']==Q])
  if s:sensum.append(dict(point=point,Q=Q,**s))
csvsave('sensitivity_summary.csv',sensum)

pairs=[]
for f in old+new:
 for comp,b,c in [('consumer','base__'+f,'full__'+f),('combined','canonical_serial__'+f,'full__'+f),('producer_matched_ecp','canonical_full__'+f,'full__'+f)]:
  if exists(b,c):pairs.append(dict(comparison=comp,**pair(b,c)))
  else:assert args.available,('Missing long comparison',b,c)
csvsave('long_contexts.csv',pairs)
longsum=[]
for comparison in ['consumer','combined','producer_matched_ecp']:
 for population,Q in [('original',1024),('original',2048),('heldout',2048)]:
  rr=[r for r in pairs if r['comparison']==comparison and r['population']==population and r['Q']==Q]
  s=stats(rr,bootstrap=population=='heldout')
  if s:longsum.append(dict(comparison=comparison,population=population,Q=Q,**s))
csvsave('long_summary.csv',longsum)
checkpoints=[];windows=[];tails=[];queryrows=[]
for r in pairs:
 bm,cm=metric(r['baseline']),metric(r['candidate']);be,ce=np.array(bm['ends'])-1,np.array(cm['ends'])-1;bl,cl=np.diff(np.r_[0,be]),np.diff(np.r_[0,ce]);rat=bl/cl
 for q in [4,16,64,256,1024,2048]:
  if q<=r['Q']:checkpoints.append(dict(comparison=r['comparison'],fixture=r['fixture'],checkpoint=q,baseline_cycles=int(be[q-1]),candidate_cycles=int(ce[q-1]),speedup=float(be[q-1]/ce[q-1]),scope='checkpoint of full registered trace with its full-horizon allocation, not a separately allocated short episode'))
 for n in [64,128,256,512]:
  windows.append(dict(comparison=r['comparison'],fixture=r['fixture'],last_queries=n,baseline_cycles=int(bl[-n:].sum()),candidate_cycles=int(cl[-n:].sum()),speedup=float(bl[-n:].sum()/cl[-n:].sum())))
 tail=dict(comparison=r['comparison'],fixture=r['fixture'],queries=r['Q'],query_wins=int((rat>1).sum()),query_ties=int((rat==1).sum()),query_losses=int((rat<1).sum()),minimum_query_ratio=float(rat.min()),ratio_p05=float(np.quantile(rat,.05)))
 for p in [.5,.95,.99,.999]:
  label=str(p*100).replace('.','p');bv=float(np.quantile(bl,p,method='linear'));cv=float(np.quantile(cl,p,method='linear'));tail.update({f'baseline_p{label}_cycles':bv,f'candidate_p{label}_cycles':cv,f'p{label}_latency_ratio':bv/cv})
 tails.append(tail)
 for q,(b,c)in enumerate(zip(bl,cl)):queryrows.append(dict(comparison=r['comparison'],fixture=r['fixture'],query=q,baseline_cycles=int(b),candidate_cycles=int(c),speedup=float(b/c)))
csvsave('checkpoints.csv',checkpoints);csvsave('late_windows.csv',windows);csvsave('tail_contexts.csv',tails);csvsave('query_latency.csv',queryrows)

area=[];activity=[];surface=[]
for r in pairs:
 S=r['speedup'];area.append(dict(comparison=r['comparison'],fixture=r['fixture'],speedup=S,area_delay_break_even_fraction=S-1,energy_break_even_average_power_fraction=S-1,edp_break_even_average_power_fraction=S*S-1,scope='parameterized total owner area/power ratios; no synthesis or watts'))
 b,c=metric(r['baseline']),metric(r['candidate']);row=dict(comparison=r['comparison'],fixture=r['fixture'])
 for k in sorted(set(b['commands'])|set(c['commands'])):row['delta_command_'+k]=c['commands'].get(k,0)-b['commands'].get(k,0)
 for k in ['exact_exp_calls','softmax_score_read_bytes','exp_scratch_read_bytes','exp_scratch_write_bytes','qpipe_spec_score_read_bytes','qpipe_spec_exp_write_bytes','qpipe_group_sum_write_bytes']:
  row['delta_'+k]=c['counters'][k]-b['counters'][k]
 activity.append(row)
 for a in [0,.01,.025,.05,.075,.1,.15,.2]:
  for p in [0,.01,.025,.05,.075,.1,.15,.2]:surface.append(dict(comparison=r['comparison'],fixture=r['fixture'],area_overhead=a,total_average_power_overhead=p,normalized_area_delay=(1+a)/S,normalized_energy=(1+p)/S,normalized_edp=(1+p)/(S*S),normalized_area_energy_delay=(1+a)*(1+p)/(S*S)))
csvsave('area_power_break_even.csv',area);csvsave('activity_delta.csv',activity);csvsave('area_power_surface.csv',surface)
budgets=[]
for s in longsum:
 S=s['gm'];mn=s['minimum'];budgets.append(dict(comparison=s['comparison'],population=s['population'],Q=s['Q'],n=s['n'],gm_speedup=S,cohort_gm_area_or_average_power_budget=S-1,all_context_area_or_average_power_budget=mn-1,cohort_gm_edp_power_budget=S*S-1,all_context_edp_power_budget=mn*mn-1,scope='strict improvement requires overhead below budget; total owner power includes changed activity; cohort GM is not an all-query guarantee'))
csvsave('area_power_cohorts.csv',budgets)
summary=dict(status='PARTIAL_PROGRESS_ONLY'if args.available else'ALL_REGISTERED_ANALYSES_COMPLETE',ablation=abls,sensitivity=sensum,long=longsum,incremental_cache_budgets=incremental_summary,capacity_matched_cache={str(Q):stats([r for r in shadow if r['Q']==Q])for Q in [1024,2048]},definitions={'speedup':'paired baseline (retirement cycle-1) / candidate (retirement cycle-1), common actor start removed','first_output':'query-zero last output-producing merge minus actor start; first retirement separately reported','tail':'per-query end-to-end retirement intervals; first includes startup; percentiles within each context, no iid query CI','area':'(1+a)/S','energy':'(1+p)/S','edp':'(1+p)/S^2','area_energy_delay':'(1+a)(1+p)/S^2','power':'p is total average owner power change including all activity and leakage, not only new logic power'})
save(out/'CAMPAIGN_ANALYSIS.json',summary)
print('ANALYSIS',len(abl),'ablation placements,',len(sens),'sensitivity pairs,',len(pairs),'long pairs',flush=True)
