from pathlib import Path
import json,gzip,hashlib,csv,re
import numpy as np
R=Path(__file__).resolve().parents[3];C=R/'output/consumer';O=R/'output/experiment_data'
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def resolve(p):
 p=Path(p)
 if p.is_absolute()and p.is_relative_to(R):return p
 for m in ['/output/','/native_base/','/restored_inputs/']:
  if m in str(p):return R/m.strip('/')/str(p).split(m,1)[1]
 return p
def read(p):
 p=resolve(p)
 with(gzip.open(p,'rt')if p.suffix=='.gz'else p.open())as f:return json.load(f)
def record(family,t):return C/family/t['name']/'record.json.gz'
def q0(r):return max(x['end']for x in r['result']['phases']if x['query']==0 and x['phase']=='merge')-1
def stats(rows,blocks=True):
 a=np.asarray([r['baseline']for r in rows],float);b=np.asarray([r['candidate']for r in rows],float);ratio=a/b
 z=dict(n=len(rows),gm=float(np.exp(np.log(ratio).mean())),cycle_sum=float(a.sum()/b.sum()),minimum=float(ratio.min()),maximum=float(ratio.max()),wins=int((a>b).sum()),ties=int((a==b).sum()),losses=int((a<b).sum()),first_output_gm=float(np.exp(np.mean([np.log(r['baseline_q0']/r['candidate_q0'])for r in rows]))))
 bs=sorted({r['source_block']for r in rows});z['source_blocks']=len(bs)
 if blocks and len(bs)>1:
  idx=[np.asarray([i for i,r in enumerate(rows)if r['source_block']==s])for s in bs];rng=np.random.default_rng(20261025);boot=[]
  for _ in range(10000):
   ix=np.concatenate([idx[i]for i in rng.integers(0,len(idx),len(idx))]);boot.append([np.exp(np.log(ratio[ix]).mean()),a[ix].sum()/b[ix].sum()])
  z['gm_block_bootstrap95']=np.quantile(np.asarray(boot)[:,0],[.025,.975]).tolist();z['sum_block_bootstrap95']=np.quantile(np.asarray(boot)[:,1],[.025,.975]).tolist()
 return z
def csvwrite(p,rows):
 with Path(p).open('w')as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
def verify(t,p):
 r=read(p);z=r['result'];old=read(t['expected_record']);assert r['task']==t
 assert r['status']=='PASS_NATIVE_COMMANDS_PIPELINE_OWNERSHIP_SIX_ARRAYS'and r['arrays']==old['arrays']
 assert sha(p.parent/'output.f32')==r['arrays']['output.f32']
 assert z['submitted']==z['callbacks']and r['physical_audit']['events']==z['callbacks']and r['physical_audit']['status']=='PASS'
 assert sum(p['cycles']for p in z['phases'])+1==r['native_cycles']and z['peak_live_scratch_bytes']<=262144
 assert r['pipeline_audit']['physical_readout_epu_overlap_cycles']==0 and r['pipeline_audit']['batches']==z['qpipe_batches']and z['qpipe_peak_slots']<=t['parameters'].get('qk_pipeline_depth',2)
 assert z['qk_mac']==old['result']['qk_mac']and z['value_mac']==old['result']['value_mac']and z['probability_move_commands']==old['result']['probability_move_commands']
 assert z['qpipe_additional_accumulator_bytes']==0 and z['qpipe_release_checks']==z['qpipe_batches']
 if z['qk_exact_exp_cache']:
  n=z['max_tokens'];g=(n+15)//16;assert z['qpipe_exp_buffer_bytes']==4*n and z['qpipe_exp_group_sum_bytes']==4*g and z['qpipe_exp_validity_bytes']==(g+7)//8 and z['qpipe_predictor_bytes']==64
 def pipeline_storage(v):
  vn=v['max_tokens'];vg=(vn+15)//16
  if v.get('qk_exact_exp_cache',False):return 4*vn+4*vg+(vg+7)//8+64
  return 64 if v.get('qk_priority_first',False)else 0
                                                                         
                                                                           
                                                                          
 assert z['base_peak_live_scratch_bytes']-old['result']['base_peak_live_scratch_bytes']==pipeline_storage(z)-pipeline_storage(old['result'])
 if t.get('expected_equal_cycles'):assert r['native_cycles']==old['native_cycles']
 return r,old
