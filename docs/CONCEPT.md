# Runtime Concept

## Status

**Design concept only. No runtime is implemented yet.**

FLAMORIS AI Runtime is intended to be a **general processing layer underneath an external intelligence**.

The intelligence that decides *what it wants to do* lives outside the Runtime.

Typical callers may include:

- ChatGPT or another remote AI assistant;
- `flamoris-ai-agent`;
- a FLAMORIS Studio assistant;
- another local or remote Agent;
- a future third-party AI client.

The Runtime is the layer that turns an AI-selected workflow into bounded execution.

## The basic shape

```text
        ChatGPT / external AI
                 │
                 │
        flamoris-ai-agent
                 │
                 │  intent + workflow
                 ▼
        FLAMORIS AI Runtime
                 │
      ┌──────────┼───────────┐
      │          │           │
      ▼          ▼           ▼
   Vision     Algorithms   Vem
      │          │           │
      ├──────► External AI ◄──┤
      │          │           │
      ├────────► MCP ◄────────┤
      │          │           │
      └──────► Generation ◄───┘
                 │
                 ▼
               result
                 │
                 ▼
        caller decides next step
```

The Agent does not have to live inside the Runtime.

In fact, keeping the Agent outside is a useful default:

- the Agent owns identity, conversation, memory, goals, and policy;
- the Runtime owns execution of a submitted processing graph;
- the Agent may inspect the result and submit another workflow when needed.

## A processing layer, not a personality

The Runtime is deliberately not a persistent personality or conversational Agent.

A caller may say:

> Inspect this image, determine what is in it, run the appropriate deterministic checks, ask a stronger external model if needed, then synthesize a spoken response.

The Runtime may execute a workflow such as:

```text
image
  ↓
Vision analysis
  ↓
algorithmic post-processing
  ↓
external AI / reasoning
  ↓
TTS / audio generation
  ↓
result
```

Another request may use a completely different graph:

```text
audio
  ↓
signal processing
  ↓
Vem capability
  ↓
external MCP tool
  ↓
result
```

The Runtime does not need to know *why* the caller chose that graph. It only needs to validate and execute it correctly.

## Ordinary AI capabilities, dynamically composed

Most individual capabilities are ordinary AI or software building blocks:

- vision;
- speech recognition;
- text or multimodal inference;
- TTS / voice generation;
- image, video, music, or other generation;
- embeddings and similarity;
- deterministic algorithms;
- external APIs;
- external AI services;
- MCP tools;
- future Vem capabilities;
- FLAMORIS product/service commands where explicitly exposed.

The unusual part is not any one capability.

The unusual part is that the **outer AI may choose and connect the capabilities itself by submitting a workflow**.

Instead of an application developer permanently wiring:

```text
Vision → LLM → TTS
```

the caller can create a workflow appropriate to the current task:

```text
Vision → algorithm → external AI → TTS
```

or:

```text
Vision → Vem → similarity → Generation MCP
```

or simply:

```text
algorithm
```

when AI is unnecessary.

## Open-ended composition

The long-term goal is intentionally broad composition.

The Runtime should not impose a product-level filter such as:

- "this is only an image workflow";
- "this is only an LLM chain";
- "this is only a media-generation graph";
- "this is only an Agent tool system".

If a capability has a stable contract and is explicitly registered, it may participate in a workflow.

This makes the Runtime a general execution layer rather than a domain-specific pipeline.

A useful design phrase is:

> **Open-ended composition. Bounded execution.**

The composition space may be broad.

Actual execution is still constrained by:

- registered capabilities;
- caller authorization;
- resource budgets;
- explicit side-effect policy;
- network/filesystem/credential boundaries;
- cancellation and timeout rules.

"Can be composed" must never mean "has ambient authority".

## Caller-driven adaptation

Because the intelligence is outside the Runtime, adaptation can happen at the caller level even before in-place graph patching exists.

```text
AI / Agent
   │
   ├─ submit workflow A
   │
   ▼
Runtime
   │
   └─ result
       │
       ▼
AI / Agent evaluates result
       │
       ├─ done
       │
       └─ submit workflow B
```

Later, safe Runtime graph patching may make some adaptive flows more efficient, but it is not required for the core model.

## Example: multimodal conversational response

A caller receives an image and wants to respond naturally.

```text
image input
   ↓
vision.describe
   ↓
algorithm.extract_features
   ↓
intelligence.reason
   ↓
generation.tts
   ↓
audio result
```

The caller can then send or present the generated audio.

The Runtime owns only the execution of this graph.

Conversation history and user identity remain outside, for example in `flamoris-ai-agent` or the calling application.

## Example: use an external specialist

A general Agent may decide that a specialized external capability is better for one step.

```text
input
  ↓
local algorithm
  ↓
external AI specialist
  ↓
external MCP tool
  ↓
result
```

The caller does not need the Runtime to become that specialist.

The Runtime only needs a registered adapter/capability with a stable contract.

## Why the separation matters

Keeping caller intelligence outside the Runtime gives several benefits:

- one Runtime can serve many different Agents and assistants;
- the Runtime remains stateless or execution-state-focused rather than personality-focused;
- Agent memory and policy are not duplicated;
- ChatGPT and FLAMORIS Agent can use the same execution layer;
- processing graphs can evolve independently from conversation/identity systems;
- capability security can be enforced once in the execution layer.

Conceptually:

```text
ChatGPT ──────────────┐
                     │
FLAMORIS AI Agent ───┼──► FLAMORIS AI Runtime
                     │
Studio AI ───────────┤
                     │
third-party Agent ───┘
```

This is a deliberate architecture goal, not an implementation claim.

## Relationship to Workflow IR

Workflow IR is the language the caller uses to describe the processing graph.

The Runtime is the machine that validates and executes it.

The outer AI is the planner that decides which workflow is useful.

```text
Planner       = what should be done?
Workflow IR   = how should the processing be composed?
Runtime       = can this be executed, and execute it safely
Capability    = one callable unit of processing
```

Keeping those roles separate is central to the design.
