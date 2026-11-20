# Repository Guidelines

## Project Structure & Module Organization

gem5 is a simulator repository with mixed C++, Python, SCons, and test assets.
Core simulator code lives in `src/`; public headers are in `include/`.
Configuration scripts and examples are in `configs/`. Build-system support is
split between `SConstruct`, `site_scons/`, `build_tools/`, and `build_opts/`.
System-level, PyUnit, and TestLib assets live under `tests/`; C++ unit tests
are usually colocated with their source under `src/`. Utility scripts are in
`util/`, and vendored third-party or integration code is under `ext/`.

## Dependency Policy

The project policy for dependency-support updates is to cover Ubuntu LTS
releases still in standard support. The oldest supported LTS sets ordinary
dependency minimums, while dependencies from the newest LTS must also work.
Non-compiler floors should normally be written as `major.minor+`; compiler
policy uses GCC and Clang major-version ranges.

Policy intent is not proof of current coverage. Before claiming a release,
compiler, or dependency is supported, verify the checks in `SConstruct`, the
workflow matrices, Docker images and bake targets, and user documentation.

## Build, Test, and Development Commands

- `scons build/ALL/gem5.opt -j <jobs>`: build the optimized gem5 binary with
  all ISA targets.
- `scons build/X86/gem5.opt`: build with the `build_opts/X86` configuration;
  replace `X86` with another configuration name from `build_opts/`.
- `scons build/ALL/unittests.opt`: build and run C++ GoogleTest unit tests.
- `cd tests && ./main.py run -j <jobs>`: run the default quick TestLib tests.
- `cd tests && ../build/ALL/gem5.opt run_pyunit.py`: run Python unit tests
  after building `build/ALL/gem5.opt`.
- `cd tests && ./main.py run --length=long`: run longer TestLib suites; use
  `--length=very-long` only for broad validation.

## Coding Style & Naming Conventions

Use 4 spaces, no tabs, no trailing whitespace, and a 79-column line limit.
C++ style is enforced by `.clang-format` and gem5 hooks; use upper camel case
for classes, lower camel case for functions and members, snake case for local
variables and parameters, and all-caps macros. Python is formatted with Black
and imports are managed by isort using the settings in `pyproject.toml`.
Install hooks with:

```sh
pip install pre-commit
pre-commit install
```

Keep the hooks installed and active so commit-time errors can be fixed locally
before pushing PR updates to GitHub.

## Testing Guidelines

gem5 has three major local test layers. C++ GTests are usually `*.test.cc`
files under `src/` and are built and run through SCons targets such as
`build/ALL/unittests.opt` or matching `*.test.opt` binaries. Python unittest
files live under `tests/pyunit` and run through `tests/run_pyunit.py` from the
`tests` directory. `tests/gem5/pyunit/test_run.py` registers them as a quick
TestLib suite. Broader regression coverage uses TestLib, whose framework is in
`ext/testlib` and whose suites are mostly under `tests/gem5`.

TestLib suites are tagged by duration. `quick` is the default and is expected
for most pull requests. `long` covers heavier daily-style validation and runs
as part of the `daily-tests.yaml` workflow.
`very-long` is for tests that may take days and should be reserved for
release-level or high-risk changes. Use `./main.py list -q --suites` to find
suites and `./main.py run --uid <SuiteUID> --skip-build` for focused reruns.
Only use `--skip-build` when the required binaries already exist.

## CI Workflows

GitHub Actions live in `.github/workflows`. `ci-tests.yaml` runs pull request
checks, including pre-commit, clang-format, unit tests, builds, and quick
TestLib execution. `daily-tests.yaml` runs long TestLib coverage, extra daily
tests, unittests, and cache-warming builds. `weekly-tests.yaml` runs quick,
long, and very-long TestLib suites plus coverage upload. `compiler-tests.yaml`
validates supported GCC/Clang and dependency-image build configurations.

