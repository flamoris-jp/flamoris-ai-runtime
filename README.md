# FLAMORIS AI Runtime

**A runtime for workflows written by AI.**

FLAMORIS AI Runtime is a planned provider-neutral execution runtime for AI-authored workflow graphs.

The core idea is simple: a human or AI describes an intent, an AI composes a bounded workflow through MCP, and the runtime validates, compiles, and executes that graph without requiring a human to build a visual node graph first.

> **Status: design only. No production runtime is implemented yet.**
>
> The documents in this repository define the initial responsibility boundary and draft contracts. They are intentionally explicit about what is planned versus what already exists.

Part of the [FLAMORIS AI ecosystem](https://github.com/flamoris-jp/flamoris-ai/blob/main/docs/ai-ecosystem.md).

## 🧭 Repository identity / このRepositoryは何者？

### What it is / 何者か

A headless execution runtime for declarative AI workflows, designed so an AI can discover available capabilities, submit a workflow graph through MCP, inspect execution results, and adapt the graph when needed.

It is inspired by the usefulness of node-based systems such as ComfyUI, but it is **not a ComfyUI compatibility project**. The workflow format is intended to be a portable execution IR for FLAMORIS rather than a UI serialization format.

### What it owns / 主な責任範囲

FLAMORIS AI Runtime is intended to own:

- the runtime workflow IR and schema versioning;
- workflow validation and compilation;
- bounded execution of workflow nodes and control-flow primitives;
- per-run execution state, limits, cancellation, and result assembly;
- runtime capability discovery;
- a narrow MCP surface for AI-authored workflow submission;
- later, safe graph patching for adaptive workflows.

### What it does not own / 持たない責任

It does **not** own:

- persistent Agent identity, conversations, memory, or personality - owned by [flamoris-ai-agent](https://github.com/flamoris-jp/flamoris-ai-agent);
- raw language/reasoning/coding provider authority - owned by [flamoris-intelligence-mcp](https://github.com/flamoris-jp/flamoris-intelligence-mcp);
- generative-media workflow/job/asset authority - owned by [flamoris-generation-mcp](https://github.com/flamoris-jp/flamoris-generation-mcp);
- local GPU/runtime lifecycle transitions - owned by [flamoris-gpu-node-manager](https://github.com/flamoris-jp/flamoris-gpu-node-manager);
- Studio account/session/UI state;
- FLAMORIS desktop product documents or editing history;
- arbitrary shell, Python, filesystem, network, or credential authority.

The runtime may call other FLAMORIS services through explicit adapters, but it must not become a second authority for their state.

### Current status / 現在の状態

This repository currently contains **design documentation only**.

The first implementation should begin with a small schema, validator, deterministic reference executor, and mock/service adapters. MCP execution, durable run state, adaptive graph patching, and Studio visualization should be added only after the core execution contract is proven.

### Where it fits / FLAMORISのどこに属する？

Conceptually:

```text
Human / AI / Agent / Studio
            │
            │ intent
            ▼
        AI planner
            │
            │ MCP: workflow IR
            ▼
   FLAMORIS AI Runtime
    ├─ validate
    ├─ compile
    ├─ execute
    ├─ observe
    └─ cancel / later patch
            │
            ├────────► Intelligence MCP
            ├────────► Generation MCP
            └────────► other explicit capabilities
```

The runtime is an **execution substrate**, not a new umbrella authority over the AI ecosystem.

## The core loop

The intended interaction is:

```text
1. Discover capabilities
2. Compose workflow
3. Validate
4. Optionally dry-run / compile
5. Execute
6. Inspect result
7. Later: patch and continue when adaptation is needed
```

The important rule is:

> **The AI may author the workflow, but the runtime never trusts the workflow.**

Every submitted graph must pass schema checks, capability checks, resource limits, permission checks, and execution policy before any side effect occurs.

## Example draft workflow

This example is illustrative. The schema is not yet frozen.

```json
{
  "schema_version": "0.1-draft",
  "workflow": {
    "id": "analyze-and-answer",
    "name": "Analyze input and produce an answer"
  },
  "inputs": {
    "text": {
      "type": "string"
    }
  },
  "nodes": [
    {
      "id": "analyze",
      "type": "intelligence.request",
      "with": {
        "task": "analyze",
        "input": "${inputs.text}"
      }
    },
    {
      "id": "answer",
      "type": "intelligence.request",
      "with": {
        "task": "answer",
        "input": "${nodes.analyze.output}"
      }
    }
  ],
  "edges": [
    ["analyze", "answer"]
  ],
  "outputs": {
    "result": "${nodes.answer.output}"
  }
}
```

The runtime should care about execution semantics, not canvas coordinates, visual layout, or editor-only metadata.

## Why this exists

Visual node systems are useful when a human wants to inspect and edit a graph.

FLAMORIS also needs the inverse:

```text
human intent
    ↓
AI composes graph
    ↓
runtime executes graph
    ↓
human sees result
```

A GUI may visualize or edit the graph later, but the graph must remain valid without a GUI.

This makes the same execution contract usable from:

- MCP;
- CLI;
- Studio;
- AI Agent;
- tests;
- future visual editors.

## Initial safety model

The first runtime should prefer a small allowlisted node set over an open-ended plugin free-for-all.

Initial design rules:

- capability discovery before composition;
- schema-versioned workflows;
- explicit allowlisted node types;
- no arbitrary code execution by default;
- no ambient filesystem or network access;
- no credential values embedded in workflow JSON;
- bounded node count, graph depth, fan-out, output size, concurrency, and duration;
- explicit cancellation;
- deterministic validation errors;
- side-effecting nodes clearly distinguished from pure nodes;
- service ownership preserved across repository boundaries.

See [Architecture](docs/ARCHITECTURE.md), [Workflow IR](docs/WORKFLOW_IR.md), and [MCP Contract](docs/MCP_CONTRACT.md).

## Proposed implementation phases

### Phase 0 - Contract spike

- workflow IR draft;
- JSON Schema;
- validator;
- deterministic in-process reference executor;
- pure control/data nodes;
- mock capability nodes;
- offline tests.

### Phase 1 - Runtime service

- bounded run lifecycle;
- capability registry;
- structured errors;
- cancellation;
- execution budgets;
- MCP surface;
- service adapters to existing FLAMORIS authorities.

### Phase 2 - Adaptive workflows

- safe graph patch operations;
- pause/resume boundaries;
- partial re-execution where semantics are explicit;
- provenance for graph revisions and outputs.

### Phase 3 - Human inspection

- Studio visualization;
- optional graph editor;
- execution trace inspection;
- export/import of portable workflow IR.

These phases are proposals, not implemented features.

## Repository principles

- **AI-authored, runtime-validated.**
- **Headless first.** GUI metadata must not define execution semantics.
- **Portable IR.** Provider and deployment details stay behind explicit capabilities.
- **One authority per domain.** Runtime execution must not absorb Agent, Generation, Intelligence, GPU, or product state ownership.
- **Bounded behavior.** Resource use, side effects, retries, and permissions are explicit.
- **No speculative compatibility promises.** ComfyUI, LangGraph, n8n, or other workflow formats are not automatically supported.
- **Human-authoritative.** AI may propose and submit graphs; humans define the capabilities and permissions the runtime is allowed to expose.

## FLAMORIS

FLAMORIS is open-source software for creative work and AI-native production.

Use it however you like.

Commercial use is welcome and does not require permission. If you'd like, we'd be happy to hear what you used FLAMORIS for. This is completely optional.

FLAMORIS software is provided as-is. We do not provide individual support or guaranteed assistance.

If you run into trouble, let your AI assistant read the repository, documentation, Issues, tests, logs, and source code and help you solve it.

If FLAMORIS helps you or you find it interesting, your support helps fund development and keeps the project growing. 🌱

<sub>Mostly GPU bills.</sub>

---

## 日本語

FLAMORIS AI Runtimeは、**AI自身が組み立てたWorkflowを実行するためのheadless runtime**です。

人間が毎回Node graphを手で組むのではなく、AIが利用可能なcapabilityを確認し、MCP経由でWorkflow IRを組み立ててRuntimeへ渡します。

Runtime側は、そのJSONを信用しません。

```text
capabilities
    ↓
validate
    ↓
compile
    ↓
execute
    ↓
result
    ↓
将来: patch / continue
```

という境界を通して、schema、permission、resource limit、side effectを検証してから実行します。

ComfyUIのようなnode graphの便利さを参考にしますが、ComfyUI互換runtimeを目標にはしません。Canvas座標やGUI状態ではなく、**AIやCLIやStudioから共通利用できるportableな実行IR**を目指します。

現在は設計段階です。実装済みRuntimeやMCP serviceがあるという意味ではありません。

### 責任範囲

AI Runtimeが将来担当するもの:

- Workflow IR / schema version
- validate / compile
- bounded execution
- run state / cancel / result
- capability discovery
- AIがWorkflowを差し込むためのMCP surface
- 将来のsafe graph patch

担当しないもの:

- AgentのIdentity / Conversation / Memory / Personality
- Intelligence providerのauthority
- Generationのdomain workflow / job / asset authority
- GPU runtime切替
- Studioのaccount / UI state
- Desktop productのDocument / Project state
- 任意shell / Python / filesystem / networkへの無制限アクセス

**AIがWorkflowを書く。でもRuntimeはAIを信用しない。**

ここを最初の設計原則にします。🐈⚙️

## License

Code and documentation in this repository are licensed under the [Apache License 2.0](LICENSE), unless otherwise noted.

AI models, model weights, datasets, media, and other non-code assets may use separate licenses. State their applicable licenses alongside those assets.
