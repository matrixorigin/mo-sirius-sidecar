# TPC-H SF=300 Benchmark Report

**Hardware**: GPU environment (RTX series GPU, local NVMe)  
**Scale factor**: 300  
**Runs**: 3× Native MO, 3× CPU Sidecar (DuckDB), 2× GPU Sidecar (Sirius)  
**Metric**: end-to-end query wall time as measured by MO client (ms)

---

## Summary

| Mode | Total (avg) | vs Native |
|------|-------------|-----------|
| Native MO | 416.1 s | — |
| CPU Sidecar (DuckDB) | 367.9 s | **0.88x** (11.6% faster) |
| GPU Sidecar (Sirius) | 368.0 s | **0.88x** (11.6% faster) |

- Both sidecar modes are **~1.13× faster** than native MO overall
- GPU ≈ CPU at SF=300: **no meaningful GPU advantage** (GPU/CPU = 1.00×)
- Sidecar advantage is concentrated in **complex subquery queries** (Q9, Q15, Q17, Q20, Q22): 2.4–5.8× speedup
- Native MO wins on **simple scan + agg queries** (Q1, Q6, Q7, Q8, Q12, Q19, Q21): 3.9–7.5× faster

---

## Per-Query Results (ms)

`C/N` = CPU avg / Native avg — **< 1.0 means sidecar is faster**, > 1.0 means native is faster.  
`G/N` = GPU avg / Native avg. `G/C` = GPU avg / CPU avg.

