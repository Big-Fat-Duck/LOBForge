# Round 4 dependency and license audit

Round 4 added no C++ runtime dependency and no Python runtime or development dependency. The C++
core remained standard-library only. At Round 4 validation time the local Python package was
`0.4.0`; the then-current `uv.lock` SHA-256 was
`318407ecb7c85f27d16ebeb86f99625cd4839d8dc4476190e46e2b32fb70bb04`. Those values are retained as
historical Round 4 evidence, not as the current release-candidate state.

Round 4.5 advances the package metadata to `0.4.0rc1` and locks the build backend as
`hatchling==1.27.0` through uv build constraints. The current `uv.lock` SHA-256 is
`b163ee35668fa86c82167df8a689e4475cfcbb66c0af56852d5c8b6635697059`. The complete current direct,
transitive, build-backend and GitHub Actions inventory is maintained in
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

Existing third-party license findings in the Round 3 audit remain applicable. No external source
code, data file, market fixture, model or paper text was identified as copied into artifacts. Round
4 itself was validated before a project license was selected; the subsequent `v0.4.0-rc1` release
candidate is licensed under Apache License 2.0 with `Copyright 2026 Haoxiang Sang` in the root
`NOTICE`.
