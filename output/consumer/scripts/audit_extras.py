from consumer_stats import *
rows=[];callbacks=0;build=read(C/'FINAL_BUILD.json')
for family,manifest in [('disabled_native','DISABLED_TASKS.json'),('ablation_native','ABLATION_TASKS.json'),('sensitivity_native','SENSITIVITY_TASKS.json')]:
 ts=read(C/manifest)
 for t in ts:
  p=record(family,t);a,b=verify(t,p);callbacks+=a['result']['callbacks'];assert a['identities']['library']==build['library_sha256']
  rows.append(dict(family=family,name=t['name'],fixture=t['fixture'],arm=t['consumer_arm'],baseline=b['native_cycles']-1,candidate=a['native_cycles']-1,ratio=(b['native_cycles']-1)/(a['native_cycles']-1),baseline_q0=q0(b),candidate_q0=q0(a),source_block='context0',all_six_arrays_exact=True,cycles_equal=a['native_cycles']==b['native_cycles']))
summary={arm:stats([r for r in rows if r['arm']==arm],False)for arm in sorted({r['arm']for r in rows})}
adversarial=[]
for t in read(C/'ADVERSARIAL_TASKS.json'):
 p=record('adversarial_native',t);a,b=verify(t,p);z=a['result'];adversarial.append(dict(fixture=t['fixture'],all_six_arrays_exact=True,invalidations=z['qpipe_invalidations'],speculative_exp_calls=z['qpipe_spec_exp_calls'],reused_exp_calls=z['qpipe_valid_exp_calls'],recomputed_exp_calls=z['qpipe_recomputed_exp_calls'],zero_port_collisions=a['pipeline_audit']['physical_readout_epu_overlap_cycles']==0))
assert len(adversarial)==4
for r in adversarial:
 if r['fixture'].endswith('max_migration'):assert r['invalidations']>3 and r['speculative_exp_calls']>r['reused_exp_calls']
csvwrite(C/'results/ablation_sensitivity.csv',rows)
(C/'results/EXTRA_AUDIT.json').write_text(json.dumps(dict(status='ALL32_ABLATION_SENSITIVITY_DISABLED_AND4_ADVERSARIAL_CANDIDATES_PASS',registered_extra_runs=len(rows),callbacks=callbacks,summary=summary,adversarial_controls=adversarial,scope='Disabled controls reproduce original cycles exactly;ablation/sensitivity compare realDense operands and samequality. Four native synthetic references are separate,not extra text/model workload samples.'),indent=2));print('EXTRA_AUDIT_COMPLETE',len(rows),len(adversarial),flush=True)