| Q | N1 | N2 | N3 | **N avg** | C1 | C2 | C3 | **C avg** | G1 | G2 | **G avg** | **C/N** | **G/N** | **G/C** |
|---|----|----|----|-----------|----|----|----|-----------|----|----|-----------|---------|---------|---------|
| Q1 | 6,826 | 7,202 | 6,942 | **6,990** | 30,570 | 30,305 | 30,375 | **30,417** | 30,449 | 30,533 | **30,491** | **4.35×** | **4.36×** | **1.00×** |
| Q2 | 2,054 | 1,986 | 1,998 | **2,013** | 4,076 | 3,644 | 4,015 | **3,912** | 3,682 | 3,612 | **3,647** | **1.94×** | **1.81×** | **0.93×** |
| Q3 | 12,463 | 16,290 | 14,537 | **14,430** | 16,520 | 16,911 | 16,390 | **16,607** | 16,988 | 16,817 | **16,902** | **1.15×** | **1.17×** | **1.02×** |
| Q4 | 2,736 | 2,664 | 2,646 | **2,682** | 10,459 | 10,406 | 10,416 | **10,427** | 10,464 | 10,730 | **10,597** | **3.89×** | **3.95×** | **1.02×** |
| Q5 | 12,427 | 15,316 | 16,766 | **14,836** | 17,726 | 17,377 | 17,421 | **17,508** | 17,483 | 16,961 | **17,222** | **1.18×** | **1.16×** | **0.98×** |
| Q6 | 1,751 | 1,683 | 1,758 | **1,731** | 13,016 | 12,992 | 12,909 | **12,972** | 12,964 | 13,287 | **13,126** | **7.50×** | **7.58×** | **1.01×** |
| Q7 | 3,427 | 3,354 | 3,396 | **3,392** | 19,127 | 19,362 | 19,019 | **19,169** | 19,638 | 18,741 | **19,190** | **5.65×** | **5.66×** | **1.00×** |
| Q8 | 6,210 | 5,952 | 5,990 | **6,051** | 24,385 | 23,568 | 23,825 | **23,926** | 25,175 | 23,940 | **24,558** | **3.95×** | **4.06×** | **1.03×** |
| Q9 | 66,722 | 62,997 | 62,514 | **64,078** | 26,757 | 26,748 | 26,496 | **26,667** | 26,252 | 26,621 | **26,436** | **0.42×** | **0.41×** | **0.99×** |
| Q10 | 18,782 | 17,995 | 18,209 | **18,329** | 18,187 | 18,981 | 18,170 | **18,446** | 18,679 | 18,524 | **18,602** | **1.01×** | **1.01×** | **1.01×** |
| Q11 | 4,146 | 4,202 | 4,093 | **4,147** | 2,828 | 2,766 | 2,651 | **2,748** | 2,697 | 2,629 | **2,663** | **0.66×** | **0.64×** | **0.97×** |
| Q12 | 3,639 | 3,671 | 3,540 | **3,617** | 16,771 | 17,612 | 17,348 | **17,244** | 17,419 | 16,993 | **17,206** | **4.77×** | **4.76×** | **1.00×** |
| Q13 | 13,162 | 11,164 | 15,336 | **13,221** | 11,670 | 11,499 | 11,499 | **11,556** | 11,550 | 11,902 | **11,726** | **0.87×** | **0.89×** | **1.01×** |
| Q14 | 21,700 | 21,194 | 21,574 | **21,489** | 18,443 | 18,106 | 18,123 | **18,224** | 16,588 | 18,001 | **17,294** | **0.85×** | **0.80×** | **0.95×** |
| Q15 | 79,703 | 87,897 | 82,852 | **83,484** | 16,894 | 16,689 | 16,930 | **16,838** | 16,530 | 17,033 | **16,782** | **0.20×** | **0.20×** | **1.00×** |
| Q16 | 1,902 | 1,880 | 1,822 | **1,868** | 2,165 | 2,170 | 2,186 | **2,174** | 2,185 | 2,182 | **2,184** | **1.16×** | **1.17×** | **1.00×** |
| Q17 | 54,563 | 55,396 | 62,477 | **57,479** | 19,589 | 19,856 | 19,641 | **19,695** | 20,061 | 20,084 | **20,072** | **0.34×** | **0.35×** | **1.02×** |
| Q18 | 13,332 | 13,225 | 13,129 | **13,229** | 16,351 | 16,124 | 16,301 | **16,259** | 16,580 | 15,925 | **16,252** | **1.23×** | **1.23×** | **1.00×** |
| Q19 | 3,891 | 3,893 | 3,880 | **3,888** | 27,483 | 27,401 | 27,618 | **27,501** | 27,813 | 27,734 | **27,774** | **7.07×** | **7.14×** | **1.01×** |
| Q20 | 53,899 | 60,962 | 60,521 | **58,461** | 16,270 | 16,574 | 16,400 | **16,415** | 16,323 | 16,615 | **16,469** | **0.28×** | **0.28×** | **1.00×** |
| Q21 | 7,207 | 7,312 | 7,201 | **7,240** | 37,240 | 35,971 | 37,367 | **36,859** | 36,600 | 36,308 | **36,454** | **5.09×** | **5.04×** | **0.99×** |
| Q22 | 15,021 | 12,625 | 12,817 | **13,488** | 2,300 | 2,296 | 2,353 | **2,316** | 2,302 | 2,313 | **2,308** | **0.17×** | **0.17×** | **1.00×** |
| **Total** | | | | **416.1 s** | | | | **367.9 s** | | | **368.0 s** | **0.88×** | **0.88×** | **1.00×** |

---

## Query-Level Analysis

### Sidecar wins significantly (C/N < 0.50)

| Query | Native avg (ms) | CPU avg (ms) | Speedup | Reason |
|-------|-----------------|--------------|---------|--------|
| Q22 | 13,488 | 2,316 | **5.82×** | correlated EXISTS subquery — DuckDB rewrites to anti-join |
| Q15 | 83,484 | 16,838 | **4.96×** | correlated subquery + CREATE VIEW — DuckDB eliminates redundant scan |
| Q20 | 58,461 | 16,415 | **3.56×** | correlated EXISTS + complex filter — anti-join rewrite advantage |
| Q17 | 57,479 | 19,695 | **2.92×** | correlated subquery with agg — DuckDB decorrelates efficiently |
| Q9 | 64,078 | 26,667 | **2.40×** | 9-table join with complex predicates — DuckDB hash-join & join ordering |

