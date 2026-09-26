# Contributing

Thanks for your interest in FLAMORIS AI Runtime. 🌱

This repository is currently in the design stage.

## Before contributing

Please read:

- [README.md](README.md)
- [Design phases and review gates](docs/DESIGN_PHASES.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Design acceptance scenarios](docs/DESIGN_ACCEPTANCE.md)
- [Workflow IR](docs/WORKFLOW_IR.md)
- [MCP Contract](docs/MCP_CONTRACT.md)
- [AGENTS.md](AGENTS.md)

Keep proposals aligned with the current repository responsibility boundary.

## Issues

Issues are welcome for:

- architecture questions;
- schema design;
- execution semantics;
- security concerns that are safe to discuss publicly;
- implementation proposals;
- interoperability questions.

Please describe the problem and desired outcome before prescribing a large implementation.

## Pull requests

Pull requests are accepted according to the shared FLAMORIS repository policy.

Keep PRs focused and do not mix unrelated architecture changes.

When implementation begins, include tests for externally visible behavior and validation semantics.

## Design priorities

Prefer:

- small explicit contracts;
- deterministic validation;
- bounded execution;
- provider-neutral capability boundaries;
- headless execution semantics;
- clear authority ownership.

Avoid:

- arbitrary code execution as a convenience feature;
- hidden permissions;
- provider-specific assumptions in the portable workflow IR;
- speculative compatibility layers;
- duplicate state ownership across FLAMORIS services.

## Security-sensitive reports

Do not open a public Issue containing credentials, private infrastructure details, or an exploitable vulnerability with sensitive reproduction data.

See [SECURITY.md](SECURITY.md).

