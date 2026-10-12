# Privacy and publishing guide

This project separates publishable source, private observer settings, and local
runtime data. Publishing under your personal GitHub profile connects the project
to that profile. The privacy goal here is to keep your home location, personal
email, credentials, and machine data out of the repository and public visuals.

## Review findings and protections

| Surface | Protection | Remaining exposure |
|---|---|---|
| Observer coordinates, elevation, orientation | Ignored local `.env`; public SFO defaults | Anyone with access to your local settings can read them |
| Git history, branches, stashes, commit metadata | Publish a source-only export into a fresh repository | The original local history remains sensitive |
| Location shown by the app | Normal HUD hides coordinates; `--demo` uses only public settings | Local scenes and settings still identify the location |
| Terrain, street and building caches | Per-user cache directory outside source; restrictive POSIX permissions | Caches contain coordinates and recognizable geometry |
| Logs and error messages | Config values and API response bodies withheld; logs ignored | OS crash reports and screenshots need manual review |
| Downloaded release binaries | Private settings loaded at runtime rather than embedded | Debug symbols/build paths may expose a builder's machine; prefer source builds |
| GitHub commit email | Configure a GitHub no-reply address before committing | Author name and GitHub profile remain associated with the project |
| External API requests | HTTPS; no credentials needed by the configured endpoints | Providers receive your IP and query location; this is not network anonymity |

## What lives where

- **GitHub:** reviewed source, documentation, tests, build instructions, public
  `.env.example`, and privacy/export scripts.
- **Each computer:** ignored `.env`, local environment overrides, build output,
  private caches, local agent settings, and any recordings/logs.
- **Separate private backup:** personal configuration and any original history
  you want to retain. Re-cloning GitHub cannot restore these.

The local export includes a conservative allowlist of source/documentation file
types. Images and archives are excluded; add them only after reviewing visual
content and embedded metadata. Never use `git add -f` to include local settings.

## First publication

Run from the original project root:

```sh
python3 scripts/check_privacy.py
python3 scripts/prepare_publish.py --output ../bedroom-sky-public
```

The output directory must not already exist. It does not copy `.git`, so old
commits, stashes, tags and branches cannot accidentally accompany this export.
Do not copy the original `.git` folder back into it.

In the exported directory, initialize fresh history and configure an identity:

```sh
cd ../bedroom-sky-public
git init -b main
git config user.name "YOUR_PUBLIC_DISPLAY_NAME"
git config user.email "YOUR_GITHUB_NOREPLY_ADDRESS"
git add .
python3 scripts/check_privacy.py --staged
git diff --cached --stat
git commit -m "Initial source release"
python3 scripts/check_privacy.py --staged --history
```

Replace both identity placeholders before committing. Copy your actual no-reply
address from [GitHub email settings](https://github.com/settings/emails); do not
guess it. GitHub documents [commit-email configuration](https://docs.github.com/en/account-and-profile/how-tos/email-preferences/setting-your-commit-email-address)
and [blocking pushes that expose private email](https://docs.github.com/en/account-and-profile/how-tos/email-preferences/blocking-command-line-pushes-that-expose-your-personal-email-address).
Its push-block setting checks the most recent commit, so review all new history
yourself as well. Changing Git's email setting does not change older commits.

Create an empty repository on GitHub, add its URL as `origin`, then push `main`.
This guide and the export tool do not create remotes, commit changes, or push.
Keep the original checkout and private settings until the remote contents have
been verified and a fresh clone has built successfully.

## Running elsewhere

Clone the published repository on another Mac and follow the root README. Start
with `--demo` to confirm dependencies and rendering. Create a new ignored `.env`
from `.env.example`, enter that installation's location, and run `--check-config`
before a normal launch. Use `--config` for private profiles stored elsewhere.

Flight queries and geometry fetches follow the configured observer. Public SFO,
OAK and SJC markers/landing labels remain a regional feature; they do not contain
your home's coordinates. Other airport coverage has not been implemented.
Linux/Windows cache-path handling exists, but those platforms are unverified.

## Sharing screenshots and bug reports

Use `--demo`, verify the HUD says `PUBLIC DEMO (SFO)`, and capture only the demo
window. This mode ignores `.env` and `BEDROOM_SKY_*` overrides and cannot load the
normal local cache. Review the rest of the desktop for usernames, file paths,
notifications, and terminal commands before posting. Do not attach personal
`.env`, binary caches, full local repository bundles, or OS crash reports without
review. Demo mode remains a live networked run, not an offline mock.

## Repeatable checks and their limits

`check_privacy.py` scans reviewed source for common credential formats, personal
paths, email addresses, and coordinates derived from the current local `.env`.
It reports filenames and finding categories without printing matched values.
`--staged` also scans the Git index and rejects files outside the publish list;
`--history` scans all reachable Git objects, including stash/branch/tag content
and commit metadata. These checks intentionally flag the old local repository.

This is a conservative project-specific check, not a complete secret detector.
On a fresh clone without private `.env`, it cannot recognize an unknown personal
coordinate added later. Review diffs for precise locations, addresses, names,
unexpected strings and artifacts. Tests cover parsing, global coordinates,
malformed input, redacted errors, cache isolation, permissions, and export guards.
