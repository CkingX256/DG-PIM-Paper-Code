



   
from common import *
import argparse,shutil,subprocess,os
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);ap.add_argument('--jobs',type=int,default=2);a=ap.parse_args();out=a.output.resolve();out.mkdir(exist_ok=False);src=out/'source';base=R/'native_base';v=base/'runtime/variants/lifecycle/source';ns=base/'baseline/reproduction/native/native_source'
shutil.copytree(ns,src,ignore=shutil.ignore_patterns('*.so','*.o','__pycache__'))
impl=src/'src/memory_system/impl'
for p in (U/'variant').iterdir():
 if p.is_file():shutil.copy2(p,impl/p.name)
for name in ['producer_dma_bridge.h','block_encoding.inc']:shutil.copy2(v/name,impl/name)
for name in ['port_members.inc','port_metadata.inc']:shutil.copy2(base/'baseline/reproduction/native'/name,impl/name)
shutil.copy2(v/'hbm3_pim_controller.cpp',src/'src/dram_controller/impl/hbm3_pim_controller.cpp')
                                                                           
shutil.copy2(v/'producer_dma_bridge.h',src/'src/dram_controller/impl/producer_dma_bridge.h')
build=out/'cmake';flags='-O3 -march=x86-64-v3 -ffp-contract=off -DNDEBUG'
cmakebin=shutil.which('cmake')
if cmakebin is None:
 import cmake
 cmakebin=str(Path(cmake.CMAKE_BIN_DIR)/'cmake')
commands=[[cmakebin,'-S',str(src),'-B',str(build),'-DCMAKE_BUILD_TYPE=Release','-DFETCHCONTENT_FULLY_DISCONNECTED=ON','-DSPDLOG_BUILD_TESTS=OFF','-DYAML_CPP_BUILD_TESTS=OFF','-DCMAKE_CXX_FLAGS_RELEASE='+flags],[cmakebin,'--build',str(build),'-j',str(a.jobs)]]
for cmd in commands:subprocess.run(cmd,check=True)
dest=out/'bin';dest.mkdir();shutil.copy2(build/'ramulator2',dest/'ramulator2');shutil.copy2(build/'libramulator.so',dest/'libramulator.so')
cmd=['g++','-O3','-march=x86-64-v3','-std=c++20',str(v/'audit_commands.cpp'),'-o',str(dest/'audit_commands')];subprocess.run(cmd,check=True);commands.append(cmd)
save(out/'CLEAN_BUILD_IDENTITY.json',dict(status='EVERY_SIMULATOR_AND_DEPENDENCY_OBJECT_RECOMPILED_FROM_SUPPLIED_SOURCE',commands=commands,compiler=subprocess.check_output(['g++','--version'],text=True).splitlines()[0],files={p.name:sha(p)for p in dest.iterdir()},scope='Functional equivalence must be checked with native controls; this is not silicon synthesis.'))
print('CLEAN_REBUILT',out,flush=True)
