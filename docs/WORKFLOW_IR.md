# Workflow IR

## Status

**Draft design. No schema is implemented or frozen yet.**

This document records the intended properties of the FLAMORIS AI Runtime workflow intermediate representation.

## Goals

The workflow IR should be:

- declarative;
- JSON-serializable;
- schema-versioned;
- deterministic to validate;
- independent of GUI layout;
- portable across MCP, CLI, Studio, tests, and AI planners;
- provider-neutral at the public boundary;
- able to compose algorithmic processing, future Vem capabilities, external AI/API calls, external MCP capabilities, and FLAMORIS services through one explicit capability model;
- explicit about dependencies and side effects;
- bounded enough to reject unsafe or unreasonably expensive graphs before execution.

## Non-goals

The IR should not initially be:

- a general programming language;
- an arbitrary code container;
- a visual editor save file;
- a provider configuration dump;
- a credential envelope;
- a persistence format for Agent memory;
- a persistence format for Generation jobs/assets.

## Draft top-level shape

Conceptually:

```json
{
  "schema_version": "0.1-draft",
  "workflow": {
    "id": "example",
    "name": "Example workflow"
  },
  "inputs": {},
  "nodes": [],
  "edges": [],
  "outputs": {},
  "limits": {}
}
```

Exact names may change before implementation.

## Workflow identity

A workflow should have a client-supplied logical identifier for traceability.

Runtime execution should assign a separate run identifier.

Do not use a human-readable workflow name as the idempotency key for side effects.

## Inputs

Inputs should declare type and, where practical, bounds.

Example:

```json
{
  "inputs": {
    "text": {
      "type": "string",
      "max_length": 12000
    }
  }
}
```

The runtime should validate submitted values before node execution.

## Nodes

A node should minimally identify:

- `id`
- `type`
- configuration/input bindings

Example:

```json
{
  "id": "summarize",
  "type": "intelligence.request",
  "with": {
    "task": "summarize",
    "input": "${inputs.text}"
  }
}
```

Node IDs must be unique inside the workflow.

Node types must come from the runtime capability registry. Unknown node types are validation errors, not dynamic imports.

## Capability classes

The IR is intentionally broader than an LLM workflow format.

A workflow may eventually contain nodes from several classes:

- `algorithm.*` — ordinary bounded algorithmic processing;
- `vem.*` — future Vem-backed capabilities once Vem has a stable callable contract;
- `intelligence.*` / `generation.*` — FLAMORIS-owned service capabilities;
- `external_ai.*` — configured local or remote AI services, including API-backed providers;
- `mcp.*` — configured external MCP tools/capabilities;
- later explicitly registered product/service capabilities.

These prefixes are conceptual and not frozen schema.

The important contract is that every executable node type is registered with machine-readable input/output, side-effect, permission, and resource metadata before a workflow may reference it.

This is what lets an outer AI inspect available building blocks and compose a graph whose outputs can actually feed the next stage instead of guessing ad-hoc text conventions.

### Algorithmic processing

Algorithm nodes are first-class workflow operations, not merely glue around AI calls.

They may perform deterministic transforms, analysis, filtering, scoring, conversion, geometry, signal/image processing, or other bounded operations.

Where reproducibility matters, the capability registry should identify the implementation/version whose semantics produced the result.

### Future Vem integration

Vem should integrate through the same capability registry rather than requiring a Vem-specific workflow language.

Until Vem has a stable callable contract, the IR should reserve only an integration boundary and avoid inventing Vem inputs, outputs, lifecycle, or state semantics.

### External AI / API calls

External AI services may be represented as registered capabilities.

The portable workflow should describe the logical capability and its inputs. It should not contain raw API keys, private endpoints, or unrestricted provider configuration.

### External MCP calls

External MCP tools may also become registered workflow capabilities.

The runtime must know the configured MCP connection and tool schema before validation succeeds. A workflow must not be allowed to supply an arbitrary MCP endpoint and invoke whatever tool name it wants.

## Edges

The initial graph should use explicit dependencies.

Example:

```json
{
  "edges": [
    ["analyze", "answer"]
  ]
}
```

Validation should reject:

- missing node references;
- duplicate invalid edges;
- unsupported cycles;
- impossible dependency ordering.

The first implementation should prefer DAG-only execution.

## Data flow between processing stages

Passing one capability's result into the next is a core use case, not an incidental feature.

Examples include:

```text
Vision result → algorithm
Vision result → external AI
speech recognition → reasoning
reasoning → TTS
algorithm result → MCP tool
Vem result → Generation capability
```

