                      
                                                                         
from pathlib import Path
import argparse,json,hashlib
import numpy as np
R=Path(__file__).resolve().parents[1]
def js(p):return json.loads(Path(p).read_text())
def main():
 a=argparse.ArgumentParser();a.add_argument('--records',type=Path,required=True);a.add_argument('--output',type=Path,required=True);args=a.parse_args();B=Path(js(R/'protocol/REGISTERED_PROTOCOL.json')['baseline_root']);records=js(args.records);dense={}
 for r in records:
  t=r['task'];key=(t['fixture'],t['cache'],t.get('parameters',{}).get('encoder_strategy','global'),t.get('channels',1))
  if t['mode']=='dense':dense[key]=r
 rows=[]
 for r in records:
  t=r['task'];d=args.records.parent/t['name'];N,H,D,Q=(r['result'][s]for s in ['max_tokens','H','D','Q']);f=t['fixture'];refpath=Path(t.get('reference',str(B/'reproduction/inputs'/f/'reference_fp32.npz')));z=np.load(refpath);ref=z['output'].astype('f8');out=np.fromfile(d/'output.f32',dtype='f4').reshape(Q,H,D).astype('f8');err=np.linalg.norm(out-ref,axis=-1);norm=np.linalg.norm(ref,axis=-1)
  key=(f,t['cache'],t.get('parameters',{}).get('encoder_strategy','global'),t.get('channels',1));control=dense.get(key);gap=None
  if control:
   den=np.fromfile(args.records.parent/control['task']['name']/'output.f32',dtype='f4').reshape(Q,H,D).astype('f8');gap=np.linalg.norm(out-den,axis=-1);assert np.all(gap<=.1),(t['name'],gap.max())
  rows.append(dict(name=t['name'],heads=Q*H,relative01_pass=int(np.sum(err<=.01*norm)),abs01_vs_capture_pass=int(np.sum(err<=.1)),max_relative_l2=float(np.max(err/np.maximum(norm,1e-300))),dense_control=control['task']['name']if control else None,absolute01_vs_native_dense_pass=int(np.sum(gap<=.1))if gap is not None else None,max_l2_vs_native_dense=float(gap.max())if gap is not None else None))
 result=dict(status='PASS_ALL_AVAILABLE_PAIRED_DENSE_CONTRACTS',records=len(rows),heads=sum(x['heads']for x in rows),relative01_pass=sum(x['relative01_pass']for x in rows),rows=rows);args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items()if k!='rows'}))
if __name__=='__main__':main()
