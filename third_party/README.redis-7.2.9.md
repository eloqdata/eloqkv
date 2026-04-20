This directory records third-party licensing material for Redis 7.2.9 code
that may be referenced or imported into EloqKV for Redis-compatible DUMP /
RESTORE support.

Scope

- Upstream source tree: `/home/chen/redis`
- Upstream version: `7.2.9`
- Upstream repository license: BSD-3-Clause, copied to
  `third_party/licenses/redis-7.2.9-BSD-3-Clause.txt`
- Additional component license likely needed when importing Redis LZF decoder:
  `third_party/licenses/lzf-bsd-or-gpl.txt`

Files likely to be referenced or minimally imported

- `src/rdb.c`
- `src/cluster.c`
- `src/listpack.c`
- `src/listpack.h`
- `src/intset.c`
- `src/intset.h`
- `src/lzf_d.c`
- `src/lzf.h`
- `src/lzfP.h`

Integration rules

- Preserve the original upstream copyright / license header in any copied file.
- If a file is trimmed for EloqKV, keep the upstream header and add a short
  local note such as `Modified for integration into EloqKV.`
- Keep the copied license texts in this directory in sync with imported code.
