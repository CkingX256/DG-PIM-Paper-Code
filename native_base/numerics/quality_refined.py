




   
from pathlib import Path
import argparse,csv,gzip,hashlib,json,subprocess,tempfile,shutil
from concurrent.futures import ThreadPoolExecutor,as_completed
import numpy as np
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime'))
from paths import local_path
from exact_quality import exact_metrics,controls
R=Path(__file__).resolve().parents[1];B=R/'baseline'
def sha(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def read(p):
 with(gzip.open(p,'rt')if p.suffix=='.gz'else p.open())as f:return json.load(f)
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--campaign',action='append',required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--workers',type=int,default=2);ap.add_argument('--include-cached-results',action='store_true');a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True);cache=R/'numerics/oracle_cache_v2';cache.mkdir(exist_ok=True);controls();oracle=R/'numerics/native_oracle_v3';oh=sha(oracle);source=sha(R/'numerics/native_oracle.cpp');records=[]
 for name in a.campaign:
  for parent in [R/'recovered_evidence'/name,R/'experiments'/name]:
   for rp in sorted(parent.glob('*/record.json.gz')):records.append(rp)
                                                                                          
 groups={}
 for rp in records:
  r=read(rp);t=r['task'];f=t['fixture'];p=t.get('parameters',{});inp=local_path(t.get('input',str(B/'reproduction/inputs'/f/'input.bin')));raw=local_path(t.get('raw_keys',str(B/'runtime/fixtures'/f/'raw_keys.f32')));ref=local_path(t.get('reference',str(B/'reproduction/inputs'/f/'reference_fp32.npz')));strategy=p.get('encoder_strategy','global');size=p.get('encoder_block_size',64);packed=bool(r['result']['token_packed']);C=t.get('channels',1)
  key=hashlib.sha256(json.dumps([sha(inp),sha(raw),strategy,size,packed,C,oh]).encode()).hexdigest();groups.setdefault(key,dict(input=inp,raw=raw,reference=ref,strategy=strategy,size=size,packed=packed,channels=C,rows=[]))['rows'].append(rp)
 def group(item):
  key,g=item;cp=cache/(key+'.json');den=cache/(key+'.dense.f32')
  if not cp.exists()or not den.exists():
   with tempfile.TemporaryDirectory(prefix='dg_oracle_',dir=str(cache))as td:
    subprocess.run([str(oracle),str(g['input']),str(g['raw']),g['strategy'],str(g['size']),str(int(g['packed'])),td,str(g['channels'])],check=True)
    expected={p.name:sha(p)for p in Path(td).iterdir()};
    if cp.exists():assert read(cp)['expected']==expected and read(cp)['binary_sha256']==oh and read(cp)['source_sha256']==source
    shutil.copy2(Path(td)/'dense_output.f32',den);cp.write_text(json.dumps(dict(expected=expected,source_sha256=source,binary_sha256=oh,input_sha256=sha(g['input']),raw_sha256=sha(g['raw'])),indent=2)+'\n')
  cert=read(cp);assert cert['binary_sha256']==oh and sha(den)==cert['expected']['dense_output.f32'];expected=cert['expected'];db=den.read_bytes();ref=np.ascontiguousarray(np.load(g['reference'])['output'],dtype='<f4');Q,H,D=ref.shape;rb=ref.tobytes();stride=4*D;answers=[]
  for rp in g['rows']:
   r=read(rp);t=r['task'];arrays=r['arrays'];checks={n:arrays[n]==expected[n]for n in ['scores.f32','probability.f16','encoded_keys.f16']}
   checks['anchor']=r['raw_files']['anchor.f32']['sha256']==expected['block_anchors.f32']if g['strategy']=='global'else all(r['raw_files'][n]['sha256']==expected[n]for n in ['block_anchors.f32','block_flags.u8'])
   assert all(checks.values()),('ORACLE_FUNCTIONAL_MISMATCH',rp,checks)
   dense=t['mode']in ['dense','token_dense']
   if dense:assert arrays['output.f32']==expected['dense_output.f32'],('DENSE_ORACLE_MISMATCH',rp)
   op=rp.parent/'output.f32';assert sha(op)==arrays['output.f32'];out=op.read_bytes();assert len(out)==len(db)==len(rb)
   name=rp.parent.parent.name+'__'+t['name'];target=a.output/(name+'.json.gz')
   if target.exists():
    z=read(target);assert z['record_sha256']==sha(rp)and z['oracle_key']==key;answers.append(z);continue
   rows=[];aps=rps=0;ma=mr=0.
   for qi in range(Q):
    for h in range(H):
     sl=slice((qi*H+h)*stride,(qi*H+h+1)*stride);m=exact_metrics(out[sl],db[sl]);c=exact_metrics(out[sl],rb[sl]);aps+=m['absolute_epsilon_pass'];rps+=c['relative_epsilon_pass'];ma=max(ma,m['absolute_l2']);mr=max(mr,c['relative_l2']);rows.append(dict(query=qi,head=h,absolute01=m['absolute_epsilon_pass'],relative01=c['relative_epsilon_pass'],absolute_l2=m['absolute_l2'],relative_l2=c['relative_l2'],dense_error_units=m['squared_error_units_2p298'],capture_error_units=c['squared_error_units_2p298'],capture_norm_units=c['squared_reference_norm_units_2p298']))
   assert aps==Q*H,('ABSOLUTE_CONTRACT_FAILURE',rp,ma)
   z=dict(status='PASS_EXACT_INTEGER_ABSOLUTE_BUDGET',name=name,fixture=t['fixture'],heads=Q*H,absolute01_pass=aps,relative01_pass=rps,max_absolute_l2=ma,max_relative_l2=mr,oracle_key=key,oracle_dense_native_validated=dense,record_sha256=sha(rp),rows=rows)
   with gzip.open(target,'wt',compresslevel=1)as f:json.dump(z,f,separators=(',',':'))
   answers.append(z)
  return answers
 rows=[]
 with ThreadPoolExecutor(max_workers=a.workers)as pool:
  fs=[pool.submit(group,x)for x in groups.items()]
  for i,f in enumerate(as_completed(fs)):
   rows.extend(f.result())
   if(i+1)%24==0:print('reference_groups',i+1,'/',len(groups),flush=True)
 if a.include_cached_results:
  by_name={z['name']:z for z in rows}
  for p in a.output.glob('*.json.gz'):
   z=read(p);camp,name=z['name'].split('__',1);rp=next((root/name/'record.json.gz'for root in [R/'experiments'/camp,R/'recovered_evidence'/camp]if(root/name/'record.json.gz').exists()),None);assert rp and sha(rp)==z['record_sha256'];cert=read(cache/(z['oracle_key']+'.json'));assert cert['source_sha256']==source and cert['binary_sha256']==oh;assert z['absolute01_pass']==z['heads'];by_name[z['name']]=z
  rows=list(by_name.values())
 summaries=[{k:v for k,v in z.items()if k!='rows'}for z in rows];result=dict(status='PASS_FUNCTIONAL_ORACLE_AND_EXACT_INTEGER_ABSOLUTE_BUDGET',runs=len(rows),heads=sum(x['heads']for x in rows),absolute01_pass=sum(x['absolute01_pass']for x in rows),relative01_pass=sum(x['relative01_pass']for x in rows),native_dense_validations=sum(x['oracle_dense_native_validated']for x in rows),source_sha256=source,binary_sha256=oh,integer_gate_sha256=sha(R/'numerics/exact_quality.py'),rows=summaries)
 (a.output/'SUMMARY.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items()if k!='rows'}),flush=True)
if __name__=='__main__':main()
