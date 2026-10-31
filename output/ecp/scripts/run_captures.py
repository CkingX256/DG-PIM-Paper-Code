from common import *
import subprocess,sys,time
models={x['model']:x for x in read(U/'MODELS_PINNED.json')};done=[]
for x in read(U/'PROTOCOL.json')['heldout']:
 m=models[x['model']];name=f"new_m{x['model']}_c{x['context']}_q2048";out=U/'captures_new'/name
 if(out/'PROVENANCE.json').exists():
  p=read(out/'PROVENANCE.json')
  for n,h in p['files'].items():assert sha(out/n)==h
  done.append(dict(name=name,status='REUSED_EXACT_CAPTURE',provenance=p));continue
 assert not out.exists(),'Retain/classify incomplete capture before replay'
 cmd=[sys.executable,str(U/'scripts/capture_real.py'),'--snapshot',m['snapshot'],'--model-id',m['name'],'--revision',m['revision'],'--text',str(U/'contexts'/f"context_{x['context']}.txt"),'--l0','257','--queries','2048','--layer',str(m['layer']),'--owner','0','--output',str(out)]
 begin=time.monotonic();r=subprocess.run(cmd,capture_output=True,text=True);folder=U/'capture_attempts'/name;folder.mkdir(parents=True,exist_ok=True);(folder/'stdout.log').write_text(r.stdout);(folder/'stderr.log').write_text(r.stderr);save(folder/'attempt.json',dict(command=cmd,returncode=r.returncode,seconds=time.monotonic()-begin));assert r.returncode==0,(name,r.stderr[-2500:]);p=read(out/'PROVENANCE.json');assert p['Q']==2048 and p['N']==2304
 done.append(dict(name=name,status='REAL_MODEL_CAPTURE_COMPLETE',provenance=p));save(U/'NEW_CAPTURES_COMPLETE.json',done);print('REAL_Q2048_CAPTURE',name,p['H'],p['D'],flush=True)
assert len(done)==12;print('ALL12_NEW_REAL_Q2048_CAPTURES_COMPLETE',flush=True)
