# MCP Contract

## Status

**Draft design. No MCP server is implemented in this repository yet.**

MCP is intended to be the primary AI-facing control surface for FLAMORIS AI Runtime, but the execution core must remain transport-independent.

## Design goal

An AI should be able to:

1. discover what the runtime can do;
2. construct a workflow without guessing node types;
3. validate it;
4. submit it;
5. observe status;
6. retrieve results;
7. cancel it;
8. later, safely patch an eligible running/paused workflow.

The runtime must not infer permission merely because an AI requested an operation.

## Namespace

If exposed through FLAMORIS MCP Hub, the expected public namespace is conceptually:

```text
runtime.*
```

A directly connected MCP server may use shorter local tool names.

Exact naming should be finalized with implementation and ecosystem integration.

## MCP has two roles

This repository may use MCP in two different directions:

```text
AI / Agent
   │
   │ MCP control plane
   ▼
FLAMORIS AI Runtime
   │
   │ registered workflow capability
   ▼
external MCP server/tool
```

The first role is the Runtime's own MCP surface, used to discover, validate, submit, observe, and cancel workflows.

The second role is optional outbound execution: a configured external MCP tool may be exposed to a workflow as a registered capability.

These roles must not be confused. A workflow must not gain the ability to choose arbitrary MCP endpoints, credentials, or unrestricted tools. Connection configuration and authorization live outside portable workflow JSON.

## Initial tool surface

Conceptual tools:

```text
capabilities.list
workflow.validate
run.submit
run.status
run.result
run.cancel
```

Later:

```text
run.patch
run.events
```

Avoid one giant `execute_anything` tool.

## capabilities.list

Purpose:

Return the capabilities available to the current caller.

The response should be sufficient for an AI planner to compose valid workflow nodes.

It may include:

- node/capability identifier;
- version;
- input schema;
- output schema;
- side-effect classification;
- idempotency;
- cancellation support;
- permission/availability state;
- bounded resource hints;
- conceptual capability class such as algorithmic, future Vem, external AI/API, external MCP, or FLAMORIS service, when useful for planning.

It must not expose:

- credentials;
- private endpoints;
- private topology;
- secrets hidden behind adapters.

## workflow.validate

Purpose:

Validate a workflow without executing side effects.

Validation should cover:

- schema version;
- serialized size;
- node/edge limits;
- graph validity;
- node type existence;
- input/output references;
- capability availability;
- caller permissions;
- server execution policy;
- resource budgets;
- unsupported cycle/loop constructs.

A validation response should be structured.

Conceptual response:

```json
{
  "valid": false,
  "errors": [
    {
      "code": "unknown_node_type",
      "path": "nodes[2].type",
      "message": "Capability is not available."
    }
  ]
}
```

Messages should not leak sensitive implementation details.

## run.submit

Purpose:

Admit a validated workflow for execution.

The server must revalidate at submission time. A previous successful `workflow.validate` response is advisory and does not reserve permissions or capability availability.

Conceptual request:

```json
{
  "request_id": "uuid",
  "workflow": {},
  "inputs": {}
}
```

Important properties:

- bounded request size;
- caller-scoped authorization;
- idempotency semantics defined before side effects;
- structured admission failure;
- server-assigned `run_id`.

The workflow must not carry raw credentials.

## run.status

Purpose:

Return bounded execution state.

Potential states:

- `queued`
- `running`
- `succeeded`
- `failed`
- `cancelling`
- `cancelled`

Do not expose arbitrary raw upstream logs by default.

## run.result

Purpose:

Return the final declared workflow outputs and stable metadata.

Large generated media should normally be represented by references owned by the service that created them, not copied into the MCP response.

## run.cancel

Purpose:

Request cancellation.

Cancellation semantics must be explicit:

- cancellation is best-effort only where an upstream capability cannot stop immediately;
- already completed side effects are not rolled back automatically;
- cancellation must not be reported as successful until the runtime reaches a defined cancelled/terminal state.

## run.patch

Future feature.

Purpose:

Apply validated graph changes to an eligible run revision.

The patch contract must include:

- base run/workflow revision;
- explicit patch operations;
- validation before commit;
- rejection of edits that would rewrite completed side effects;
- resulting revision;
- provenance.

Do not accept arbitrary replacement of internal executor state.

## Events and streaming

Streaming is useful but should not be required for correctness.

A future event stream may include bounded events such as:

- run admitted;
- node started;
- node completed;
- node failed;
- run cancelled;
- run completed;
- graph revision changed.

Events must avoid secrets and unbounded model/provider output.

## Authentication and authorization

Transport authentication and runtime authorization are separate concerns.

Even after transport authentication, the runtime must evaluate whether the current principal may use each capability.

A future deployment may support different capability sets per principal.

## Error handling

Prefer stable error codes.

Examples:

- `invalid_request`
- `unsupported_schema_version`
- `invalid_workflow`
- `capability_unavailable`
- `permission_denied`
- `budget_exceeded`
- `duplicate_request`
- `run_not_found`
- `run_not_terminal`
- `run_cancelled`
- `upstream_failure`
- `internal_error`

Do not make callers parse prose to determine error class.

## Retry behavior

Retries are dangerous around side effects.

The MCP contract should:

- define idempotency for `run.submit`;
- avoid automatic retries with a fresh request ID after an uncertain submission;
- distinguish retryable transport failure from known runtime rejection;
- never silently replay non-idempotent side-effecting nodes.

## Relationship to MCP Hub

MCP Hub may aggregate and namespace Runtime tools.

Hub remains routing/aggregation infrastructure.

It must not:

- become the workflow executor;
- duplicate runtime run state;
- reinterpret workflow semantics;
- become a second capability registry authority.

## Relationship to Agent

AI Agent may use Runtime to execute an AI-authored graph.

Agent remains authoritative for:

- identity;
- conversation;
- memory;
- personality;
- Agent policy.

Runtime output may be referenced by Agent state, but Runtime must not become the Agent's memory store.

## Relationship to external AI/API and MCP capabilities

External AI/API services may be exposed through configured adapters.

External MCP tools may be exposed through configured MCP connections and registered tool schemas.

For both:

- workflow JSON selects a registered logical capability rather than carrying secrets or arbitrary endpoint configuration;
- capability discovery reflects caller-specific availability;
- timeout, cancellation, side-effect, cost/resource, and retry policy remain explicit;
- raw upstream errors are not automatically returned to callers;
- registration does not transfer ownership of remote state into the Runtime.

Future Vem capabilities should follow the same model once Vem has a stable callable contract.

## Relationship to Intelligence and Generation

Runtime should compose their public capabilities rather than re-own their domain state.

Conceptually:

```text
AI-authored workflow
       │
       ▼
   AI Runtime
    │      │
    │      └────► Generation MCP
    │
    └───────────► Intelligence MCP
```

This keeps the workflow executor generic while preserving the existing FLAMORIS authority map.
