                      
                                                                            
from pathlib import Path
import argparse, csv, hashlib, json, math, time
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
def js(p):return json.loads(Path(p).read_text())
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def save(p,x):p.write_text(json.dumps(x,indent=2,allow_nan=False)+'\n')
def table(p,rows):
    with p.open('w',newline='')as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
def mean32(k):
    a=k[0].copy()
    for x in k[1:]:a=np.add(a,x,dtype=np.float32)
    return np.divide(a,np.float32(len(k)),dtype=np.float32)
def softmax(s):
    e=np.exp(s-np.max(s));return e/np.sum(e)
def encoded(q,k,v,anchors,B,L,fp64=False):
    ids=np.arange(L)//B;a=anchors[ids].astype('f8'if fp64 else'f4')
    if fp64:
        residual=k[:L].astype('f8')-a
        score=(residual@q.astype('f8')+a@q.astype('f8'))/math.sqrt(k.shape[1])
        return softmax(score)@v[:L].astype('f8'),score
    residual=np.subtract(k[:L],a,dtype='f4').astype('f2').astype('f4')
    scale=np.float32(1/math.sqrt(k.shape[1]));qq=q.astype('f2').astype('f4')
    score=np.add((residual@qq)*scale,(a@qq)*scale,dtype='f4')
    e=np.exp(score-np.max(score),dtype='f4');prob=(e/np.sum(e,dtype='f4')).astype('f2').astype('f4')
    return (prob@v[:L].astype('f2').astype('f4')).astype('f4'),score

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--baseline',type=Path);a=ap.parse_args()
    protocol=js(ROOT/'protocol/REGISTERED_PROTOCOL.json');base=a.baseline or Path(protocol['baseline_root'])
    fixtures=js(base/'reproduction/capture_metadata/FIXTURE_MANIFEST.json')
    rows=[];hybrid=[];cert=[];bind=[];began=time.monotonic();max_identity=0
    for ix,f in enumerate(fixtures):
        name=f['fixture'];path=base/'reproduction/inputs'/name/'reference_fp32.npz';z=np.load(path)
        q,k,v,ref=(z[n]for n in ['q','k','v','output']);N,D=k.shape;L0=N-3
        global_a=mean32(k[:L0]);G=float(np.max(np.linalg.norm(v.astype('f2').astype('f8'),axis=1)))
        bind.append({'fixture':name,'npz_sha256':sha(path),'input_sha256':sha(base/'reproduction/inputs'/name/'input.bin')})
        globals_={}
        for qi in range(4):
            for h in range(f['H']):globals_[qi,h]=encoded(q[qi,h],k,v,np.tile(global_a,(N,1)),1,L0+qi)[0]
        for B in protocol['numerical_grid']['block_sizes']:
            nb=(N+B-1)//B
                                                                          
                                                                            
                                                                           
            anchors=np.empty((nb,D),'f4')
            for b in range(nb):
                start=b*B;end=min((b+1)*B,L0)
                anchors[b]=mean32(k[start:end])if end>start else k[start]
            defaults={'oracle_global_diagnostic':global_a,'causal_first_block':anchors[0]}
            mixed={}
            for kind,default in defaults.items():
                triggers=[];out=anchors.copy()
                for b in range(nb):
                    start=b*B;end=min((b+1)*B,L0);kb=k[start:end]
                    assert len(kb)>0, 'Registered endpoints keep every anchor block in the initial prefix'
                    r0=np.subtract(kb,default,dtype='f4');rb=np.subtract(kb,anchors[b],dtype='f4')
                    e0=float(np.max(np.abs(r0.astype('f2').astype('f4')-r0)))
                    eb=float(np.max(np.abs(rb.astype('f2').astype('f4')-rb)))
                    hit=e0>2*eb;triggers.append(hit)
                    if not hit:out[b]=default
                mixed[kind]=out
                hybrid.append({'fixture':name,'model_index':f['model_index'],'dataset_index':f['dataset_index'],'round':f['round'],'layer':f['layer'],'N':N,'D':D,'B':B,'default':kind,'blocks':nb,'triggered':sum(triggers),'trigger_rate':sum(triggers)/nb,'adjacent_equal_rate':float(np.mean(np.array(triggers[1:])==np.array(triggers[:-1])))if nb>1 else 1,'trigger_bitmap':''.join('1'if t else'0'for t in triggers),'default_source_reads':'two_pass'if kind.startswith('oracle')else'one_pass'})
            for qi in range(4):
                L=L0+qi
                for h in range(f['H']):
                    qq=q[qi,h];ideal=softmax(k[:L].astype('f8')@qq.astype('f8')/math.sqrt(D))@v[:L].astype('f8')
                    fp64,_=encoded(qq,k,v,anchors,B,L,True);gap=float(np.linalg.norm(fp64-ideal));max_identity=max(max_identity,gap)
                    assert gap<=2e-10*(1+np.linalg.norm(ideal)),(name,B,qi,h,gap)
                    golden=ref[qi,h].astype('f8');norm=float(np.linalg.norm(golden));global_err=float(np.linalg.norm(globals_[qi,h].astype('f8')-golden))
                    for variant,aa in [('block',anchors),*mixed.items()]:
                        out,scores=encoded(qq,k,v,aa,B,L);err=float(np.linalg.norm(out.astype('f8')-golden))
                        rows.append({'fixture':name,'model_index':f['model_index'],'dataset_index':f['dataset_index'],'round':f['round'],'layer':f['layer'],'N':N,'D':D,'B':B,'query':qi,'head':h,'variant':variant,'absolute_l2':err,'relative_l2':err/norm if norm else 0,'global_absolute_l2':global_err,'wins_vs_global':err<=global_err,'absolute_01':err<=.1,'relative_01':err<=.01*norm,'fp64_identity_l2':gap,'fp64_byte_equal':bool(np.array_equal(fp64,ideal))})
                                                                            
                                                                              
                    ids=np.arange(L)//B;aa=anchors[ids];res=np.subtract(k[:L],aa,dtype='f4').astype('f2').astype('f8');qh=qq.astype('f2').astype('f8')
                    corr=(anchors.astype('f8')@qh)/math.sqrt(D)
                    exact_s=(res@qh)/math.sqrt(D)+corr[ids]
                    qnorm=np.linalg.norm(qh);sigma=float(np.std(corr[:(L+B-1)//B]));seedmax=float(np.max(exact_s[:min(B,L)]))
                    seedden=float(np.sum(np.exp(exact_s[:min(B,L)]-seedmax)));m=float(np.max(exact_s))
                    records=[]
                    for b in range((L+B-1)//B):
                        sl=slice(b*B,min((b+1)*B,L));rr=float(np.max(np.linalg.norm(res[sl],axis=1)))
                                                                             
                        bound=corr[b]+qnorm*rr/math.sqrt(D)
                        slack=64*np.finfo('f8').eps*(1+abs(corr[b])+qnorm*rr/math.sqrt(D))
                        upper=np.nextafter(bound+slack,np.inf);actual=float(np.max(exact_s[sl]));assert upper>=actual,(name,B,qi,h,b)
                        records.append((upper,actual,sl.stop-sl.start,b))
                    for c in protocol['numerical_grid']['certificate_sigma_multipliers']:
                        margin=c*sigma;eligible=[x for x in records if x[0]<=m-margin]
                                                                                 
                        spent=0.;picked=0
                        for upper,actual,count,b in sorted(records):
                            if b==0 or upper>seedmax-margin:continue
                            mass=count*math.exp(min(upper-seedmax,700))/seedden
                            if 2*G*(spent+mass)<=protocol['numerical_grid']['qk_absolute_budget']:
                                spent+=mass;picked+=1
                        cert.append({'fixture':name,'model_index':f['model_index'],'dataset_index':f['dataset_index'],'round':f['round'],'layer':f['layer'],'N':N,'D':D,'B':B,'query':qi,'head':h,'c':c,'sigma_c':sigma,'absolute_margin':margin,'groups':len(records),'free_true_max_margin_hits':len(eligible),'causal_seed_budget_hits':picked,'mass_upper_sum':spent,'output_bound_2G_mass':2*G*spent,'upper_bound_violations':0,'source':'offline_FP64_encoded_logit_opportunity; native rounding not certified'})
        if(ix+1)%18==0:print(json.dumps({'fixtures':ix+1,'registered':216}),flush=True)
    table(ROOT/'numerics/HEAD_RESULTS.csv',rows);table(ROOT/'numerics/HYBRID_BLOCKS.csv',hybrid);table(ROOT/'numerics/CERTIFICATE_SCREEN.csv',cert)
    summary={'status':'PASS_COMPLETE_OFFLINE_REAL_CAPTURE_CONTRACTS','fixtures':216,'head_variant_rows':len(rows),'certificate_rows':len(cert),'hybrid_rows':len(hybrid),'max_fp64_reconstruction_l2':max_identity,'fp64_byte_equality_required':False,'new_native_simulator_calls':0,'nature':'Actual CPU numerical experiments; no cycle performance claims.','npz_bindings':bind,'protocol_sha256':sha(ROOT/'protocol/REGISTERED_PROTOCOL.json'),'host_seconds':time.monotonic()-began,'by_B_variant':[],'certificate_by_B_c':[],'hybrid_by_B_default':[]}
    for B in protocol['numerical_grid']['block_sizes']:
        for var in ['block','oracle_global_diagnostic','causal_first_block']:
            x=[r for r in rows if r['B']==B and r['variant']==var]
            summary['by_B_variant'].append({'B':B,'variant':var,'heads':len(x),'win_rate_vs_global':np.mean([r['wins_vs_global']for r in x]).item(),'abs01_pass':sum(r['absolute_01']for r in x),'relative01_pass':sum(r['relative_01']for r in x),'max_relative_l2':max(r['relative_l2']for r in x)})
        for kind in ['oracle_global_diagnostic','causal_first_block']:
            x=[r for r in hybrid if r['B']==B and r['default']==kind]
            summary['hybrid_by_B_default'].append({'B':B,'default':kind,'median_trigger_rate':float(np.median([r['trigger_rate']for r in x])),'mean_trigger_rate':float(np.mean([r['trigger_rate']for r in x]))})
        for c in protocol['numerical_grid']['certificate_sigma_multipliers']:
            x=[r for r in cert if r['B']==B and r['c']==c]
            summary['certificate_by_B_c'].append({'B':B,'c':c,'groups':sum(r['groups']for r in x),'free_max_margin_hits':sum(r['free_true_max_margin_hits']for r in x),'causal_budget_hits':sum(r['causal_seed_budget_hits']for r in x),'upper_bound_violations':sum(r['upper_bound_violations']for r in x)})
    save(ROOT/'numerics/SUMMARY.json',summary);print(json.dumps({k:v for k,v in summary.items()if k!='npz_bindings'}),flush=True)
if __name__=='__main__':main()
