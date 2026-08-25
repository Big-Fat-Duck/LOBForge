# Round 3 leakage audit

| Risk | Enforced control | Executable evidence |
|---|---|---|
| Future messages alter historical features | Feature state accepts only `sequence <= anchor_sequence`; canonical prefix rows are compared after future-tail replacement | `test_prefix_invariance_and_future_tail_perturbation` |
| Future labels leak into features | Feature logical digest excludes all `target_*` fields and is unchanged by deterministic label shuffle | `test_positive_null_and_shuffle_controls_meet_registered_oracles` |
| Wrong clock join uses first event after horizon | Grid sampler waits for a timestamp group to finish and selects the last state `<= grid`; same-timestamp order is sequence-stable | `test_clock_sampling_is_right_continuous_and_uses_last_same_timestamp` |
| Window/target overlap | Primary OFI is `(t-100ms,t]`; target is `(t,t+100ms]` | frozen protocol plus label tests |
| Label crosses invalid book/halt/end | Invalid states flush pending event and clock queues with null targets | `test_event_labels_do_not_cross_invalid_boundary` |
| Same date leaks between partitions | Split membership is keyed only by globally sorted complete date | `test_global_date_split_purge_and_symbol_cohesion` |
| Horizon overlap at split | Absolute-time purge is 1 second, the configured maximum | split test and manifest purge counts |
| Train statistics learn validation/test | Every fit API requires literal partition `train`; second fit is rejected; transform is immutable | model, split and micro-price tests |
| Calibration uses test | Base classifier fits train core and calibrator fits chronological train tail | classifier parameter artifact/tests |
| Target column enters matrix | Case-insensitive denylist rejects `target`, `future`, `next`, `lead` | `test_feature_denylist_symbol_selection_and_fit_scope` |
| High-message symbols dominate | Per-symbol-day metrics are calculated first, then equally weighted | `test_symbol_day_equal_weight_bootstrap_bh_and_deciles` |
| Adjacent observations treated IID | Bootstrap samples whole symbol-day blocks | deterministic block-bootstrap test |
| Hyperparameter/test resampling | Protocol permits validation selection and one final-test evaluation | protocol hash in final artifacts |
| Learned micro-price updates online | State/bin/matrices serialize after train and are byte-logically unchanged by test transform | `test_microprice_train_only_serialization_and_transform_is_frozen` |
| Batch/file metadata changes semantics | Column-major typed Arrow logical digest is independent of row batch and Parquet metadata | ten-run alternating-batch ingest test |

The fast Arrow validator and independent scalar validator both validate the golden stream. The
scalar path additionally checks JSON key order line by line; C++ byte-exact golden testing fixes the
wire order. A malformed, truncated, wrong-version, backward-sequence or out-of-range stream causes
a stable nonzero exit and no final output directory.
