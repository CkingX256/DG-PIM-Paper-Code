



   
from common import *
import argparse,sys,subprocess,tempfile,csv,numpy as np
sys.path.insert(0,str(R/'native_base/numerics'));from exact_quality import exact_metrics,controls
ap=argparse.ArgumentParser();ap.add_argument('--available',action='store_true');ap.add_argument('--recompute',action='store_true',help='Regenerate every independent oracle after compiling or relocating tools');ap.add_argument('--oracle-dir',type=Path,default=U/'oracles');ap.add_argument('--results-dir',type=Path,default=U/'results');a=ap.parse_args();a.results_dir.mkdir(parents=True,exist_ok=True);controls();tasks=read(U/'ORIGINAL_BASE_TASKS.json')+read(U/'NEW_BASE_TASKS.json');rows=[];contexts=[]
for t in tasks:
 p=rec(t['name'])
 if not p.exists():
  assert a.available,('Registered baseline missing',t['name']);continue
 r=read(p);z=r['result'];out=(p.parent/'output.f32').read_bytes();assert sha(p.parent/'output.f32')==r['arrays']['output.f32'];root=a.oracle_dir/t['fixture'];root.mkdir(parents=True,exist_ok=True)
 cert=root/'CERTIFICATE.json'
 assert r['arrays']['mask.u8']==hashlib.sha256(bytes(z['Q']*z['H']*((z['max_tokens']+15)//16))).hexdigest(),'Dense mask must be entirely zero'
 if cert.exists()and not a.recompute:
  c=read(cert);assert c['input_sha256']==sha(resolve(t['input']))and c['raw_keys_sha256']==sha(resolve(t['raw_keys']))and c['oracle_sha256']==sha(U/'scripts/independent_oracle');assert sha(root/'dense_output.f32')==c['files']['dense_output.f32']
 else:
  with tempfile.TemporaryDirectory(prefix='ecp26_oracle_',dir='/dev/shm')as td:
   p0=subprocess.run([str(U/'scripts/independent_oracle'),str(resolve(t['input'])),str(resolve(t['raw_keys'])),'global','0','1',td,'1'],capture_output=True,text=True);assert p0.returncode==0,p0.stderr[-1000:];td=Path(td);files={x.name:sha(x)for x in td.iterdir()if x.is_file()}
   for native,oracle in [('anchor.f32','block_anchors.f32'),('encoded_keys.f16','encoded_keys.f16'),('scores.f32','scores.f32'),('probability.f16','probability.f16')]:assert r['arrays'][native]==files[oracle],(t['name'],native)
   witness_root=U/'fixed_normalizer_witnesses'/t['fixture']
   if(witness_root/'CERTIFICATE.json').exists():
    wc=read(witness_root/'CERTIFICATE.json');assert wc['score_sha256']==files['scores.f32']and wc['witness_npz_sha256']==sha(witness_root/'witnesses.npz')
    sc=np.fromfile(td/'scores.f32',dtype='<f4').reshape(z['Q'],z['H'],z['max_tokens']);wv=np.load(witness_root/'witnesses.npz');scale=np.float32(np.float32(1)/np.sqrt(np.float32(z['D'])))
    for wr in wc['rows']:
     for side in ['low','high']:
      qx,hx=wr['query_'+side],wr['head'];Lx=z['steps'][qx]['length'];expected=np.multiply(sc[qx,hx,:Lx],scale,dtype=np.float32)
      assert Lx==wr['length_'+side]and expected.tobytes()==wv[f'h{hx}_{side}'].tobytes(),('Witness membership mismatch',t['fixture'],hx,side)
    del sc,wv
   (root/'dense_output.f32').write_bytes((td/'dense_output.f32').read_bytes());save(cert,dict(input_sha256=sha(resolve(t['input'])),raw_keys_sha256=sha(resolve(t['raw_keys'])),oracle_sha256=sha(U/'scripts/independent_oracle'),files=files))
 assert out==(root/'dense_output.f32').read_bytes(),('Native baseline differs from independent oracle',t['name'])
 ref=np.ascontiguousarray(np.load(resolve(t['reference']))['output'],dtype='<f4');Q,H,D=ref.shape;assert (Q,H,D)==(z['Q'],z['H'],z['D']);rb=ref.tobytes();local=[]
 for q in range(Q):
  for h in range(H):
   sl=slice((q*H+h)*D*4,(q*H+h+1)*D*4);m=exact_metrics(out[sl],rb[sl]);row=dict(fixture=t['fixture'],population='heldout'if t['fixture'].startswith('new_')else'original',Q=Q,query=q,head=h,relative01_pass=m['relative_epsilon_pass'],relative_l2=m['relative_l2'],absolute_l2=m['absolute_l2'],squared_error_units=m['squared_error_units_2p298'],reference_norm_units=m['squared_reference_norm_units_2p298']);rows.append(row);local.append(row)
 contexts.append(dict(fixture=t['fixture'],population=local[0]['population'],heads=len(local),relative01_pass=sum(x['relative01_pass']for x in local),worst_relative=max(x['relative_l2']for x in local),native_matches_independent_dense=True));print('INDEPENDENT_QUALITY',t['fixture'],contexts[-1]['relative01_pass'],len(local),flush=True)
if not a.available:assert len(contexts)==22 and len(rows)==137216
summary={}
for population in ['original','heldout']:
 rr=[x for x in rows if x['population']==population]
 if rr:summary[population]=dict(unique_heads=len(rr),relative01_pass=sum(x['relative01_pass']for x in rr),worst_relative=max(x['relative_l2']for x in rr))
for name,rr in [('quality_heads.csv',rows),('quality_contexts.csv',contexts)]:
 with(a.results_dir/name).open('w')as f:w=csv.DictWriter(f,fieldnames=list(rr[0]));w.writeheader();w.writerows(rr)
save(a.results_dir/'QUALITY_AUDIT.json',dict(status='ALL22_INDEPENDENT_ORACLES_AND_137216_EXACT_HEAD_GATES_COMPLETE'if not a.available else'PARTIAL_PROGRESS_ONLY',contexts=contexts,summary=summary,total_heads=len(rows),scope='All registered heads count,including every relative miss. Exact ECP image equality is distinct from captured-FP32 relative accuracy. Producer/variant repeats do not enlarge the denominator.'))
