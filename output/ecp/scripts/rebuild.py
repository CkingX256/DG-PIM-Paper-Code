from pathlib import Path
import json,argparse,subprocess,shutil,hashlib
R=Path(__file__).resolve().parents[3];U=R/'output/ecp';ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path,default=U/'variant');ap.add_argument('--output',type=Path,default=U/'build');a=ap.parse_args();base=R/'native_base';src=a.source.resolve();out=a.output.resolve();out.mkdir(exist_ok=False);(out/'bin').mkdir();v=base/'runtime/variants/lifecycle';ns=base/'baseline/reproduction/native/native_source'
flags=['g++','-O3','-march=x86-64-v3','-ffp-contract=off','-DNDEBUG','-std=c++20','-fPIC','-I',str(src),'-I',str(v/'source')]
for rel in ['ext/yaml-cpp/include','ext/spdlog/include','ext/argparse/include','src','src/memory_system/impl']:flags+=['-I',str(ns/rel)]
cmd=flags+['-c',str(src/'numeric_pair.cpp'),'-o',str(out/'numeric_pair.o')];subprocess.run(cmd,check=True);link=[]
for t in json.load(open(v/'BUILD_IDENTITY.json'))['link_command']:
 if t.startswith('/')and t.endswith('libramulator.so'):t=str(out/'bin/libramulator.so')
 elif t.endswith('numeric_pair.cpp.o'):t=str(out/'numeric_pair.o')
 elif t.endswith('hbm3_pim_controller.cpp.o'):t=str(v/'build/hbm3_pim_controller.cpp.o')
 elif '/native/native_build/'in t:t=str(base/'baseline/reproduction/native/native_build'/t.split('/native/native_build/',1)[1])
 link.append(t)
link[0]='g++';subprocess.run(link,check=True)
for n in ['ramulator2','audit_commands']:shutil.copy2(v/'bin'/n,out/'bin'/n);(out/'bin'/n).chmod(0o755)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest();(out/'BUILD_IDENTITY.json').write_text(json.dumps(dict(compile=cmd,link=link,source={p.name:sha(p)for p in src.iterdir()if p.is_file()},library_sha256=sha(out/'bin/libramulator.so')),indent=2));print('REBUILT',out,flush=True)
