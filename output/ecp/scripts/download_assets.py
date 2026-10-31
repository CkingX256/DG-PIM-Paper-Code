                                                                             
from pathlib import Path
import os,json,hashlib
os.environ['HF_HUB_DISABLE_XET']='1'
from huggingface_hub import snapshot_download,hf_hub_download
import pyarrow.parquet as pq
R=Path(__file__).resolve().parents[3];U=R/'output/ecp';old=U/'original_captures';sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
corpus=json.load(open(old/'CORPUS_IDENTITY.json'));p=Path(hf_hub_download(corpus['repo'],corpus['file'],repo_type='dataset',revision=corpus['revision'],cache_dir=R/'model_cache'));assert sha(p)==corpus['sha256'];rows=pq.read_table(p).column('text').to_pylist()
registration=json.load(open(old/'CONTEXT_REGISTRATION.json'));text=None
for sep in ['\n','']:
 candidate=sep.join(rows)
 if all(hashlib.sha256(candidate[z['character_offset']:z['character_offset']+z['characters']].encode()).hexdigest()==z['sha256']for z in registration['contexts']):text=candidate;break
assert text is not None,'Old serialization must match all four source hashes'
(U/'contexts').mkdir(exist_ok=True);contexts=[]
for c in [4,5,6,7]:
 value=text[c*100000:c*100000+65000];assert len(value)==65000;p=U/'contexts'/f'context_{c}.txt';p.write_text(value);contexts.append(dict(context=c,start=c*100000,end=c*100000+65000,sha256=sha(p)))
(U/'CONTEXT_IDENTITY.json').write_text(json.dumps(dict(corpus=corpus,whole_text_sha256=hashlib.sha256(text.encode()).hexdigest(),old_four_contexts_reproduced=True,new=contexts),indent=2));print('PINNED_CORPUS_RESTORED',len(text),flush=True)
models=[]
for m in [1,2,3]:
 prov=json.load(open(old/'captures'/f'real_m{m}_c0_q1024/PROVENANCE.json'))
 root=Path(snapshot_download(prov['model'],revision=prov['revision'],cache_dir=R/'model_cache',allow_patterns=['*.json','*.safetensors','merges.txt','vocab.json','tokenizer.model','special_tokens_map.json'],max_workers=3))
 for n,h in prov['weights'].items():assert sha(root/n)==h,(prov['model'],n)
 models.append(dict(model=m,name=prov['model'],revision=prov['revision'],snapshot=str(root),weights=prov['weights'],layer=prov['layer'],owner=0));(U/'MODELS_PINNED.json').write_text(json.dumps(models,indent=2));print('PINNED_MODEL_RESTORED',m,prov['model'],flush=True)
