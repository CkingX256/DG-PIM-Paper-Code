# Analysis utilities

`evaluate_dispatch_and_l2.py` generates parameterized policy, remapping, sensitivity and L2 projection results from fixed inputs in the script. It does not invoke the native simulator or collect new real-model measurements. `verify_metrics.py` performs consistency checks on the supplied summary tables.

```bash
python3 analysis/scripts/verify_metrics.py
python3 analysis/scripts/evaluate_dispatch_and_l2.py
```

The generated JSON is written under `analysis/data/`. NumPy is required for the evaluation command. Neither command replaces the omitted native traces, model captures or hardware validation.
