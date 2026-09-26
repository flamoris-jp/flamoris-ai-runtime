# FLAMORIS AI Runtime

**A runtime for workflows written by AI.**

FLAMORIS AI Runtime is a planned provider-neutral **processing and execution layer for external AI callers**.

The caller intelligence lives outside the Runtime. ChatGPT, `flamoris-ai-agent`, Studio AI, or another Agent can decide what it wants to do, compose a workflow through MCP, and hand that graph to the Runtime. The Runtime validates, compiles, and executes the requested processing without needing to own the caller's personality, memory, or conversation.

The core idea is simple: **the outer AI plans; the Runtime executes.**

> **Status: design only. No production runtime is implemented yet.**
>
> The documents in this repository define the initial responsibility boundary and draft contracts. They are intentionally explicit about what is planned versus what already exists.

Part of the [FLAMORIS AI ecosystem](https://github.com/flamoris-jp/flamoris-ai/blob/main/docs/ai-ecosystem.md).

## 🧭 Repository identity / このRepositoryは何者？

### What it is / 何者か

A headless processing layer for declarative AI workflows, designed so an external AI can discover available capabilities, submit a workflow graph through MCP, inspect execution results, and decide what to do next.

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
 ChatGPT / external AI / Studio AI
                │
        flamoris-ai-agent
                │
                │ intent + workflow IR
                ▼
       FLAMORIS AI Runtime
        ├─ validate
        ├─ compile
        ├─ execute
        ├─ observe
        └─ cancel / later patch
                │
      ┌─────────┼──────────┬─────────┐
      ▼         ▼          ▼         ▼
   Vision   Algorithms    Vem    External AI
      │         │          │         │
      ├─────────┼──────────┼──────► MCP
      │         │          │         │
      └─────────┴──────────┴────► Generation
                │
                ▼
              result
```

The Runtime is an **execution substrate beneath the caller intelligence**, not a new umbrella authority over the AI ecosystem.

A persistent Agent can sit outside the Runtime very naturally: the Agent owns identity, memory, conversation, goals, and policy, while the Runtime executes the processing graph the Agent selected. ChatGPT or another external AI can use the same layer without adopting the FLAMORIS Agent model.

See [Runtime Concept](docs/CONCEPT.md) for the intended caller/runtime split and example flows.

## What workflows are meant to compose

The workflow graph is not limited to LLM calls. The long-term design is to let one runtime graph compose several kinds of work behind the same validated capability boundary:

- **algorithmic processing** — deterministic or explicitly versioned processing implemented as runtime/library capabilities, useful for transforms, analysis, filtering, scoring, conversion, and other work that does not require an AI model;
- **Vem capabilities** — future Vem-backed processing should be exposable as registered capabilities once Vem has a stable callable contract; the portable workflow IR should not hard-code Vem-specific execution details before that contract exists;
- **external AI services** — local or remote AI providers, including API-based services, may be called through registered adapters without embedding provider credentials or private endpoints in workflow JSON;
- **external MCP capabilities** — configured MCP servers/tools may be exposed to workflows as registered capabilities, subject to the same validation, permission, timeout, side-effect, and resource rules as any other node.

Conceptually, a single graph may eventually mix them:

```text
input
  │
  ├─► algorithmic preprocessing
  │
  ├─► Vem capability
  │
  ├─► external AI / API
  │
  └─► external MCP capability
            │
            ▼
          result
```

These are **capability classes**, not permission shortcuts. AI-authored workflows still receive only the capabilities explicitly registered and authorized for that caller.

The Runtime is intentionally not restricted to one "kind" of AI application. A caller may route image analysis into another processing stage, combine deterministic algorithms with model inference, call an external specialist AI or MCP tool, generate speech for a response, or avoid AI entirely for steps where an ordinary algorithm is better.

The unusual part is not Vision, TTS, APIs, or MCP individually. The unusual part is that the **outer AI can choose how those ordinary capabilities are composed for the current task**.

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
- service adapters to existing FLAMORIS authorities;
- first registered algorithmic capabilities;
- bounded adapters for external AI/API and MCP capabilities;
- a reserved integration boundary for future Vem capabilities, without freezing Vem-specific semantics prematurely.

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

- **Outer AI plans; Runtime executes.** The caller intelligence stays outside the processing layer.
- **AI-authored, runtime-validated.**
- **Headless first.** GUI metadata must not define execution semantics.
- **Portable IR.** Provider and deployment details stay behind explicit capabilities.
- **Hybrid execution.** A workflow may combine Vision, speech/TTS, ordinary algorithms, future Vem capabilities, external AI/API calls, MCP capabilities, and FLAMORIS services without turning any of them into ambient authority.
- **Open-ended composition, bounded execution.** The Runtime should avoid arbitrary product-level restrictions on what registered capabilities may be composed, while still enforcing permissions, side-effect rules, credentials, network/filesystem boundaries, timeouts, and resource budgets.
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

FLAMORIS AI Runtimeは、**ChatGPTやAI Agentの下に置く汎用の処理・実行層**です。

考える主体はRuntimeの外側にいます。ChatGPT、`flamoris-ai-agent`、Studio内AI、その他のAgentが「何をしたいか」を決め、利用可能なcapabilityからWorkflow IRを組み立ててMCP経由でRuntimeへ渡します。

Runtimeは人格やConversationを持つ必要はありません。**外側のAIが考え、Runtimeが処理する**という分離です。

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

WorkflowはLLM呼び出しだけを対象にしません。たとえばVisionで画像を解析し、その結果をアルゴリズム処理へ流し、必要な部分だけ外部の賢いAIへ渡し、最後にTTSで音声応答を作る、といった処理をひとつのgraphとして表現できます。

将来的には、同じgraphの中で:

- **アルゴリズムによる各種処理**
- **Vemのcapability**
- **外部AI / APIの呼び出し**
- **外部MCPのcapability呼び出し**
- **FLAMORIS内の各service**
- **Vision / speech recognition / TTSなどの一般的なAI capability**

を組み合わせられる構想です。

ここで重要なのは、個々のCapabilityが特別なのではなく、**外側のAIがその場で処理の組み方を決められる**ことです。固定された `Vision → LLM → TTS` pipelineではなく、必要に応じて `Vision → algorithm → external AI → TTS` にしたり、MCPやVemを途中へ挟んだり、AIが不要ならalgorithmだけで終えることもできます。

構想上は用途の組み合わせをできるだけ限定しません。一方で実行権限は無制限にしません。

> **Open-ended composition. Bounded execution.**
>
> 組み方は広く、実行は厳格に。

という境界を目指します。

Vemについては、安定した呼び出しcontractが定義された時点でregistered capabilityとして接続し、現段階でWorkflow IRへVem固有仕様を固定しません。外部AI/APIやMCPも、workflow JSONにcredentialや任意endpointを直接埋め込むのではなく、Runtime側で登録・許可されたadapter / capabilityを通して利用します。

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
