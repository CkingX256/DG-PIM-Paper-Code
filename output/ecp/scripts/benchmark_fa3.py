




   
from common import *
from external_inputs import decode
from external_quality import qualify
import argparse,subprocess,tarfile,math,struct,importlib,sys,numpy as np
ap=argparse.ArgumentParser();ap.add_argument('--probe-only',action='store_true');ap.add_argument('--fa3-source',type=Path);ap.add_argument('--manifest',type=Path,default=U/'NEW_BASE_TASKS.json');ap.add_argument('--warmup',type=int,default=2);ap.add_argument('--repetitions',type=int,default=20);a=ap.parse_args()
import torch
probe=dict(torch=torch.__version__,cuda_build=torch.version.cuda,cuda_available=torch.cuda.is_available(),required='Hopper H100/H800,CUDA>=12.3,official pinned hopper implementation',timing_generated=False)
if not torch.cuda.is_available():
 probe['status']='BLOCKED_NO_CUDA_GPU';save(U/'results/FA3_RUNTIME_STATUS.json',probe);print(json.dumps(probe));raise SystemExit(0 if a.probe_only else 2)
probe.update(device=torch.cuda.get_device_name(),capability=list(torch.cuda.get_device_capability()))
if torch.cuda.get_device_capability()[0]!=9:probe['status']='BLOCKED_NOT_HOPPER';save(U/'results/FA3_RUNTIME_STATUS.json',probe);raise SystemExit(2)
if a.probe_only:probe['status']='HOPPER_AVAILABLE_NOT_YET_BENCHMARKED';save(U/'results/FA3_RUNTIME_STATUS.json',probe);print(json.dumps(probe));raise SystemExit(0)
assert a.fa3_source is not None
revision=subprocess.check_output(['git','-C',str(a.fa3_source),'rev-parse','HEAD'],text=True).strip();pin=read(U/'FA3_SOURCE_PIN.json')['sources'][0]['revision'];assert revision==pin,'Use exactly the pinned official source'
assert not subprocess.check_output(['git','-C',str(a.fa3_source),'diff','HEAD','--'],text=True),'Tracked source modifications must be separately qualified'
assert tuple(int(x)for x in torch.version.cuda.split('.')[:2])>=(12,3)
sys.path.insert(0,str(a.fa3_source.resolve()/'hopper'));module=importlib.import_module('flash_attn_interface')
interface_pin=next(x for x in read(U/'FA3_SOURCE_PIN.json')['sources']if x['path']=='hopper/flash_attn_interface.py')
assert sha(Path(module.__file__))==interface_pin['sha256'],'Imported Python interface must match the pinned source bytes'
extension=importlib.import_module('flash_attn_3._C')
probe.update(interface_path=module.__file__,interface_sha256=sha(Path(module.__file__)),extension_path=extension.__file__,extension_sha256=sha(Path(extension.__file__)),warmup=a.warmup,repetitions=a.repetitions,dtype='float16',cache_state='warm resident',timing_generated=True)
torch.backends.cuda.matmul.allow_tf32=False;rows=[];quality=[]
for t in read(a.manifest):
 meta,lengths,q,k,v,input_ids=decode(t);N,H,D,Q=(meta[k]for k in ['N','H','D','Q']);L0=meta['L0']
 qg=torch.from_numpy(q).to('cuda');kg=torch.from_numpy(k).to('cuda').view(1,N,1,D);vg=torch.from_numpy(v).to('cuda').view(1,N,1,D)
 def episode():
                                                                          
                                                                       
  return[module.flash_attn_func(qg[i:i+1].unsqueeze(0),kg[:,:L0+i],vg[:,:L0+i],causal=True,softmax_scale=1/math.sqrt(D)).float()for i in range(Q)]
 for _ in range(a.warmup):output=episode()
 torch.cuda.synchronize();values=[]
 for _ in range(a.repetitions):
  start=torch.cuda.Event(enable_timing=True);end=torch.cuda.Event(enable_timing=True);start.record();output=episode();end.record();end.synchronize();values.append(start.elapsed_time(end))
 rows.append(dict(fixture=t['fixture'],mode='sequential_decode_loop',input_identities=input_ids,milliseconds=values,includes='GPU work plus CPU-launch gaps within CUDA event span;FP32 publication',excludes='H2D,D2H,Key production/upstream projection;resident consumer only'))
 graph=torch.cuda.CUDAGraph()
 with torch.cuda.graph(graph):gout=episode()
 graph.replay();torch.cuda.synchronize();values=[]
 for _ in range(a.repetitions):
  start=torch.cuda.Event(enable_timing=True);end=torch.cuda.Event(enable_timing=True);start.record();graph.replay();end.record();end.synchronize();values.append(start.elapsed_time(end))
 rows.append(dict(fixture=t['fixture'],mode='fixed_input_cuda_graph',milliseconds=values,scope='One kernel per causal query,prefixed views,pre-recorded consumer trace;not free-running model decoding or an episode ratio'))
 ref=np.asarray(np.load(resolve(t['reference']))['output'],dtype='<f4');dense=U/'oracles'/t['fixture']/'dense_output.f32';dense_cert=read(dense.parent/'CERTIFICATE.json');assert sha(dense)==dense_cert['files']['dense_output.f32']
 for mode,buffers in [('sequential_decode_loop',output),('fixed_input_cuda_graph',gout)]:
  actual=torch.cat([x.squeeze(0)for x in buffers],dim=0).cpu().numpy().astype('<f4');assert actual.shape==ref.shape
  for row in qualify(actual.tobytes(),ref.tobytes(),dense.read_bytes(),Q,H,D):quality.append(dict(fixture=t['fixture'],mode=mode,**row))
save(U/'results/FA3_BENCHMARK.json',dict(status='MEASURED_OFFICIAL_PINNED_FA3_ON_HOPPER',environment=probe,source_revision=pin,rows=rows,quality=quality,scope='Consumer-only platform results. Do not divide PIM episode cycles by GPU graph time. Different floating-point reduction/output rounding is independently evaluated. No clocks,areas or transfer boundaries are silently equated.'))
