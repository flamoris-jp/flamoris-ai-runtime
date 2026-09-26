# AGENTS.md

This repository is part of the FLAMORIS ecosystem.

FLAMORIS AI Runtime is currently a **design-stage repository** for a headless execution runtime that will accept AI-authored workflow IR, validate it, and execute it under explicit capability and resource boundaries.

Do not implement behavior based only on prior chat context. Read the current repository documentation first and keep planned behavior clearly separated from implemented behavior.

## Repository identity

### This repository owns

Planned ownership:

- workflow IR and schema versioning;
- workflow validation and compilation;
- bounded execution semantics;
- per-run runtime state, cancellation, limits, and results;
- capability discovery;
- a narrow MCP surface for workflow submission;
- later, safe adaptive graph patching.

### This repository does not own

Do not duplicate authority from neighboring FLAMORIS repositories:

- Agent identity, conversations, memory, personality, and Agent policy belong to `flamoris-ai-agent`;
- raw language/reasoning/coding provider authority belongs to `flamoris-intelligence-mcp`;
- generative-media workflow/job/asset authority belongs to `flamoris-generation-mcp`;
- local GPU/runtime lifecycle transitions belong to `flamoris-gpu-node-manager`;
- Studio and desktop products retain authority for their own state and documents.

The runtime may call another service through an explicit adapter. It must not silently copy or become authoritative for that service's state.

## Core design rule

**AI-authored does not mean AI-trusted.**

Workflow input is untrusted execution input.

Before execution, the runtime must validate at least:

- schema version;
- node types;
- graph structure;
- references;
- capability availability;
- permission requirements;
- resource budgets;
- side-effect policy;
- concurrency and fan-out bounds;
- timeout and cancellation behavior.

Do not add a general arbitrary-code node, unrestricted shell execution, ambient filesystem access, ambient network access, or credential injection as a shortcut.

## Headless-first rule

Execution semantics must not depend on GUI state.

Do not make canvas coordinates, visual grouping, editor tabs, colors, comments, or layout metadata required for execution.

A future visual editor may keep optional presentation metadata, but the portable workflow IR must remain executable without it.

## Provider and service boundaries

Prefer capability names and explicit adapters over provider-specific architecture.

Examples of acceptable conceptual node families:

- `control.*`
- `data.*`
- `intelligence.*`
- `generation.*`
- explicitly registered product/service capabilities

Provider-specific configuration should stay behind the owning service or adapter unless a public provider-specific contract is intentionally approved.

## Workflow evolution

The initial implementation should stay small.

Preferred sequence:

1. schema and validator;
2. deterministic reference executor;
3. pure control/data nodes;
4. mock adapters and offline tests;
5. bounded run lifecycle;
6. MCP surface;
7. real service adapters;
8. only then adaptive graph patching and richer orchestration.

Do not jump directly to a plugin marketplace or a general-purpose distributed scheduler.

## MCP design

MCP is the intended AI-facing submission boundary, but the core runtime must not depend on MCP-specific message framing.

Keep:

```text
workflow/runtime core
        ↑
    MCP adapter
```

rather than embedding execution semantics inside MCP handlers.

The draft MCP contract lives in `docs/MCP_CONTRACT.md`.

## Security

Treat workflow JSON, model output, tool output, service responses, and retrieved content as untrusted.

Security-sensitive behavior must fail closed.

Never:

- commit or log secrets, credentials, tokens, or private topology;
- embed credentials in workflow JSON;
- expose raw provider error bodies when they may contain sensitive data;
- silently retry non-idempotent side effects;
- infer permission from the fact that an AI requested an operation.

## Documentation discipline

Current implementation is authoritative.

At the moment, this repository contains design documents only. Do not change README wording to imply a runtime, MCP service, schema, or node catalog is implemented until the corresponding code and tests exist.

When implementation begins:

- document the exact setup and test commands from the real repository;
- add deterministic tests for validation and execution semantics;
- keep examples aligned with the current schema;
- version breaking workflow-schema changes explicitly.

## Before a substantial change

Read:

- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/WORKFLOW_IR.md`
- `docs/MCP_CONTRACT.md`
- relevant Issues and PRs
- the FLAMORIS AI ecosystem map when cross-repository behavior is involved

Then identify:

- the authority being changed;
- whether behavior is runtime-core, adapter, MCP, or documentation;
- whether the change adds a side effect;
- whether the change expands permissions or resource use;
- whether the public workflow contract changes.

## Testing expectations

When code exists, prefer deterministic offline tests for:

- valid and invalid graph structure;
- unsupported schema versions;
- unknown node types;
- missing references;
- cycle policy;
- resource-limit rejection;
- cancellation;
- timeouts;
- fan-out/concurrency bounds;
- result-size limits;
- idempotency and side-effect boundaries;
- adapter failures;
- stable structured errors.

## Licensing

Unless stated otherwise, code and documentation are Apache License 2.0.

Models, weights, datasets, media, providers, and third-party components may use separate terms. Document them explicitly.
