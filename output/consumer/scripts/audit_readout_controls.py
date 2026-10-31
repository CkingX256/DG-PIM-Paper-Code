                                                                                
from consumer_stats import *
from raw_evidence import verify_archive
families=[('STREAM_REFINEMENT_TASKS.json','stream_refinement_native'),('READOUT_PILOT_TASKS.json','readout_pilot_native'),('READOUT_REAL_TASKS.json','readout_real_native'),('READOUT_LIGHT_TASKS.json','readout_light_native'),('READOUT_SENSITIVITY_TASKS.json','readout_sensitivity_native'),('READOUT_ADVERSARIAL_TASKS.json','readout_adversarial_native'),('READOUT_DISABLED_TASKS.json','readout_disabled_native')]
pins=read(C/'READOUT_BUILD.json')
for name,h in pins.items():assert sha(C/name)==h,(name,'build changed')
rows=[];sens=[];adv=[];callbacks=0
for manifest,family in families:
 for t in read(C/manifest):
  p=record(family,t);r,old=verify(t,p);z=r['result'];archive=p.parent/'raw.tar.gz';raw=verify_archive(p.parent,r['archive_sha256']);assert r['identities']['library']==pins['build_readout/bin/libramulator.so']
  D,H,Q=(z[k]for k in ['D','H','Q']);L0=z['steps'][0]['length'];S=z['buffered_prefill_retained_tokens'];N=z['max_tokens'];channels=t.get('channels',1)
  nk=H*((D+63)//64)*sum((L0+q+(32 if z['token_packed']else 16)-1)//(32 if z['token_packed']else 16)for q in range(Q));assert nk==z['qk_mac']
  ns=8*D*(L0-S)//32;floor=max((N*D+7)//8,(ns+channels-1)//channels)+(nk+channels-1)//channels;assert r['native_cycles']-1>=floor
  source=z['initial_source_write_commands']+z['append_source_write_commands']+z['encoder_raw_encode_read_commands']+z['encoder_raw_mean_read_commands'];assert source>=ns
  if z['lifecycle_source_spill']and z['append_source_ingress_mode']=='forward':assert source==ns
  if z['qk_reuse_pipeline']:assert z['exact_exp_calls']==H*sum(x['length']for x in z['steps'])+z['qpipe_spec_exp_calls']-z['qpipe_valid_exp_calls']
  callbacks+=z['callbacks'];rows.append(dict(family=family,name=t['name'],record=str(p.relative_to(R)),record_sha256=sha(p),archive=str(archive.relative_to(R)),archive_sha256=r['archive_sha256'],archive_bytes=raw['archive_bytes'],callbacks=z['callbacks'],native_elapsed=r['native_cycles']-1,issue_floor=floor,six_arrays_exact=True,cycles_equal=r['native_cycles']==old['native_cycles'],peak_scratch=z['peak_live_scratch_bytes']))
  if family=='readout_sensitivity_native':sens.append(dict(arm=t['consumer_arm'],fixture=t['fixture'],source_block=t['context_source_block'],baseline=old['native_cycles']-1,candidate=r['native_cycles']-1,baseline_q0=q0(old),candidate_q0=q0(r)))
  if family=='readout_adversarial_native':adv.append(dict(fixture=t['fixture'],six_arrays_exact=True,invalidations=z['qpipe_invalidations'],spec_exp_calls=z['qpipe_spec_exp_calls'],reused_exp_calls=z['qpipe_valid_exp_calls'],recomputed_exp_calls=z['qpipe_recomputed_exp_calls']))
summary={arm:stats([r for r in sens if r['arm']==arm],False)for arm in sorted({r['arm']for r in sens})}
assert len(rows)==88
(C/'results/READOUT_CONTROLS_AUDIT.json').write_text(json.dumps(dict(status='ALL88_READOUT_RELEASE_AND_REFINEMENT_CASES_AUDITED',registered_cases=len(rows),callbacks=callbacks,disabled_switch_controls=12,stream_refinement_cycle_exact_controls=12,short_and_channel_pilots=12,long_timing_cases=40,sensitivity_cases=8,adversarial_cases=4,sensitivity=summary,adversarial=adv,rows=rows,scope='Supplement to1220unique eager-campaign cases. Same source contexts. Refinement and disabled controls must match cycles;other timing outcomes are observed,not forced.'),indent=2));print('READOUT88_CASE_AUDIT_COMPLETE',callbacks,flush=True)
