# Repository guide

Fork of [`hharte/mm_manager`](https://github.com/hharte/mm_manager), a
Manager for Nortel Millennium payphones. C11, CMake, bundled SQLite.

## Layout

| Path | What it is |
|---|---|
| `src/mm_manager.c` | Main program: CLI, main loop, `process_mm_table()` per-table dispatch, table download. CARDMAN call sites are the `DLOG_MT_FUNF_CARD_AUTH` and `DLOG_MT_CALL_DETAILS` cases. |
| `src/mm_manager.h` | Packed wire structs for every DLOG table and the table-ID enum. |
| `src/mm_proto.c` | Packet framing, CRC, ACK/retry, pcap/UDP mirroring. |
| `src/mm_connection.c`, `src/mm_serial*.c`, `src/mm_modem.c` | Modem and serial transport, or file playback. |
| `src/mm_accounting.c` | SQLite persistence (TAUTH, TCDR, TOPCODE, ...). |
| `src/mm_util.c` | BCD/phone helpers, `mask_pan()`, table printers. |
| `src/mm_cardman_bridge.[ch]` | Fork-only HTTP/JSON client to the CARDMAN bridge. |
| `src/mm_packtest.c` | `sizeof` assertions for every wire struct. |
| `src/mm_card.c`, `mm_rate.c`, ... | Standalone table editor CLIs. |
| `tables/` | Binary table files downloaded to terminals. |
| `tools/cardman_stub_bridge.py` | Approve-all / `--decline` stub bridge for local tests. |
| `generate_*.py` | Table generators (LCD, NPA, intl SBR, ...). |
| `docs/cardman-compat/` | CARDMAN architecture analysis and plan. |
| `AI/` | These instruction files. |

## Build and test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest -C Release --output-on-failure
```

Warnings are errors (`-Wall -Wextra -Werror`, `CMakeLists.txt` L95).
A change is not done until every target builds clean, not only
`mm_manager`. CI (`.github/workflows/release.yml`) builds Linux glibc,
Linux static musl, macOS, and Windows x64/x86.

### Local end-to-end check (no hardware)

```sh
python3 tools/cardman_stub_bridge.py --port 8420 &        # or --decline
CARDMAN_BRIDGE_ADDR=127.0.0.1:8420 ./build/mm_manager -f <session.dlog>
```

`-f <file>` without `-m` plays back a session recorded with `-l`
(README, "logging" section). Only use fixtures with test card numbers.

## Conventions

- **Wire structs are `PACKED`** and little-endian. Convert every
  multi-byte field with `LE16`/`LE32`/`LE64` after receiving and before
  sending. If you add or change a struct, add a `sizeof` assert in
  `src/mm_packtest.c`.
- **Strings**: bounded `snprintf` only; no `strcpy`/`sprintf`.
- **Phone/card numbers** are packed BCD; use `phone_num_to_string()`.
- **Style**: match surrounding code (4-space indent, K&R braces,
  `/* */` comments explaining why, not what).
- **Fork delta**: CARDMAN-specific logic goes in `mm_cardman_*.c` files
  where possible; touch upstream files only at call sites, so upstream
  merges stay easy.
- **Line numbers** in docs drift; cite a symbol along with the line.
