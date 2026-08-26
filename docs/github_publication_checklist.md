# GitHub publication readiness checklist

This repository remains private. Apache License 2.0 has been selected and recorded in `LICENSE`,
with `Copyright 2026 Haoxiang Sang` in `NOTICE`; no visibility change was made.

- [x] Tracked-file scan excludes `.itch`, Parquet, packet capture, model pickle and credentials.
- [x] Build trees, artifacts, virtual environments, coverage files and generated large reports are
  ignored.
- [x] Documentation uses repository-relative links and contains no user-specific absolute path.
- [x] Simulator manifests omit input absolute paths and system time.
- [x] No broker/exchange connection, order entry, live feed, credential handling or network service
  was introduced.
- [x] Synthetic fixtures contain no licensed market data.
- [x] Apache License 2.0 and the project copyright notice are present and registered in package
  metadata.
- [ ] User explicitly approves a visibility change after reviewing full Git history.
- [ ] A final history-wide secret and large-object scan is run immediately before publication.
- [ ] Any real dataset remains outside Git and is reviewed against provider license terms.

Passing this checklist does not authorize publication, packaging, deployment or live trading.
