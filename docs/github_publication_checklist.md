# GitHub publication readiness checklist

This repository remains private and proprietary. No license or visibility change was made. Before
any future publication, the user must explicitly choose a license and repository visibility.

- [x] Tracked-file scan excludes `.itch`, Parquet, packet capture, model pickle and credentials.
- [x] Build trees, artifacts, virtual environments, coverage files and generated large reports are
  ignored.
- [x] Documentation uses repository-relative links and contains no user-specific absolute path.
- [x] Simulator manifests omit input absolute paths and system time.
- [x] No broker/exchange connection, order entry, live feed, credential handling or network service
  was introduced.
- [x] Synthetic fixtures contain no licensed market data.
- [ ] User selects an open-source or source-available license, if publication is intended.
- [ ] User explicitly approves a visibility change after reviewing full Git history.
- [ ] A final history-wide secret and large-object scan is run immediately before publication.
- [ ] Any real dataset remains outside Git and is reviewed against provider license terms.

Passing this checklist does not authorize publication, packaging, deployment or live trading.
