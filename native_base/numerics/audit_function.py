                      
from pathlib import Path
import argparse,hashlib,json,subprocess,tarfile,tempfile
R=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--records',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();B=Path(json.load(open(R/'protocol/REGISTERED_PROTOCOL.json'))['baseline_root']);records=json.load(open(a.records));rows=[];cache={};(R/'numerics/native_oracle').chmod(0o755)
 for r in records:
  t=r['task'];s=t.get('parameters',{}).get('encoder_strategy','global')
  if s=='global':continue
  inp=Path(t.get('input',str(B/'reproduction/inputs'/t['fixture']/'input.bin')));raw=Path(t.get('raw_keys',str(B/'runtime/fixtures'/t['fixture']/'raw_keys.f32')));bs=t['parameters'].get('encoder_block_size',64);packed=r['result']['token_packed'];key=(sha(inp),s,bs,packed)
  if key not in cache:
   with tempfile.TemporaryDirectory(prefix='dg_oracle_',dir='/tmp')as out:
    subprocess.run([str((R/'numerics/native_oracle').resolve()),str(inp),str(raw),s,str(bs),str(int(packed)),out],check=True)
    cache[key]={n:sha(Path(out)/n)for n in ['encoded_keys.f16','block_anchors.f32','block_flags.u8','scores.f32']}
  expected=cache[key];actual={n:r['raw_files'][n]['sha256']for n in expected}
  assert actual==expected,(t['name'],actual,expected)
  rows.append(dict(name=t['name'],expected=expected,status='EXACT_PAYLOAD_ANCHORS_FLAGS_AND_SCORES'))
 result=dict(status='PASS_INDEPENDENT_ORDERED_FP32_FUNCTIONAL_ORACLE',runs=len(rows),oracle_source_sha256=sha(R/'numerics/native_oracle.cpp'),oracle_binary_sha256=sha(R/'numerics/native_oracle'),rows=rows);a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items()if k!='rows'}))
if __name__=='__main__':main()