The IR therefore needs typed or schema-described outputs that can be referenced by downstream nodes without requiring every intermediate result to become human-readable text.

Structured JSON-like values may flow directly when their schemas are compatible.

Large media payloads should prefer bounded handles/references or explicitly supported streaming/materialization mechanisms rather than repeatedly embedding large binary/base64 values into every node boundary.

## References

References should be explicit and resolvable during validation/compilation.

Candidate reference families:

- `${inputs.name}`
- `${nodes.node_id.output}`
- `${nodes.node_id.output.field}`

The exact expression grammar must remain deliberately small.

Do not embed a full scripting language into reference expressions.

## Outputs

Workflow outputs should select already-produced data.

Example:

```json
{
  "outputs": {
    "result": "${nodes.answer.output}"
  }
}
```

Output size must be bounded.

Large binary assets should normally remain owned by the service that produced them and be returned as explicit references/handles rather than embedded into the runtime result.

## Limits

A workflow may request tighter limits than the server policy.

Example draft:

```json
{
  "limits": {
    "max_parallelism": 4,
    "timeout_seconds": 120
  }
}
```

A workflow must not be able to raise itself above server policy.

Effective limit:

```text
min(client-requested limit, server/caller policy limit)
```

## Capability metadata

For AI planning, the runtime should expose enough information to generate valid nodes.

A future capability description may resemble:

```json
{
  "type": "intelligence.request",
  "version": "1",
  "input_schema": {},
  "output_schema": {},
  "side_effects": false,
  "idempotent": true,
  "cancellable": true
}
```

This is illustrative, not frozen.

## Side-effect declaration

Capabilities must declare whether execution can create external state.

The runtime should not trust the workflow author to label a side-effecting node as pure. Side-effect classification belongs to registered capability metadata.

## Control flow

Initial control flow should remain explicit and bounded.

Candidates:

- `control.if`
- `control.switch`
- `control.merge`

Dynamic loops should be postponed.

If iteration is later introduced, it must include hard bounds.

## Secrets

Secrets must not be embedded in workflow JSON.

Bad:

```json
{
  "api_key": "..."
}
```

Preferred:

```json
{
  "credential_ref": "approved-runtime-binding"
}
```

Even a credential reference must be validated against the caller's authorization context.

The exact secret-binding design is intentionally deferred.

## Provider neutrality

The IR should express the capability needed, not leak deployment details unnecessarily.

Prefer:

```json
{
  "type": "intelligence.request",
  "with": {
    "task": "summarize"
  }
}
```

over encoding a machine name, private endpoint, or provider-specific invocation directly into the portable graph.

Provider selection can be added only where it is an intentional public choice.

The same rule applies to external MCP. Prefer a registered logical capability over embedding an MCP server URL, transport secret, or deployment-specific tool routing detail directly in the workflow.

## Example

```json
{
  "schema_version": "0.1-draft",
  "workflow": {
    "id": "classify-and-answer",
    "name": "Classify and answer"
  },
  "inputs": {
    "text": {
      "type": "string"
    }
  },
  "nodes": [
    {
      "id": "classify",
      "type": "intelligence.request",
      "with": {
        "task": "classify",
        "input": "${inputs.text}"
      }
    },
    {
      "id": "answer",
      "type": "intelligence.request",
      "with": {
        "task": "answer",
        "input": "${inputs.text}",
        "context": "${nodes.classify.output}"
      }
    }
  ],
  "edges": [
    ["classify", "answer"]
  ],
  "outputs": {
    "result": "${nodes.answer.output}"
  },
  "limits": {
    "max_parallelism": 2,
    "timeout_seconds": 60
  }
}
```

## Versioning

Breaking execution-semantic changes require a schema-version change.

The runtime should reject unsupported schema versions with a deterministic error.

"Best effort" interpretation of unknown future schemas is unsafe and should not be used.

## Future adaptive patch format

Adaptive workflows should use explicit patch operations rather than replacing hidden executor state.

Conceptual example:

```json
{
  "base_revision": 3,
  "operations": [
    {
      "op": "add_node",
      "node": {
        "id": "refine",
        "type": "intelligence.request",
        "with": {
          "task": "refine",
          "input": "${nodes.answer.output}"
        }
      }
    },
    {
      "op": "add_edge",
      "from": "answer",
      "to": "refine"
    }
  ]
}
```

This is a future design direction, not part of Phase 0.
