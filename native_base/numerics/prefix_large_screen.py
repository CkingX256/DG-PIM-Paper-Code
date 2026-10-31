                      
from pathlib import Path
import sys,json,math,hashlib
import numpy as np
sys.path.insert(0,str(Path(__file__).parent));from run_contracts import mean32,encoded
R=Path(__file__).resolve().parents[1];B=Path(json.load(open(R/'protocol/REGISTERED_PROTOCOL.json'))['baseline_root']);rows=[]
for f in json.load(open(B/'reproduction/capture_metadata/FIXTURE_MANIFEST.json')):
 z=np.load(B/'reproduction/inputs'/f['fixture']/'reference_fp32.npz');q,k,v,ref=(z[n]for n in ['q','k','v','output']);N,D=k.shape
 for sample in [128,256,512,1024]:
  a=mean32(k[:min(sample,N-3)]);aa=np.tile(a,(N,1));errs=[]
  for qi in range(4):
   for h in range(f['H']):
    out,_=encoded(q[qi,h],k,v,aa,1,N-3+qi);err=float(np.linalg.norm(out.astype('f8')-ref[qi,h]));norm=float(np.linalg.norm(ref[qi,h].astype('f8')));errs.append(err/norm)
  rows.append(dict(fixture=f['fixture'],sample=sample,heads=len(errs),relative01_pass=sum(x<=.01 for x in errs),max_relative_l2=max(errs)))
r={'nature':'Adaptive hypothesis after centered/hybrid screening, untimed NumPy numerical screen; no native or quality-contract claim.','rows':rows,'summary':[{ 'sample':s,'heads':sum(x['heads']for x in rows if x['sample']==s),'relative01_pass':sum(x['relative01_pass']for x in rows if x['sample']==s),'max_relative_l2':max(x['max_relative_l2']for x in rows if x['sample']==s)}for s in [128,256,512,1024]]};(R/'numerics/PREFIX_LARGE_SCREEN.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r['summary']))
