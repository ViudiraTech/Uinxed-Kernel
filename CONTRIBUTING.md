# Contributing to Uinxed-Kernel

## Contributing Guide

Contributions are welcome! All changes are reviewed and merged via Pull Request; nothing is ever pushed directly to `develop` or `master`. See [Development Model](#development-model) below for the branching and versioning rules.

### Submit an Issue

Encountering a bug? File an issue - we welcome them all. A few guidelines:

1. **Describe the problem in as much detail as possible.** Logs and code snippets go a long way toward understanding what happened.
2. **Just be polite.** A respectful report gets solved smoothly; hostility helps nobody.
3. **No need to be overly formal.** Casual is fine - we are partners in making this project better.
4. **Your native language is welcome.** You may write in any language, but keep in mind that typos can confuse translation tools.

### Ways to Contribute

#### GitHub Pull Request

1. **Fork** the repository on GitHub, then clone your fork:
   ```bash
   git clone https://github.com/<your-username>/Uinxed-Kernel.git
   cd Uinxed-Kernel
   ```
2. **Create a contribution branch** based on the latest `develop`:
   ```bash
   git fetch origin
   git checkout -b feature/<your-change> origin/develop
   ```
3. **Make your changes and commit them.**
4. **Run checks** before submitting: `make check`, then `make format`.
5. **Push the branch to your fork:**
   ```bash
   git push -u origin <your-branch>
   ```
6. **Open a Pull Request** from your branch against `develop`, and describe what you changed and why.
7. After a maintainer reviews and merges it, delete your branch.

#### Email Patch

1. **Clone the repository:**
   ```bash
   git clone https://github.com/ViudiraTech/Uinxed-Kernel.git
   cd Uinxed-Kernel
   ```
2. **Create a contribution branch** based on the latest `develop`:
   ```bash
   git fetch origin
   git checkout -b feature/<your-change> origin/develop
   ```
3. **Make your changes and commit them** (no fork or push needed — just local commits).
4. **Run checks** before submitting: `make check`, then `make format`.
5. **Generate the patches:**
   ```bash
   git format-patch -o patches origin/develop..HEAD
   ```
6. **Email** the generated `.patch` files to the maintainer's address (see Contact in README.md).
7. The maintainer applies them, reviews, and merges; you can delete your branch afterwards.

## Development Model

### Branching

Three kinds of branches are used:

* **`master`** — the mainline branch, reserved for released, versioned snapshots. It advances only when `develop` is merged in via Pull Request and the version number is bumped.
* **`develop`** — the integration branch and the working baseline for all development. This is what your Pull Request targets.
* **Contribution branches** (`feature/*`, `fix/*`, ...) — individual changes live here. Create one from `develop`, and delete it once merged.

In short: create a contribution branch from `develop`, work on it, and open a Pull Request to merge it into `develop`. Changes reach `master` only through a release Pull Request from `develop`.

Create your branch from the latest `develop` and name it according to its purpose:

| Purpose              | Prefix       |
|----------------------|--------------|
| Feature development  | `feature/*`  |
| Bug fixes            | `fix/*`      |
| Code refactoring     | `refactor/*` |
| Code formatting      | `format/*`   |
| Performance tuning   | `perf/*`     |
| Documentation        | `docs/*`     |
| Review branches      | `review/*`   |
| Anything else        | `other/*` (please explain in the commit) |

### Versioning

Uinxed follows semantic versioning in the format `MAJOR.MINOR.PATCH[-alpha.N|-beta.N|-rc.N]`. Incrementing a higher component resets all lower components to zero (carry-like behavior): `MAJOR++` resets both `MINOR` and `PATCH` to 0; `MINOR++` resets `PATCH` to 0.

| Component | Meaning | Bump when |
|-----------|---------|-----------|
| `MAJOR` | Breaking changes, major updates, or historic releases | Syscall ABI break, architectural refactoring, data structure layout change, dropped platform support, landmark feature releases, paradigm-shifting rewrites |
| `MINOR` | New features or significant improvements | New filesystem, new driver subsystem, new syscalls, scheduler rewrite |
| `PATCH` | Bug fixes, performance optimizations, or security patches | Driver fixes, memory corruption fixes, security patches, performance tuning |

Pre-release versions carry one of three suffixes; no suffix means a stable release:

| Suffix | Phase | Character |
|--------|-------|-----------|
| `-alpha.N` | Initial development | Features unstable, updates aggressive, APIs may change freely |
| `-beta.N` | Stabilization | Bug fixes, refinements, performance tuning; feature set frozen |
| `-rc.N` | Release candidate | Final polish before release; only critical fixes accepted |
| (none) | Final release | Stable, tagged, and published |

Release rules: alpha/beta/rc receive no tag and no GitHub Release (they may exist on `master` as regular commits); only a final release receives a tag (`vMAJOR.MINOR.PATCH`) and a GitHub Release, kept forever.

### Commit Message Guidelines

There is no strict commit message format, but please:

* Describe clearly in the message what you did.
* Always end the message with a `Signed-off-by` trailer.
