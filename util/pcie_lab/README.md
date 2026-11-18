# PCIe lab configuration and sizing

This directory implements the M0 host-side contract and the M1 analytical
sizing tasks. New files inherit the project license in root
`COPYING`/`LICENSE`; copyright holder: cary.

From the gem5 repository root:

```sh
python3 -m unittest discover -s tests/pcie_switch -p 'test_*.py'
python3 -m util.pcie_lab.cli util/pcie_lab/examples/reference.json --root .
python3 -m util.pcie_lab.cli util/pcie_lab/examples/reference.json --root . --size
```

The CLI emits a versioned `not_run` result with the experiment snapshot, source
and implementation hashes, unresolved automatic values, and Git provenance.
Invalid input produces a structured diagnostic on stderr and exit status 2.
The CLI has no simulation-success status. All Python runtime dependencies are
standard-library modules; auditing HTML in the tests additionally requires
Node.js. No gem5 binary, compiler, GPU, or network access is required.

For `lab_reference`, `--size` adds `reference_sizing` and retains the
`not_run` status. This field contains analytical results in the HTML units,
including resolved resources, sorted ceilings, and warnings. A `null`
ceiling denotes a non-limiting resource for that workload. The input
snapshot retains automatic zero values for reproducibility.

The CLI also accepts a `pcie6-hardware-calibration-v1` file. It compiles a
reference experiment from `inputs` and the seed when present, then stores
the complete input document under `imported_calibration`, including any
browser simulation data.
Top-level workspaces produce a separate diagnostic.

```sh
python3 -m util.pcie_lab.cli util/pcie_lab/examples/reference.json --root . \
    --export-calibration > /tmp/pcie-calibration.json
python3 -m util.pcie_lab.cli /tmp/pcie-calibration.json --root . --size
```

Exporting an experiment projects its sizing into the page's calibration
fields and adds versioned provenance with `sim: null`. Exporting an
imported calibration preserves its original JSON values and extensions.

Run the reference event model with a gem5 binary from this checkout:

```sh
build/NULL/gem5.fast -d /tmp/pcie-m1 configs/example/pcie/reference.py \
    util/pcie_lab/examples/reference.json \
    --calibration-output /tmp/pcie-m1/calibration.json
```

The default result path is `reference.json` in the gem5 output directory.
`reference_sim` contains the browser-compatible counters and measurements.
The reference dispatcher retains the browser's fractional deadlines and
heap ordering, using a gem5 event at the next picosecond for each callback.
The output records the native last tick separately from the reference
measurement window. Queue peaks are compared with sizing limits after the
run. The reference profile uses timed credit return and replay-window
throttling. These behaviors approximate the offline lab.

## Artifacts and boundaries

- `hardware_fields.json`: all 63 HTML fields with labels, defaults, bounds,
  enum values, steps, groups, and display conditions. Hidden fields persist.
- `contracts.py`: executable experiment/result schema, override precedence,
  input units, reference Tag budgets, protocol declarations, and structural
  topology/resource validation. Decimal step checks deliberately reject
  off-grid values instead of applying the browser's floating tolerance.
- `capabilities.json`: normative rule references, HTML approximations,
  unsupported execution, milestones, and scenario IDs. Entries do not claim
  the corresponding protocol behavior is implemented.
- `source_lock.json`: SHA-256 fingerprints for the HTML and Base Specification.
  A changed source requires an explicit audit and lock/manifest update. Licensed
  PDF contents are neither copied nor interpreted by this code.
- `audit.py`: extracts only the trusted HTML's browser-free sizing module in
  an isolated Node VM with timeouts, to compare every manifest property.
  This is a local oracle helper, not a security sandbox for arbitrary HTML.

Inputs are merged as HTML defaults, profile overrides, then user overrides;
all overrides and raw/effective inputs remain in the experiment. `0=auto`
values are resolved in the optional sizing output. Zero rate limits mean
unlimited injection rate *input* and do not request automatic sizing.

`protocol.rcb_role` identifies the System Element receiving the modeled Read
Completions. Strict input checks permit RCB 64/128 for a Root Complex and 128
for an endpoint. This is a configuration constraint, not Completion execution.
`shared_fc_tx` declares FLIT transmitter capability and must be true in the
strict profile even when service is approximated at TLP granularity; it does
not claim that M0 implements the Shared FC gate or that RX must enable it.

The M0 topology subset is a single-root transparent tree whose root and
endpoints are leaves and whose root-to-endpoint paths all cross the configured
`switches` count. That count is **not** the total number of Switch nodes.
Switch degree is checked against the port budget. Per-link port/lane wiring,
address windows, mixed path lengths, physical deployment and complete resource
backing remain explicitly unvalidated. `resources.forwarding_slices` is distinct
from HTML `inputs.slices`, which remains parallel Fabric channel count.

The three mandatory strict resource fields are positive integer RX header
slots, RX data bytes, and core descriptor slots. They are declarations, not
proof that core payload/VOQ/Fabric/staging/replay capacity is sufficient; those
checks belong to the corresponding sizing and timing milestones.

See `docs/pcie_switch/validation/M0.md` for stage evidence and limitations.