## Validation Tiers

Match validation to the edit. For docs-only changes, run `git diff --check`.
For Python or SCons files, `python3 -m py_compile <files>` checks syntax only.
`scons -Q --help` evaluates the top-level `SConstruct` and imported site setup,
but without a target it does not read build-specific `SConsopts` or
`SConscript` files, run configure probes, or validate source registration.
Use a narrow explicit SCons target when those behaviors may be affected. For
C++ edits, prefer targeted object or unit-test builds before broad binaries.
For TestLib changes, start with `./main.py list ... -q` to confirm selection,
then run the narrowest relevant suite; use `--skip-build` only when all
required binaries already exist.

## Commit & Pull Request Guidelines

Develop changes on branches based on `develop`, not `stable`. Commit subjects
use gem5 component tags from `MAINTAINERS.yaml`, for example
`tests,base: Add coverage for bit helpers`; keep the subject under 65
characters and body lines under 72 characters. Preserve blank lines between
body paragraphs and backtick literal code identifiers, commands, and paths.
Prefer small, focused commits and include relevant GitHub issue links. Pull
requests target `gem5/gem5:develop`, should explain the change and validation
performed, and must pass CI and review before merge.

Before broad or cross-subsystem edits, inspect `MAINTAINERS.yaml` for component
tags, maintainers, experts, and orphaned status. Use those tags in commit
subjects and use the ownership information to frame PRs and likely reviewers.
Follow the user's explicit instructions about committing and pushing; never
rewrite shared history or disrupt another contributor's work unless explicitly
directed.

New source files should carry an appropriate copyright notice and license
header. Follow the 3-Clause BSD license in `LICENSE` unless the contributor's
copyright holder or institutional requirements call for another repository-
accepted header; do not infer a copyright holder from nearby files.

## Release Process

The documented gem5 release process targets three releases per year.
Maintainers announce the release window, create `release-staging-{VERSION}`
from `develop`, run the full test suite, then merge the staging branch to
`stable` when ready. The
stable branch is tagged as `v{YY}.{MAJOR}.{MINOR}.{HOTFIX}`. During staging,
target normal work at `develop`; submit to staging only for release-critical
fixes. Hotfixes branch from `stable`, require normal review, merge back to both
`stable` and `develop`, and receive an incremented hotfix tag.

## Agent-Specific Notes

Preserve user constraints. If asked not to compile or run simulations, stay
with source, logs, artifacts, and metadata. For CI failures, inspect the actual
failing job logs before assigning cause; SCons configure failures often require
reading `build/ALL/gem5.build/scons_config.log`, not just the headline error.

Every new source file should use the copyright holder required by the
contributor's institution or other obligation. Avoid editing `gem5/ext/` for
policy or dependency updates; prefer build logic, Dockerfiles, workflows, and
documentation.

For dependency or compiler-support maintenance, keep `SConstruct`,
`.github/workflows`, `util/dockerfiles`, and docs aligned with the Ubuntu LTS
policy above.

Docker images provide standardized environments for many gem5 tests. Exact
historical reproduction also requires the workflow revision and the image
digest used by the failing job because a `:latest` tag can change. Ubuntu
all-dependency images are the baseline for many CI jobs. To reproduce a
failure, inspect the relevant workflow and use its image where practical. GPU
full-system tests use the all-dependencies image and the `ALL` build alongside
the other TestLib suites.

A GPU container alone does not select GPU TestLib suites. Use `--isa=ALL` with
the appropriate length and always confirm selection with
`./main.py list ... -q` before an expensive run.

Do not edit generated build outputs under `build/`. When generated files look
wrong, change the source generator, SCons rule, SLICC input, or SimObject
definition that produces them.

Common investigation paths: for SCons configure failures, read
`build/ALL/gem5.build/scons_config.log`; for GitHub Actions, inspect the
specific failing job rather than only the workflow conclusion; for long test
failures, use harness metadata such as `results.xml` before estimating rerun
cost from suite wall time.

