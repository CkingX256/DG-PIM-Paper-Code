                                                                              
from pathlib import Path
import gzip,json,sys
R=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(R/'statistics'))
from aggregate_v2 import load
from exact_quality import exact_metrics
def main():
 rows=[];heads=0;passed=0;maxabs=maxrel=0.
 for campaign in ['stationary_channels','stationary_long']:
  rr=load(campaign);assert len(rr)==(864 if campaign=='stationary_channels'else 96)
  ix={(r['task']['fixture'],r['task']['cache'],r['task']['method'],r['task']['channels']):r for r in rr}
  for(f,c,m,C),r in ix.items():
   if m!='reuse'or C==1:continue
   one=ix[f,c,m,1]
   for k in ['scores.f32','probability.f16','mask.u8','encoded_keys.f16','anchor.f32']:assert r['arrays'][k]==one['arrays'][k]
   a=(Path(r['_path']).parent/'output.f32').read_bytes();b=(Path(one['_path']).parent/'output.f32').read_bytes();assert len(a)==len(b);D=r['result']['D'];stride=4*D;ma=mr=0.;n=0
   for i in range(0,len(a),stride):
    z=exact_metrics(a[i:i+stride],b[i:i+stride]);assert z['absolute_epsilon_pass'];n+=1;heads+=1;passed+=1;ma=max(ma,z['absolute_l2']);mr=max(mr,z['relative_l2'])
   maxabs=max(maxabs,ma);maxrel=max(maxrel,mr);rows.append(dict(campaign=campaign,fixture=f,state=c,channels=C,heads=n,intermediate_five_arrays_equal=True,output_byte_equal=r['arrays']['output.f32']==one['arrays']['output.f32'],max_absolute_l2=ma,max_relative_l2=mr))
 out=dict(status='PASS_CROSS_CHANNEL_NUMERICAL_REQUALIFICATION',pairs=len(rows),heads=heads,absolute01_pass=passed,max_absolute_l2=maxabs,max_relative_l2=maxrel,scope='PV reduction order changes across channel counts. Five intermediate arrays match. Each channel also has its own independently byte-validated Dense reference and exact absolute sparse-budget audit.',rows=rows)
 (R/'numerics/CROSS_CHANNEL_REQUALIFICATION.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:v for k,v in out.items()if k!='rows'}),flush=True)
if __name__=='__main__':main()
