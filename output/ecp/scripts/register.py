from common import *
import copy
assert read(U/'PROTOCOL.json')['status']=='FROZEN_BEFORE_NEW_CAPTURES_OR_TIMINGS'
baseparams=dict(encoder_anchor_sample='buffered_prefill_mean',producer_stationary_operands=True,stationary_coordinate_capacity=128,stationary_fixed_physical_bank=True,producer_scratch_bytes_per_cycle=64,source_ingress_mode='forward',lifecycle_source_spill=True,producer_stage_in_alias=False,producer_ingress_mean_hardware=True,producer_ingress_mean_fusion=True,append_source_ingress_mode='forward',producer_dma_online=True,early_anchor_tokens=128,native_channels=1,channel_work_conserving_credits=True,channel_fair_credit_pool=True,compact_channel_state=True,scratch_capacity_bytes=262144,qk_reuse_pipeline=False,qk_priority_first=False,qk_exact_exp_cache=False,qk_readout_first=True,qk_exp_policy='predictive',qk_pipeline_depth=2,qk_pipeline_startup=2,inplace_exp=True,qk_cache_reserve_only=False)
arms={'serial':{},'pipeline':dict(qk_reuse_pipeline=True),'priority':dict(qk_reuse_pipeline=True,qk_priority_first=True),'priority_shadow_cache':dict(qk_reuse_pipeline=True,qk_priority_first=True,qk_cache_reserve_only=True),'cache_all_no_priority':dict(qk_reuse_pipeline=True,qk_exact_exp_cache=True,qk_exp_policy='all',inplace_exp=False),'cache_all_priority':dict(qk_reuse_pipeline=True,qk_priority_first=True,qk_exact_exp_cache=True,qk_exp_policy='all',inplace_exp=False),'serial_bank_cache':dict(qk_reuse_pipeline=True,qk_priority_first=True,qk_exact_exp_cache=True,qk_pipeline_depth=1,inplace_exp=False),'eager_full':dict(qk_reuse_pipeline=True,qk_priority_first=True,qk_exact_exp_cache=True,qk_readout_first=False,inplace_exp=False),'full':dict(qk_reuse_pipeline=True,qk_priority_first=True,qk_exact_exp_cache=True,inplace_exp=False)}
old=sorted(p.parent.name for p in(U/'original_captures/captures').glob('*/input.bin'));new=[f'new_m{m}_c{c}_q2048'for m in [1,2,3]for c in [4,5,6,7]]
def task(fixture,arm='serial',name=None,producer='L'):
 folder=U/('captures_new'if fixture.startswith('new_')else'original_captures/captures')/fixture
 params=copy.deepcopy(baseparams);params.update(arms[arm])
 if producer=='Bc':params.update(lifecycle_source_spill=False,append_source_ingress_mode='materialize',canonical_append_read_bypass=True)
 return dict(name=name or(('base__'if arm=='serial'else arm+'__')+fixture),fixture=fixture,arm=arm,producer=producer,input=str(folder/'input.bin'),raw_keys=str(folder/'raw_keys.f32'),reference=str(folder/'reference_fp32.npz'),parameters=params,cache='existing',mode='token_dense',channels=1,bytes=32,alu=16,capacity=262144,binary=str(U/'build/bin/ramulator2'),auditor=str(U/'build/bin/audit_commands'),pipeline_auditor=str(U/'scripts/audit_pipeline'),expected_record=None if arm=='serial'and producer=='L'else str(rec('base__'+fixture)))
groups={'ORIGINAL_BASE_TASKS.json':[task(f)for f in old],'ABLATION_TASKS.json':[task(f,a)for f in old for a in arms if a!='serial'],'NEW_BASE_TASKS.json':[task(f)for f in new],'NEW_FULL_TASKS.json':[task(f,'full')for f in new],'CANONICAL_FULL_TASKS.json':[task(f,'full',name='canonical_full__'+f,producer='Bc')for f in old+new]}
points={'capacity384':dict(parameters={'scratch_capacity_bytes':393216},capacity=393216),'capacity512':dict(parameters={'scratch_capacity_bytes':524288},capacity=524288),'scratch16':dict(bytes=16),'scratch64':dict(bytes=64),'credits16':dict(parameters={'actor_inflight_cap':16}),'credits32':dict(parameters={'actor_inflight_cap':32}),'mac7':dict(parameters={'mac_latency':7},controller_parameters={'numeric_mac_latency':7}),'mac8':dict(parameters={'mac_latency':8},controller_parameters={'numeric_mac_latency':8}),'maxgroup2':dict(parameters={'max_group_cycles':2}),'startup8':dict(parameters={'qk_pipeline_startup':8})}
structural=[f'real_m{m}_c0_q1024'for m in range(4)]+[f'new_m{m}_c4_q2048'for m in [1,2,3]];sens=[];pairs=[]
for f in structural:
 for point,delta in points.items():
  names={}
  for arm in ['serial','full']:
   if point=='startup8'and arm=='serial':names[arm]='base__'+f;continue
   t=task(f,arm,name=f'sens__{point}__{arm}__{f}');t['expected_record']=str(rec('base__'+f));t['parameters'].update(delta.get('parameters',{}));t.update({k:v for k,v in delta.items()if k!='parameters'});sens.append(t);names[arm]=t['name']
  pairs.append(dict(fixture=f,point=point,baseline=names['serial'],candidate=names['full'],scope='both arms share changed resource;startup only affects the pipeline'))
                                                                                  
 pairs.append(dict(fixture=f,point='selector_plus1',baseline='base__'+f,candidate=f'sens__mac7__full__{f}',scope='candidate MAC+1 penalty;reference remains nominalMAC6'))
groups['SENSITIVITY_TASKS.json']=sens
for name,ts in groups.items():save(U/name,ts)
save(U/'SENSITIVITY_PAIRS.json',pairs);save(U/'ARMS.json',arms)
save(U/'REGISTRATION.json',dict(status='REGISTERED_BEFORE_ALL_SCIENTIFIC_NATIVE_EXECUTIONS',groups={n:dict(runs=len(ts),sha256=sha(U/n))for n,ts in groups.items()},total_unique_runs=sum(map(len,groups.values())),old_contexts=old,new_contexts=new,ablation_arms=list(arms),structural_sensitivity_contexts=structural,sensitivity_points=points,source=read(U/'PROTOCOL.json')['source'],scope='Original design population and12predeclared newheldoutcontexts separate. Inputs not yet present are pinned by immutable capture protocol,not selected after quality/timing. Every failure is retained.'))
print('REGISTERED',sum(map(len,groups.values())),{n:len(ts)for n,ts in groups.items()},flush=True)
