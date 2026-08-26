# Public release runbook

This runbook begins only after `docs/round4_5_release_audit.md` reports
`LOCAL_RELEASE_AUDIT_PASS`. It describes external actions for the repository owner; the Round 4.5
audit does not perform them. Do not paste credentials into commands, configuration or issue text.

## 1. Review the release diff and manifest

Review every planned file and the canonical release scope before staging anything:

```sh
git status --short
git diff --stat
git diff
python tools/release_audit.py --manifest artifacts/round4_5/release_manifest.json \
  --no-write --verify-existing
```

Confirm that the only source-controlled artifact is
`artifacts/round4_5/release_manifest.json`. Do not remove ignored user data merely to make the
workspace look clean.

## 2. Verify the project license

Apache License 2.0 has been selected for LOBForge. Verify that the root `LICENSE` contains the
complete terms, `NOTICE` contains `Copyright 2026 Haoxiang Sang`, and `python/pyproject.toml` uses
the SPDX expression `Apache-2.0`. Confirm that third-party materials remain governed by the terms
listed in `THIRD_PARTY_NOTICES.md`; the project license must not be presented as relicensing them.
Seek qualified legal advice if ownership, employment or third-party rights are uncertain.

## 3. Confirm public Git identity

Inspect the name and email that the release-candidate commit would expose:

```sh
git config user.name
git config user.email
git log -1 --format=fuller
```

Prefer a verified public identity or an appropriate GitHub `noreply` address. Do not put a private
email in repository files or this runbook.

## 4. Create or confirm a private GitHub repository

Use GitHub's UI or an authenticated tool to confirm a private repository owned by the intended
account/organization. Verify collaborator access and branch protection. Do not reuse an untrusted
remote or embed a token in its URL. This runbook intentionally contains no repository address.

## 5. Commit and push the release candidate privately

After the diff, license and identity reviews, create one auditable release-candidate commit and push
the intended branch to the private remote. Create and push the annotated pre-release tag
`v0.4.0-rc1` only after its target commit is verified; never use the final `v0.4.0` tag for RC
metadata. Record the commit and tag target SHAs in the release audit follow-up.

## 6. Require hosted CI to pass

Wait for GCC Release, Clang Release, Python, sanitizer, fuzz/smoke, format and release-audit jobs to
finish successfully. Local validation is `LOCAL_CI_VALIDATED`; only the actual hosted run may be
called `GITHUB_ACTIONS_GREEN`. Investigate cancellations and skipped jobs rather than treating them
as success.

## 7. Review GitHub Security results

In the repository Security settings/pages, inspect Secret Scanning, Dependabot and Code Scanning
results where the account plan supports them. Enable recommended protections before publication.
Never copy a detected secret value into a ticket, log or chat.

## 8. Resolve every high-risk finding

For a real credential, revoke/rotate it with the provider and determine whether Git history needs
coordinated cleanup. For vulnerable dependencies or code findings, apply a reviewed fix and rerun
the local and hosted gates. Recreate the candidate commit if its content changes.

## 9. Verify GitHub rendering

On the private GitHub page, open README, Mermaid architecture, tables, relative links and any
images. Check filename case using the web UI. Do not add badges until their repository URL and
workflow status are confirmed.

## 10. Change visibility only after a final review

Reconfirm collaborator access, Actions logs/artifacts, Git history, license, manifest, ignored data
and Security findings. Then the owner may change the repository from private to public. This is a
separate authorization step and is never implied by a local audit pass.

## 11. Create the formal release

The current candidate is `v0.4.0-rc1`; do not put the final `v0.4.0` tag on RC metadata. First
promote the CMake/Python/package/report state from `0.4.0rc1` to `0.4.0`, regenerate `uv.lock` and
the release manifest, rerun every local and hosted gate, and create a new reviewed final commit.
After public rendering and those hosted checks are verified, create the annotated `v0.4.0` tag
from that final commit and a GitHub Release summarizing engineering scope, synthetic evidence and
the real-data/profitability blockers. If the metadata has not been promoted, only an explicitly
pre-release `v0.4.0-rc1` tag is valid. Do not attach datasets, local artifacts or packages unless a
separate distribution audit has approved them.

## 12. Perform the post-publication check

Immediately re-scan repository files, commit history, hosted Actions logs and Release attachments.
Confirm that no private information, credential, local path or restricted dataset is visible. Record
the public commit/tag and hosted workflow URLs in a post-release note, without recording tokens.

## Rollback and escalation

If exposure is found after publication, make the repository private while coordinating remediation;
that does not revoke a leaked credential or erase existing clones. Rotate real credentials through
their providers and obtain owner/legal approval before destructive history rewriting. Preserve a
redacted incident record and rerun the full release audit before republishing.
