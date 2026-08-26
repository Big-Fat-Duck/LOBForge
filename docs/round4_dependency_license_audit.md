# Round 4 dependency and license audit

Round 4 adds no C++ runtime dependency and no Python dependency. The C++ core remains standard
library only. The locked Python graph remains the Round 3 set: NumPy, PyArrow, SciPy,
scikit-learn and Matplotlib at runtime; pytest, Hypothesis, pytest-cov, Ruff and mypy for
development. `uv.lock` remains the complete direct/transitive lock; only the local package version
advanced to 0.4.0. Its final SHA-256 is
`318407ecb7c85f27d16ebeb86f99625cd4839d8dc4476190e46e2b32fb70bb04`.

Existing third-party license findings in the Round 3 audit remain applicable. New Round 4 code is
private/proprietary repository code. No external source code, data file, market fixture, model or
paper text was copied into artifacts. No repository license was added because that choice belongs
to the user.