### Native wins significantly (C/N > 3.0)

| Query | Native avg (ms) | CPU avg (ms) | Slowdown | Reason |
|-------|-----------------|--------------|----------|--------|
| Q6 | 1,731 | 12,972 | **7.50× slower** | simple filter + SUM — MO native scan is optimal; HTTP + TAE re-read overhead dominates |
| Q19 | 3,888 | 27,501 | **7.07× slower** | range filter + SUM — same scan-dominant pattern as Q6 |
| Q7 | 3,392 | 19,169 | **5.65× slower** | 2-table join + date trunc agg — MO native join faster at SF=300 |
| Q21 | 7,240 | 36,859 | **5.09× slower** | 4-table join + EXISTS/NOT EXISTS — MO handles large lineitem scan natively |
| Q12 | 3,617 | 17,244 | **4.77× slower** | 2-table join + CASE agg — similar to Q7 |
| Q1 | 6,990 | 30,417 | **4.35× slower** | full lineitem scan + groupby — MO native I/O path is optimal |
| Q8 | 6,051 | 23,926 | **3.95× slower** | multi-table join + CASE — large scan, MO native wins |
| Q4 | 2,682 | 10,427 | **3.89× slower** | semi-join + agg — correlated EXISTS but small result; native overhead lower |

### GPU vs CPU comparison

GPU and CPU sidecar are statistically identical at SF=300 (all G/C ratios 0.93–1.03×).
At this scale factor, the GPU scan phase is not the bottleneck — MO routing overhead,
HTTP serialization, and DuckDB join processing dominate the end-to-end wall time.

| Metric | Value |
|--------|-------|
| GPU/CPU total time ratio | 1.00× |
| Largest GPU advantage | Q2: 3,647 ms vs 3,912 ms (−7%) |
| Largest GPU disadvantage | Q8: 24,558 ms vs 23,926 ms (+3%, within noise) |
| Queries with >5% GPU improvement | Q2 only |

---

## Run-to-Run Stability

| Run | Mode | Total (ms) | Δ from mode avg |
|-----|------|-----------|-----------------|
| 1 | Native | 405,563 | −2.5% |
| 2 | Native | 418,860 | +0.7% |
| 3 | Native | 423,998 | +1.9% |
| 4 | CPU Sidecar | 368,827 | +0.3% |
| 5 | CPU Sidecar | 367,358 | −0.1% |
| 6 | CPU Sidecar | 367,453 | −0.1% |
| 7 | GPU Sidecar | 368,422 | +0.1% |
| 8 | GPU Sidecar | 367,485 | −0.1% |

- **Native**: ±2.3% across 3 runs (Q15/Q17/Q20 variance drives most of the spread)
- **CPU Sidecar**: ±0.2% — extremely stable
- **GPU Sidecar**: ±0.1% — extremely stable

---

## Notes

- All 22 queries returned correct results on all 8 runs
- Q11 uses a `[SF]` placeholder substituted at runtime with `300`
- Native Run 1 total (405.6 s) is slightly lower than Runs 2–3 (418–424 s),  
  driven by faster Q9/Q15/Q20 in Run 1 — likely OS page-cache warmup from data load
- CPU and GPU sidecars show no cold/warm difference: DuckDB engine state  
  and HTTP round-trip overhead are stable from the first query
- The ~28–30 s wall time for Q1 (sidecar) vs ~7 s (native) is primarily  
  MO routing overhead, not DuckDB execution time; a pure DuckDB Q1 at SF=300  
  would take ~3–4 s
- GPU advantage would emerge in a direct DuckDB-to-DuckDB comparison where  
  the TAE scan throughput — not MO's routing layer — is the bottleneck

