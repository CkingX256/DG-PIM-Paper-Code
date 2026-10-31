                                                                                
from common import *

def metric(name):
 p=rec(name);cache=U/'metrics'/(name+'.json.gz');h=sha(p)
 if cache.exists():
  m=read(cache);assert m['record_sha256']==h
  if m.get('metric_schema')==2:return m
 r=read(p);z=r['result'];t=r['task']
 assert r['native_cycles']==z['actor_end_cycle']
 assert z['submitted']==z['callbacks']==r['physical_audit']['events']
 assert sum(x['cycles'] for x in z['phases'])+1==z['actor_end_cycle']
 assert z['peak_live_scratch_bytes']<=t['capacity']
 assert z['producer_dma_timing_mutations']==0
 assert r['pipeline_audit']['physical_readout_epu_overlap_cycles']==0
 steps=z['steps'];assert len(steps)==z['Q']
 ends=[int(s['end'])for s in steps]
 assert ends[-1]==z['actor_end_cycle'] and all(b>a for a,b in zip([0]+ends,ends))
 if t.get('expected_record'):
  cert=read(resolve(t['expected_record']).parent/'ARRAY_CERTIFICATE.json')
  assert r['arrays']==cert['arrays'],('array mismatch',name)
 phase={}
 for x in z['phases']:phase[x['phase']]=phase.get(x['phase'],0)+x['cycles']
                                                                          
 scalars={k:v for k,v in z.items()if isinstance(v,(int,float,str,bool))}
 first_output=max(x['end']for x in z['phases']if x['query']==0 and x['phase']=='merge')-1
 assert first_output<=ends[0]-1
 m=dict(metric_schema=2,name=name,fixture=t['fixture'],arm=t['arm'],producer=t['producer'],
  population='heldout'if t['fixture'].startswith('new_')else'original',
  record_sha256=h,archive_sha256=r['archive_sha256'],arrays=r['arrays'],
  native_cycles=r['native_cycles'],elapsed_cycles=r['native_cycles']-1,
  first_output_elapsed=first_output,ends=ends,phase_cycles=phase,counters=scalars,
  commands=r['physical_audit']['commands'],pipeline=r['pipeline_audit'],
  parameters=t['parameters'],capacity=t['capacity'],callbacks=z['callbacks'])
 save(cache,m);return m

def available():
 return [metric(p.parent.name)for p in sorted((U/'native').glob('*/record.json.gz'))]

if __name__=='__main__':
 mm=available();save(U/'results/METRIC_PROGRESS.json',dict(completed=len(mm),callbacks=sum(m['callbacks']for m in mm)))
 print('COMPACT_METRICS',len(mm),flush=True)