Useful research sources, in rough order, are the current codebase and commit
history; GitHub PRs, issues, and Discussions; public archives for
`gem5-dev@gem5.org` and `gem5-users@gem5.org`; the gem5 website and its linked
resources; and comments or documentation embedded near the relevant source
code.

Concrete sources:
`https://github.com/gem5/gem5`,
`https://github.com/gem5/gem5/pulls`,
`https://github.com/gem5/gem5/issues`,
`https://github.com/orgs/gem5/discussions`,
`https://www.mail-archive.com/gem5-dev%40gem5.org/`,
`https://www.mail-archive.com/gem5-users%40gem5.org/`, and
`https://www.gem5.org/`.

## PCIe 6.0 switch project

Source precedence for this project is:

1. `docs/pcie_switch/PCI-Express-Base-Specification-Revision-6.0.pdf`:
   normative protocol behavior.
2. `docs/pcie_switch/PCIe6_Scaleup_Lab_Offline.html`: simulation features,
   inputs, sizing, outputs, and reference behavior.
3. `docs/pcie_switch/pcie_sw_databook.pdf`: supplementary controller detail.

When HTML simplifies the protocol, label the approximation and retain a
reference mode; never let it override the Base Specification. Databook
interface replication is optional and must not expand initial lab scope.
Retain competitor-driven specs as supplementary requirements (requirements
section 11 and Appendix A), with sources, measurement boundaries, and
explicit target profiles. They do not override protocol rules or lab defaults.

Read these design documents before implementing the switch:

1. `docs/pcie_switch/PCIe6_Switch_Requirements.md`: requirements and IDs.
2. `docs/pcie_switch/PCIe6_Switch_Architecture.md`: hardware decomposition.
3. `docs/pcie_switch/PCIe6_Simulation_Architecture.md`: gem5 implementation
   contracts, fidelity levels, milestones, and acceptance criteria.
4. `docs/pcie_switch/PCIe6_Implementation_Plan.md`: phased tasks,
   dependencies, exit criteria, and validation evidence.
5. `docs/pcie_switch/PCIe6_Validation_Scenarios.md`: scenario inputs,
   independent expected results, and milestone exit checks.

This is a proposed gem5 timing model, not an existing PCIe 6.0 implementation.
Use gem5's event queue and existing PCI configuration infrastructure. Keep
protocol transaction identity separate from gem5 packets and workload IDs.
Do not implement PCIe by merely changing a generic bridge's bandwidth.

Start with standalone synthetic endpoints, one transparent switch, and
explicit Read/Write semantics. SimAI integration, full-system enumeration,
NTB, multicast, INC, and other protocols are staged extensions. Unsupported
features must be rejected or explicitly marked as abstracted in results.

Keep finite queues, resource ownership, retry behavior, ordering, and
completion semantics explicit in `pcie6_timing`. The `lab_reference` mode
retains HTML approximations, including timed credit recovery and queue peak
comparison; it is not protocol evidence. No silent congestion drops,
duplicate completion, or second independent event clock in the timing model.
Gen6 shared flow control must be covered before claiming detailed FLIT
fidelity; a dedicated-credit approximation must be labeled accordingly.

### SimAI integration

After M3, complete M4A: build the actual gem5 topology from configuration,
run standalone workloads, and round-trip configuration and results with the
offline HTML. Require M4A acceptance before implementing or integrating
SimAI through AstraNetworkAPI in M4B. Read the SimAI-side design at
`/workspaces/SimAI/docs/pcie_switch/PCIe6_Simulation_Architecture.md` and audit
current APIs before implementing. Reuse the same gem5 endpoint/link/switch
runtime for microbenchmarks and workloads. All ranks share one event queue;
message matching, send/receive completion, bounded injection, and DAG feedback
need explicit tests. Do not duplicate the PCIe core or inherit NS3/NVLink
latency/PFC behavior.

