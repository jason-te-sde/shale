## first-session-v011.json
| benchmark | ops/s | p99 (us) | settled ops/s | write-amp (returned / settled) | files per level (before -> after) | compaction (before -> after) |
| --- | --- | --- | --- | --- | --- | --- |
| fillseq | 353,592 [351,506, 361,211] | 4.9 [4.9, 4.9] | n/a | 2.17 / n/a | n/a | n/a |
| fillrandom | 250,686 [203,065, 256,023] | 5.8 [5.5, 16.4] | n/a | 5.09 / n/a | n/a | n/a |
| fillsync | 339 [339, 341] | 4,681.6 [4,664.1, 4,686.7] | n/a | 1.19 / n/a | n/a | n/a |
| overwrite | 227,493 [225,379, 229,440] | 6.0 [5.7, 6.1] | n/a | 6.03 / n/a | n/a | n/a |
| readrandom | 335,910 [322,018, 336,597] | 4.3 [4.2, 5.5] | n/a | n/a | n/a | n/a |
| readmissing | 1,561,970 [1,557,656, 1,567,684] | 1.9 [1.9, 2.0] | n/a | n/a | n/a | n/a |
| readseq | 4,308,290 [4,271,166, 4,313,903] | n/a | n/a | n/a | n/a | n/a |
| seekrandom | 170,310 [170,249, 170,769] | 9.0 [8.8, 9.0] | n/a | n/a | n/a | n/a |
## shale-writes.json
| benchmark | ops/s | p99 (us) | settled ops/s | write-amp (returned / settled) | files per level (before -> after) | compaction (before -> after) |
| --- | --- | --- | --- | --- | --- | --- |
| fillrandom | 310,618 [306,986, 314,251] | 5.1 [5.0, 5.1] | 309,932 [306,308, 313,556] | 2.60 / 2.60 | 3/5/0/0/0/0/0 | compactions 1 |
| overwrite | 315,225 [314,338, 316,113] | 5.1 [5.1, 5.2] | 314,630 [313,748, 315,512] | 2.60 / 2.60 | 3/5/10/0/0/0/0 | compactions 1 |
## leveldb-writes.json
| benchmark | ops/s | p99 (us) | settled ops/s | write-amp (returned / settled) | files per level (before -> after) | compaction (before -> after) |
| --- | --- | --- | --- | --- | --- | --- |
| fillrandom | 301,846 [299,803, 303,889] | 5.7 [5.7, 5.7] | 272,887 [268,793, 276,981] | n/a / n/a | 0/6/1/0/0/0/0 -> 1/0/7/0/0/0/0 | write_mb 30 -> 46 |
| overwrite | 301,281 [292,749, 309,813] | 5.8 [5.5, 6.0] | 289,674 [281,796, 297,551] | n/a / n/a | 2/6/8/0/0/0/0 -> 3/6/8/0/0/0/0 | write_mb 29 -> 32 |
## leveldb-first.json
| benchmark | ops/s | p99 (us) | settled ops/s | write-amp (returned / settled) | files per level (before -> after) | compaction (before -> after) |
| --- | --- | --- | --- | --- | --- | --- |
| readrandom | 624,612 [352,980, 626,018] | 2.0 [1.9, 3.3] | n/a | n/a | 1/0/1/0/0/0/0 -> 0/0/2/0/0/0/0 | write_mb 0 -> 2 |
| readmissing | 2,153,896 [2,148,516, 2,185,165] | 1.5 [1.5, 1.5] | n/a | n/a | 0/0/2/0/0/0/0 | write_mb 2 |
| readseq | 13,121,930 [13,009,535, 13,203,865] | n/a | n/a | n/a | 0/0/2/0/0/0/0 | write_mb 2 |
| seekrandom | 477,362 [476,394, 477,639] | 3.1 [3.1, 3.1] | n/a | n/a | 0/0/2/0/0/0/0 | write_mb 2 |
## leveldb-first-1.json leveldb-first-2.json leveldb-first-3.json
| benchmark | ops/s | p99 (us) | settled ops/s | write-amp (returned / settled) | files per level (before -> after) | compaction (before -> after) |
| --- | --- | --- | --- | --- | --- | --- |
| readrandom | 615,539 [589,789, 617,204] | 2.2 [2.2, 2.2] | n/a | n/a | 1/0/1/0/0/0/0 -> 0/0/2/0/0/0/0 | write_mb 0 -> 2 |
| readmissing | 2,158,516 [2,102,607, 2,165,069] | 1.5 [1.5, 1.6] | n/a | n/a | 0/0/2/0/0/0/0 | write_mb 2 |
| readseq | 13,033,205 [12,969,471, 13,160,414] | n/a | n/a | n/a | 0/0/2/0/0/0/0 | write_mb 2 |
| seekrandom | 475,235 [474,981, 478,044] | 3.1 [3.1, 3.1] | n/a | n/a | 0/0/2/0/0/0/0 | write_mb 2 |
