                      
                                                                          
from pathlib import Path
import argparse,csv,hashlib,json,os,shutil,subprocess,tempfile
import numpy as np
from exact_quality import exact_metrics,controls
R=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def js(p):return json.loads(Path(p).read_text())
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--records',type=Path,action='append',required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--cache',type=Path,default=Path('/tmp/dg_dense_reference_cache'));a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True);a.cache.mkdir(parents=True,exist_ok=True);B=Path(js(R/'protocol/REGISTERED_PROTOCOL.json')['baseline_root']);oracle=R/'numerics/native_oracle';oracle_hash=sha(oracle);rows=[];function_fail=[];cache={}
 controls();old={r['name']:r for r in js(B/'runtime/FULL_RECORDS.json')}
 fields=['campaign','run','query','head','absolute_l2_vs_dense','absolute01_vs_dense_pass','relative_l2_vs_capture','relative01_vs_capture_pass','squared_error_dense_units_2p298','squared_error_capture_units_2p298','squared_capture_norm_units_2p298']
 with(a.output/'HEAD_QUALITY.csv').open('w',newline='')as file:
  w=csv.DictWriter(file,fieldnames=fields);w.writeheader()
  for path in a.records:
   for i,r in enumerate(js(path)):
    t=r['task'];d=path.parent/t['name'];f=t['fixture'];params=t.get('parameters',{});strategy=params.get('encoder_strategy','global');size=params.get('encoder_block_size',64);packed=r['result']['token_packed'];C=t.get('channels',1)
    inp=Path(t.get('input',str(B/'reproduction/inputs'/f/'input.bin')));raw=Path(t.get('raw_keys',str(B/'runtime/fixtures'/f/'raw_keys.f32')));refpath=Path(t.get('reference',str(B/'reproduction/inputs'/f/'reference_fp32.npz')))
    key=hashlib.sha256(json.dumps([sha(inp),strategy,size,packed,C,oracle_hash],separators=(',',':')).encode()).hexdigest();cp=a.cache/(key+'.json');densepath=a.cache/(key+'.dense.f32')
    if key not in cache:
     if not cp.exists():
      with tempfile.TemporaryDirectory(prefix='dg_qref_',dir='/tmp')as td:
       subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(oracle.resolve()),str(inp),str(raw),strategy,str(size),str(int(packed)),td,str(C)],check=True)
       expected={n:sha(Path(td)/n)for n in ['scores.f32','probability.f16','encoded_keys.f16','block_anchors.f32','block_flags.u8','dense_output.f32']};shutil.copy2(Path(td)/'dense_output.f32',densepath);cp.write_text(json.dumps(dict(expected=expected,source_sha256=sha(R/'numerics/native_oracle.cpp'),binary_sha256=oracle_hash,input_sha256=sha(inp),raw_keys_sha256=sha(raw),strategy=strategy,B=size,packed=packed,channels=C),indent=2)+'\n')
     cache[key]=js(cp)
    expected=cache[key]['expected'];assert sha(densepath)==expected['dense_output.f32']
    checks={n:r['raw_files'][n]['sha256']==expected[n]for n in ['scores.f32','probability.f16','encoded_keys.f16']}
    if strategy!='global':checks.update({n:r['raw_files'][n]['sha256']==expected[n]for n in ['block_anchors.f32','block_flags.u8']})
    else:checks['anchor.f32']=r['raw_files']['anchor.f32']['sha256']==expected['block_anchors.f32']
    if not all(checks.values()):function_fail.append(dict(name=t['name'],checks=checks));raise AssertionError(function_fail[-1])
    if t['mode']in ['dense','token_dense']:assert r['arrays']['output.f32']==expected['dense_output.f32'],('Native Dense differs from independently validated oracle',t['name'])
    assert sha(d/'output.f32')==r['arrays']['output.f32'];out=(d/'output.f32').read_bytes();den=densepath.read_bytes();z=np.load(refpath);ref=np.ascontiguousarray(z['output'],dtype='<f4');Q,H,D=ref.shape;reference=ref.tobytes();stride=4*D;absolute_pass=relative_pass=0;max_abs=max_rel=0.;gate=hashlib.sha256()
    for qi in range(Q):
     for h in range(H):
      sl=slice((qi*H+h)*stride,(qi*H+h+1)*stride);m=exact_metrics(out[sl],den[sl]);c=exact_metrics(out[sl],reference[sl]);absolute_pass+=m['absolute_epsilon_pass'];relative_pass+=c['relative_epsilon_pass'];max_abs=max(max_abs,m['absolute_l2']);max_rel=max(max_rel,c['relative_l2']);gate.update((m['squared_error_units_2p298']+':'+c['squared_error_units_2p298']+':'+c['squared_reference_norm_units_2p298']+'\n').encode())
      w.writerow(dict(campaign=path.parent.name,run=t['name'],query=qi,head=h,absolute_l2_vs_dense=m['absolute_l2'],absolute01_vs_dense_pass=m['absolute_epsilon_pass'],relative_l2_vs_capture=c['relative_l2'],relative01_vs_capture_pass=c['relative_epsilon_pass'],squared_error_dense_units_2p298=m['squared_error_units_2p298'],squared_error_capture_units_2p298=c['squared_error_units_2p298'],squared_capture_norm_units_2p298=c['squared_reference_norm_units_2p298']))
    assert absolute_pass==Q*H,('Absolute numerical contract violation',t['name'],max_abs)
    old_equiv=None
    if params.get('encoder_anchor_sample')=='retained_prefill_mean'and Q==4 and t['mode']in ['lazy_fused','token_lazy']and C==1 and t.get('alu',16)==16 and t.get('bytes',32)==32:
     old_equiv=r['arrays']==old[f+'__'+t['mode']+'__'+t['cache']]['numerical_array_hashes'];assert old_equiv,('Same-format retention changed canonical bytes',t['name'])
    rows.append(dict(campaign=path.parent.name,name=t['name'],heads=Q*H,absolute01_vs_dense_pass=absolute_pass,relative01_vs_capture_pass=relative_pass,max_absolute_l2_vs_dense=max_abs,max_relative_l2_vs_capture=max_rel,score_probability_encoding_checks=checks,dense_reference_key=key,exact_gate_digest=gate.hexdigest(),retained_original_six_arrays_equal=old_equiv))
    if(i+1)%72==0:print(json.dumps(dict(campaign=path.parent.name,complete=i+1)),flush=True)
 result=dict(status='PASS_BYTE_EXACT_FUNCTIONAL_CONTRACT_AND_INTEGER_ABSOLUTE_BUDGET',runs=len(rows),heads=sum(x['heads']for x in rows),absolute01_vs_dense_pass=sum(x['absolute01_vs_dense_pass']for x in rows),relative01_vs_capture_pass=sum(x['relative01_vs_capture_pass']for x in rows),oracle_source_sha256=sha(R/'numerics/native_oracle.cpp'),oracle_binary_sha256=oracle_hash,exact_gate_source_sha256=sha(R/'numerics/exact_quality.py'),head_csv_sha256=sha(a.output/'HEAD_QUALITY.csv'),rows=rows);(a.output/'SUMMARY.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items()if k!='rows'}),flush=True)
if __name__=='__main__':main()
