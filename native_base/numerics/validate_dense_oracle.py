from pathlib import Path
import hashlib,json,subprocess,tempfile
R=Path(__file__).resolve().parents[1];B=Path(json.load(open(R/'protocol/REGISTERED_PROTOCOL.json'))['baseline_root']);paths=[R/'runtime/block_pilot/RECORDS.json',R/'runtime/mc_pilot/RECORDS.json',Path('/tmp/dg_native_stable/retained_pilot/RECORDS.json'),Path('/tmp/dg_native_stable/packed_dense_quality/RECORDS.json')];rows=[];cache={};oracle=R/'numerics/native_oracle';oracle.chmod(0o755)
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
for path in paths:
 for r in json.load(open(path)):
  t=r['task']
  if t['mode']not in ['dense','token_dense']:continue
  f=t['fixture'];s=t.get('parameters',{}).get('encoder_strategy','global');ch=t.get('channels',1);packed=r['result']['token_packed'];key=(f,s,ch,packed)
  if key not in cache:
   with tempfile.TemporaryDirectory(dir='/tmp',prefix='dg_dense_')as td:
    subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(oracle.resolve()),str(B/'reproduction/inputs'/f/'input.bin'),str(B/'runtime/fixtures'/f/'raw_keys.f32'),s,'64',str(int(packed)),td,str(ch)],check=True)
    cache[key]={n:sha(Path(td)/n)for n in ['dense_output.f32','probability.f16','scores.f32','encoded_keys.f16']}
  expected=cache[key];checks={n:expected[n]==r['raw_files']['output.f32'if n=='dense_output.f32'else n]['sha256']for n in expected};rows.append(dict(name=t['name'],checks=checks));assert all(checks.values()),(t['name'],checks)
result={'status':'PASS_BYTE_EXACT_NATIVE_DENSE_FUNCTIONAL_ORACLE','runs':len(rows),'source_sha256':sha(R/'numerics/native_oracle.cpp'),'binary_sha256':sha(oracle),'rows':rows};(R/'numerics/DENSE_ORACLE_VALIDATION.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS',len(rows))
