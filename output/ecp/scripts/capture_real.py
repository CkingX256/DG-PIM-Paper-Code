




   
from pathlib import Path
import argparse,json,hashlib,struct,sys,importlib,inspect
import numpy as np

def validate_plan(l0,Q,context_limit,token_count):
 if l0<1 or Q not in [256,1024,2048]:raise ValueError('unsupported prefix/horizon')
 n=l0+Q-1
 if n>context_limit:raise ValueError(f'N={n} exceeds native model limit {context_limit}; no implicit RoPE extension')
 if token_count<n:raise ValueError('corpus shorter than the requested causal context; no repetition permitted')
 return n

def write_fixture(out,k,v,q,reference,provenance):
 N,D=k.shape;Q,H,Dq=q.shape;assert D==Dq and v.shape==k.shape and reference.shape==q.shape
 for x in [k,v,q,reference]:assert np.isfinite(x).all()
 L0=N-Q+1;lengths=np.arange(L0,N+1,dtype='<u4');out.mkdir(parents=True,exist_ok=False)
 np.ascontiguousarray(k,dtype='<f4').tofile(out/'raw_keys.f32')
 with (out/'input.bin').open('wb')as f:
  f.write(b'DGNUM006');f.write(struct.pack('<4I',N,H,D,Q));f.write(lengths.tobytes())
  for x in [k,v,q]:f.write(np.ascontiguousarray(x,dtype='<f2').tobytes())
 np.savez(out/'reference_fp32.npz',k=k,v=v,q=q,output=reference)
 provenance.update(N=N,H=H,D=D,Q=Q,L0=L0,status='REAL_MODEL_CAPTURE_COMPLETE',files={p.name:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()for p in sorted(out.iterdir())})
 (out/'PROVENANCE.json').write_text(json.dumps(provenance,indent=2))

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--snapshot',type=Path,required=True);ap.add_argument('--model-id',required=True);ap.add_argument('--revision',required=True);ap.add_argument('--text',type=Path,required=True);ap.add_argument('--l0',type=int,required=True);ap.add_argument('--queries',type=int,required=True);ap.add_argument('--layer',type=int);ap.add_argument('--owner',type=int,default=0);ap.add_argument('--token-offset',type=int,default=0);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
 if len(a.revision)!=40 or any(c not in '0123456789abcdef'for c in a.revision):raise ValueError('pin the actual 40-hex HF snapshot revision')
 if a.snapshot.resolve().name!=a.revision:raise ValueError('use the immutable HF snapshots/<revision> directory so revision identity is not an unchecked label')
 import torch,transformers
 if transformers.__version__!='4.57.1':raise RuntimeError('adapter is pinned to transformers4.57.1; review adapter before changing versions')
 from transformers import AutoModelForCausalLM,AutoTokenizer
 torch.set_num_threads(2);torch.use_deterministic_algorithms(True)
 config=json.load(open(a.snapshot/'config.json'));cfgsha=hashlib.file_digest((a.snapshot/'config.json').open('rb'),'sha256').hexdigest()
 tok=AutoTokenizer.from_pretrained(a.snapshot,local_files_only=True,trust_remote_code=False)
 text=a.text.read_text();ids=tok(text,return_tensors='pt',add_special_tokens=False).input_ids[:,a.token_offset:]
 n=validate_plan(a.l0,a.queries,int(config['max_position_embeddings']),ids.shape[1]);ids=ids[:,:n]
 model=AutoModelForCausalLM.from_pretrained(a.snapshot,local_files_only=True,trust_remote_code=False,dtype=torch.float32,attn_implementation='eager').eval()
 layers=model.gpt_neox.layers if config['model_type']=='gpt_neox'else model.model.layers
 layer=len(layers)//2 if a.layer is None else a.layer
 if not 0<=layer<len(layers):raise ValueError('invalid layer')
 target=layers[layer].attention if config['model_type']=='gpt_neox'else layers[layer].self_attn
 mod=importlib.import_module(target.__class__.__module__)
 if not hasattr(mod,'eager_attention_forward'):raise RuntimeError('post-RoPE eager interface absent; refusing to capture pre-RoPE projections')
 original=mod.eager_attention_forward;captured={}
 def wrapped(module,query,key,value,*args,**kwargs):
  if module is target:
   if captured:raise RuntimeError('unexpected repeated attention call')
   captured.update(q=query.detach().float().cpu().clone(),k=key.detach().float().cpu().clone(),v=value.detach().float().cpu().clone())
  return original(module,query,key,value,*args,**kwargs)
 mod.eager_attention_forward=wrapped
 try:
  with torch.no_grad():
   backbone=model.gpt_neox if config["model_type"]=="gpt_neox"else model.model
   backbone(input_ids=ids,use_cache=False)
 finally:mod.eager_attention_forward=original
 if not captured:raise RuntimeError('capture hook never called; no output registered')
 Qh,Kh=captured['q'].shape[1],captured['k'].shape[1]
 if Qh%Kh or not 0<=a.owner<Kh:raise ValueError('invalid GQA owner mapping')
 group=Qh//Kh;hq=slice(a.owner*group,(a.owner+1)*group)
 k=captured['k'][0,a.owner];v=captured['v'][0,a.owner];q=captured['q'][0,hq,a.l0-1:n].transpose(0,1).contiguous();out=torch.empty_like(q)
 with torch.no_grad():
  for i in range(a.queries):
   l=a.l0+i;p=torch.softmax((q[i]@k[:l].T)/(k.shape[1]**.5),dim=-1);out[i]=p@v[:l]
 weight_ids={p.name:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()for p in a.snapshot.glob('*.safetensors')}
 if not weight_ids:raise RuntimeError('expected safetensors weights in pinned snapshot')
 prov=dict(model=a.model_id,revision=a.revision,layer=layer,owner=a.owner,query_head_range=[hq.start,hq.stop],token_offset=a.token_offset,text_sha=hashlib.sha256(text.encode()).hexdigest(),tokens_sha=hashlib.sha256(ids.numpy().tobytes()).hexdigest(),config_sha=cfgsha,weights=weight_ids,torch=torch.__version__,transformers=transformers.__version__,adapter_module=mod.__name__,adapter_sha=hashlib.sha256(inspect.getsource(original).encode()).hexdigest(),capture='Teacher-forced causal FP32 backbone post-RoPE eager operands, real text appends. LM vocabulary projection omitted outside the owner boundary. Reference is independent FP32 owner softmax/PV from those operands; no whole-model generation-quality claim.')
 write_fixture(a.output,k.numpy(),v.numpy(),q.numpy(),out.numpy(),prov)
if __name__=='__main__':main()
