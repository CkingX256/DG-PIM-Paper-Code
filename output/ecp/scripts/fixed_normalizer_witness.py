






   
from common import *
import argparse,tarfile,numpy as np,math,csv
from decimal import Decimal,localcontext,ROUND_FLOOR,ROUND_CEILING
ap=argparse.ArgumentParser();ap.add_argument('--available',action='store_true');ap.add_argument('--recheck-intervals',action='store_true');a=ap.parse_args()
tasks=read(U/'ORIGINAL_BASE_TASKS.json')+read(U/'NEW_BASE_TASKS.json');rows=[]
def interval_sum(values,shift):
                                                                          
                                                                              
                                                                              
 with localcontext()as ctx:
  ctx.prec=80;ctx.Emin=-999999999;ctx.Emax=999999999
  lo=Decimal(0);hi=Decimal(0)
  for v in values:
   x=Decimal.from_float(float(v))
   ctx.rounding=ROUND_FLOOR;xl=x-shift
   ctx.rounding=ROUND_CEILING;xh=x-shift
   el=xl.exp().next_minus();eh=xh.exp().next_plus()
   ctx.rounding=ROUND_FLOOR;lo+=el
   ctx.rounding=ROUND_CEILING;hi+=eh
  return lo,hi
def witness(lo,hi):
 shift=Decimal.from_float(float(max(lo.max(),hi.max())))
 ll,lh=interval_sum(lo,shift);hl,hh=interval_sum(hi,shift)
 with localcontext()as ctx:
  ctx.prec=80;ctx.rounding=ROUND_FLOOR
  r=hl/lh
                                                           
                                                                       
  numerator=r-1;ctx.rounding=ROUND_CEILING;denominator=r+1
  ctx.rounding=ROUND_FLOOR;e=max(Decimal(0),numerator/denominator)
 return dict(common_shift=str(shift),Z_low_interval=[str(ll),str(lh)],Z_high_interval=[str(hl),str(hh)],ratio_lower=str(r),minimax_error_lower=str(e),excludes_uniform_one_percent=e>Decimal('0.01'))
for t in tasks:
 p=rec(t['name'])
 if not p.exists():assert a.available;continue
 r=read(p);z=r['result'];Q,H,D,N=z['Q'],z['H'],z['D'],z['max_tokens'];folder=U/'fixed_normalizer_witnesses'/t['fixture'];folder.mkdir(parents=True,exist_ok=True);cp=folder/'CERTIFICATE.json'
 if cp.exists():
  cert=read(cp);assert cert['score_sha256']==r['arrays']['scores.f32']and cert['witness_npz_sha256']==sha(folder/'witnesses.npz')
  if a.recheck_intervals:
   wv=np.load(folder/'witnesses.npz')
   for row in cert['rows']:
    h=row['head'];checked=witness(wv[f'h{h}_low'],wv[f'h{h}_high']);assert all(row[k]==v for k,v in checked.items()),('Interval recheck mismatch',t['fixture'],h)
  rows+=cert['rows'];continue
 assert sha(p.parent/'raw.tar.gz')==r['archive_sha256']
 with tarfile.open(p.parent/'raw.tar.gz','r:gz')as tar:data=tar.extractfile('scores.f32').read()
 assert hashlib.sha256(data).hexdigest()==r['arrays']['scores.f32'];sc=np.frombuffer(data,dtype='<f4').reshape(Q,H,N)
 scale=np.float32(np.float32(1)/np.sqrt(np.float32(D)));scaled=np.multiply(sc,scale,dtype=np.float32);del sc,data
 lengths=np.array([s['length']for s in z['steps']]);screen=np.empty((Q,H))
 for q,L in enumerate(lengths):
  x=scaled[q,:,:L].astype(np.float64);m=x.max(axis=1);screen[q]=m+np.log(np.exp(x-m[:,None]).sum(axis=1))
 witnesses={};local=[]
 for h in range(H):
  qlo,qhi=int(screen[:,h].argmin()),int(screen[:,h].argmax());lo=scaled[qlo,h,:lengths[qlo]].copy();hi=scaled[qhi,h,:lengths[qhi]].copy();witnesses[f'h{h}_low']=lo;witnesses[f'h{h}_high']=hi
  row=dict(fixture=t['fixture'],head=h,query_low=qlo,query_high=qhi,length_low=int(lengths[qlo]),length_high=int(lengths[qhi]),scope='ideal fixed-normalizer family on frozen binary32 logits; not published SoftmaxPIM quality or timing',**witness(lo,hi));local.append(row)
 np.savez_compressed(folder/'witnesses.npz',**witnesses);cert=dict(status='80_DIGIT_OUTWARD_INTERVAL_WITNESSES',score_sha256=r['arrays']['scores.f32'],scale_binary32_hex=scale.tobytes().hex(),witness_npz_sha256=sha(folder/'witnesses.npz'),rows=local);save(cp,cert);rows+=local;print('FIXED_NORMALIZER_WITNESS',t['fixture'],sum(x['excludes_uniform_one_percent']for x in local),H,flush=True)
if not a.available:assert len({r['fixture']for r in rows})==22
save(U/'results/FIXED_NORMALIZER_WITNESSES.json',dict(status='PARTIAL_PROGRESS_ONLY'if a.available else'ALL22_CONTEXT_WITNESSES_COMPLETE',heads=len(rows),excluding_one_percent=sum(x['excludes_uniform_one_percent']for x in rows),rows=rows,scope='Diagnostic contract incompatibility only; no inference about trained ConSmax task accuracy, finite-LUT outputs or measured SoftmaxPIM implementation. No new timing samples.'))
with(U/'results/fixed_normalizer_witnesses.csv').open('w',newline='')as f:
 flat=[{k:v for k,v in r.items()if not isinstance(v,list)}for r in rows];w=csv.DictWriter(f,fieldnames=list(flat[0]));w.writeheader();w.writerows(flat)
