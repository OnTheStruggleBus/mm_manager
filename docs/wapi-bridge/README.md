# WAPI bridge

Architecture decision and spec for the production CARDMAN WAPI bridge,
the terminal configuration API (advertising prompts, rate tables), and
the shared vocabulary other apps can integrate with. Docs only; nothing
here is built yet.

| File | What it covers |
|---|---|
| [`ADR-0001-WAPI-BRIDGE.md`](ADR-0001-WAPI-BRIDGE.md) | The decisions (W1-W10), the owner decisions still open (DW-1 to DW-6), consequences. Start here. |
| [`SPEC.md`](SPEC.md) | Components, card plane and config plane APIs, advert and rate-table rules, staging and delivery, events, BRICKS/CARDMAN constraints and requests (R9-R12), phasing, acceptance, hardware checks, and inventories of every CARDMAN route and DLOG message considered. |
| [`VOCABULARY.md`](VOCABULARY.md) | Payphone Interchange Vocabulary (PIV) v1: field dictionary mapped to CARDMAN, DLOG and the bridge hop; sensitivity classes; CloudEvents envelope; enumerations; error envelope; versioning. |
| [`ADVERSARIAL_REVIEW.md`](ADVERSARIAL_REVIEW.md) | Red-team findings against the first draft and what changed because of them. |

Builds on [`../cardman-compat/`](../cardman-compat/README.md) (the card
path, `BRIDGE_CONTRACT.md`, objectives O1/O2, tasks T14-T16) without
reopening its settled decisions.
