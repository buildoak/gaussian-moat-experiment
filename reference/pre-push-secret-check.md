# Pre-Push Secret Check

Run this before pushing or publishing rewritten history.

## Current Tree Scan

```bash
git status --short --branch
git grep -n -I -E '(AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|gh[pousr]_[0-9A-Za-z_]{36,}|xox[baprs]-[0-9A-Za-z-]{10,}|sk-[A-Za-z0-9_-]{20,}|BEGIN (RSA |OPENSSH |EC |DSA |)PRIVATE KEY|api[_-]?key|secret|password|token)' -- . \
  ':(exclude)reference/pre-push-secret-check.md'
```

Expected result: no credential-like values. Words such as `token`, `secret`,
or `api key` in documentation are allowed only when they do not include a live
value.

## History Scan

Preferred if installed:

```bash
gitleaks detect --source . --no-banner
```

Fallback:

```bash
git log --all -G 'AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|gh[pousr]_[0-9A-Za-z_]{36,}|xox[baprs]-[0-9A-Za-z-]{10,}|sk-[A-Za-z0-9_-]{20,}|BEGIN (RSA |OPENSSH |EC |DSA |)PRIVATE KEY' --oneline
```

If anything credible appears, stop. Do not push. Rotate the credential and
rewrite history only after the rotation decision is made.
