# Security policy

## Supported versions

Only the latest commit on `main` receives security fixes.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub: open the repository's **Security** tab and choose **Report a vulnerability**. Don't open a public issue.

Include the affected version or commit, steps to reproduce, and the impact you expect. You can expect an acknowledgement within a week.

## Deployment notes

CodeScope has no authentication. Anyone who can reach the server can read all indexed source code.

- Keep it bound to `127.0.0.1` (the default), or put it behind a proxy that adds authentication.
- Requests must be addressed to `localhost`, `127.0.0.1` or `[::1]`, which blocks DNS rebinding. Add other names only with `--allow-host`.
- Don't index directories that contain secrets such as `.env` files or private keys.