### New PCIe source attribution

The user specifies `Copyright (c) 2026 cary` (no company). New PCIe code
and tests inherit the existing project license described by `COPYING` and
`LICENSE`; use the root `LICENSE` header with this copyright holder. Do not
add a separate license file, relicense existing source, or copy another
institution's copyright holder. This supersedes the earlier MIT request.

### Stage completion and delegation

For this PCIe project, implement one reviewable stage at a time, with
M4A and M4B accepted and committed separately.
After its exit criteria pass, update the stage status and validation evidence,
then commit the stage's code, tests, and documentation together. The user
has authorized these stage commits; do not push without instruction. Do not
mark a stage complete merely because a subset or its documents are done.
Preserve unrelated user changes and inspect the staged diff before commit.

The user requests low-cost subagents for bounded work. Prefer a low-cost
model for independent read-only reviews, test-matrix audits, or narrow tasks
with clear inputs and outputs. Explain each delegation briefly. Keep core
integration and final verification with the primary agent, review every
result, and avoid full-history forks when a self-contained task suffices.
Do not delegate shared-file edits without explicit file ownership.

### Test-driven development

TDD is the development method, not a replacement architecture or roadmap.
Preserve the gem5 switch implementation design and M0-M5 feature milestones.
For each task, define executable scenarios, independent expected results,
and exit criteria before implementing the feature; record semantic Red,
minimal Green, and regression evidence. Do not impose a functional-to-
SystemC-to-RTL lowering pipeline on this simulator project. The validation
scenario document supplements the implementation plan, not vice versa.

### Document ownership

Requirements define intent; hardware architecture defines modeled resources;
simulation architecture defines implementation and verification in gem5.
Keep requirement IDs stable. Write design prose in Chinese, matching the
existing documents. Preserve the offline HTML as a reference estimator unless
asked to change it. Do not treat browser estimates as calibration evidence.

### Evidence and scope

All architecture modules and implementation paths are proposals until source
and tests exist. Mark numerical values as standard facts, design assumptions,
targets, calculated budgets, or measured results. Record measurement boundaries.
144 lanes, three slices, 3 MiB SRAM, and sub-100 ns core latency are exploration
points or targets; they are not validated implementation specifications.

Use the supplied PCI Express Base Specification Revision 6.0 for protocol
rules, recording section and printed page. Check relevant errata/ECNs before
claiming compliance. The supplied DWC switch databook is version 5.71a,
May 2020, at `docs/pcie_switch/pcie_sw_databook.pdf`. Use its controller/
application contracts (RADM, XADM, CDM, TRGT1, XALI, DBI, Appendix P) as
structural references, not as Gen6 protocol authority. Keep controller RX
queues/replay separate from application SRAM/staging. Requirements F-23
through F-32 are supplementary refinements; pin/beat fidelity is optional.
Follow the lab requirements first for the simulation feature scope.
Do not copy proprietary databook text or diagrams into source or public docs.

Keep PCIe FLITs, internal fabric cells, SRAM sectors, and application chunks
separate. Distinguish dedicated/shared external credits, internal resource
reservations, and TX replay storage. Do not substitute Ethernet PFC/RDMA or
CXL semantics for PCIe. Mesh/Clos connectivity alone does not establish a
legal transparent PCIe hierarchy.

### Review checklist

- State timing-port ownership, request/response retry, and posted-write
  adaptation explicitly.
- State each buffer's allocation, ownership transfer, and release event.
- Specify Read segmentation, Completion assembly, Tag lifetime, ordering,
  and forward-progress assumptions.
- Separate L1 approximations from L2 FLIT protocol coverage.
- Keep proposed file names and commands distinct from existing assets.
- Pair milestones with observable acceptance criteria and requirement IDs.
- Run `git diff --check`; documentation edits do not require a simulator build.
