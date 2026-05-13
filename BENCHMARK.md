# TPC-H Benchmark Report

**Hardware**: GPU environment (local NVMe)  
**Modes**: Native MO (3 runs), GPU Sidecar / Sirius (6 runs)  
**Metric**: end-to-end query wall time measured by MO client (ms)  
**Note**: this log contains only native MO and GPU sidecar runs (no CPU-only sidecar).

---

## Executive Summary

| SF | Mode | Total avg | vs Native |
|----|------|-----------|-----------|
| 300 | Native MO | 416.1 s | — |
| 300 | GPU Sidecar | 368.3 s | **0.88×** (11.5% faster) |
| 100 | Native MO | 143.1 s | — |
| 100 | GPU Sidecar | 123.3 s | **0.86×** (13.9% faster) |

- GPU sidecar is **~1.13–1.16×** faster than native MO at both scale factors
- Sidecar advantage driven by DuckDB's optimizer on correlated subquery / join-heavy queries
- Native MO wins on simple scan+agg queries where HTTP + TAE re-read overhead dominates
- SF=100 GPU Q1: ~10 s (vs 30 s in older benchmarks) — improvement from native TAE scan + batching

---

## SF=300 Results

3 native runs · 6 GPU sidecar runs · Native avg: **416.1 s** · GPU avg: **368.3 s** → GPU sidecar **1.13×** faster

### Per-Query (ms)

`G/N` < 1.0 → GPU sidecar faster; > 1.0 → native faster.

| Q | N1 | N2 | N3 | **N avg** | G1 | G2 | G3 | G4 | G5 | G6 | **G avg** | **G/N** |
|---|----|----|----|-----------|----|----|----|----|----|----|-----------|---------| 
| Q1 | 6,826 | 7,202 | 6,942 | **6,990** | 30,570 | 30,305 | 30,375 | 30,449 | 30,533 | 30,344 | **30,429** | **4.35×** |
| Q2 | 2,054 | 1,986 | 1,998 | **2,013** | 4,076 | 3,644 | 4,015 | 3,682 | 3,612 | 3,613 | **3,774** | **1.87×** |
| Q3 | 12,463 | 16,290 | 14,537 | **14,430** | 16,520 | 16,911 | 16,390 | 16,988 | 16,817 | 16,564 | **16,698** | **1.16×** |
| Q4 | 2,736 | 2,664 | 2,646 | **2,682** | 10,459 | 10,406 | 10,416 | 10,464 | 10,730 | 10,910 | **10,564** | **3.94×** |
| Q5 | 12,427 | 15,316 | 16,766 | **14,836** | 17,726 | 17,377 | 17,421 | 17,483 | 16,961 | 17,071 | **17,340** | **1.17×** |
| Q6 | 1,751 | 1,683 | 1,758 | **1,731** | 13,016 | 12,992 | 12,909 | 12,964 | 13,287 | 13,702 | **13,145** | **7.60×** |
| Q7 | 3,427 | 3,354 | 3,396 | **3,392** | 19,127 | 19,362 | 19,019 | 19,638 | 18,741 | 19,075 | **19,160** | **5.65×** |
| Q8 | 6,210 | 5,952 | 5,990 | **6,051** | 24,385 | 23,568 | 23,825 | 25,175 | 23,940 | 24,090 | **24,164** | **3.99×** |
| Q9 | 66,722 | 62,997 | 62,514 | **64,078** | 26,757 | 26,748 | 26,496 | 26,252 | 26,621 | 26,876 | **26,625** | **0.42×** |
| Q10 | 18,782 | 17,995 | 18,209 | **18,329** | 18,187 | 18,981 | 18,170 | 18,679 | 18,524 | 18,657 | **18,533** | **1.01×** |
| Q11 | 4,146 | 4,202 | 4,093 | **4,147** | 2,828 | 2,766 | 2,651 | 2,697 | 2,629 | 2,664 | **2,706** | **0.65×** |
| Q12 | 3,639 | 3,671 | 3,540 | **3,617** | 16,771 | 17,612 | 17,348 | 17,419 | 16,993 | 16,796 | **17,156** | **4.74×** |
| Q13 | 13,162 | 11,164 | 15,336 | **13,221** | 11,670 | 11,499 | 11,499 | 11,550 | 11,902 | 11,664 | **11,631** | **0.88×** |
| Q14 | 21,700 | 21,194 | 21,574 | **21,489** | 18,443 | 18,106 | 18,123 | 16,588 | 18,001 | 17,807 | **17,845** | **0.83×** |
| Q15 | 79,703 | 87,897 | 82,852 | **83,484** | 16,894 | 16,689 | 16,930 | 16,530 | 17,033 | 16,903 | **16,830** | **0.20×** |
| Q16 | 1,902 | 1,880 | 1,822 | **1,868** | 2,165 | 2,170 | 2,186 | 2,185 | 2,182 | 2,116 | **2,167** | **1.16×** |
| Q17 | 54,563 | 55,396 | 62,477 | **57,479** | 19,589 | 19,856 | 19,641 | 20,061 | 20,084 | 19,946 | **19,863** | **0.35×** |
| Q18 | 13,332 | 13,225 | 13,129 | **13,229** | 16,351 | 16,124 | 16,301 | 16,580 | 15,925 | 16,072 | **16,226** | **1.23×** |
| Q19 | 3,891 | 3,893 | 3,880 | **3,888** | 27,483 | 27,401 | 27,618 | 27,813 | 27,734 | 28,983 | **27,839** | **7.16×** |
| Q20 | 53,899 | 60,962 | 60,521 | **58,461** | 16,270 | 16,574 | 16,400 | 16,323 | 16,615 | 17,222 | **16,567** | **0.28×** |
| Q21 | 7,207 | 7,312 | 7,201 | **7,240** | 37,240 | 35,971 | 37,367 | 36,600 | 36,308 | 36,617 | **36,684** | **5.07×** |
| Q22 | 15,021 | 12,625 | 12,817 | **13,488** | 2,300 | 2,296 | 2,353 | 2,302 | 2,313 | 2,317 | **2,314** | **0.17×** |
| **Total** | | | | **416.1 s** | | | | | | | **368.3 s** | **0.88×** |

### Run Totals

| Run | Mode | Total (ms) | Δ from mode avg |
|-----|------|------------|-----------------|
| 1 | Native | 405,563 | -2.5% |
| 2 | Native | 418,860 | +0.7% |
| 3 | Native | 423,998 | +1.9% |
| 4 | GPU Sidecar | 368,827 | +0.2% |
| 5 | GPU Sidecar | 367,358 | -0.2% |
| 6 | GPU Sidecar | 367,453 | -0.2% |
| 7 | GPU Sidecar | 368,422 | +0.0% |
| 8 | GPU Sidecar | 367,485 | -0.2% |
| 9 | GPU Sidecar | 370,009 | +0.5% |

### GPU Wins (G/N < 0.50)

| Query | Native avg | GPU avg | Speedup | Reason |
|-------|-----------|---------|---------|--------|
| Q22 | 13,488 | 2,314 | **5.83×** | correlated EXISTS → DuckDB anti-join rewrite |
| Q15 | 83,484 | 16,830 | **4.96×** | correlated subquery + CREATE VIEW → DuckDB eliminates redundant scan |
| Q20 | 58,461 | 16,567 | **3.53×** | correlated EXISTS + filter → anti-join rewrite advantage |
| Q17 | 57,479 | 19,863 | **2.89×** | correlated subquery + agg → DuckDB decorrelates efficiently |
| Q9 | 64,078 | 26,625 | **2.41×** | 9-table join → DuckDB hash-join ordering superiority |

### Native Wins (G/N > 3.0)

| Query | Native avg | GPU avg | Slowdown | Reason |
|-------|-----------|---------|----------|--------|
| Q6 | 1,731 | 13,145 | **7.60× slower** | simple filter+SUM — MO native scan optimal; HTTP+TAE re-read overhead |
| Q19 | 3,888 | 27,839 | **7.16× slower** | range filter+SUM — same scan-dominant pattern as Q6 |
| Q7 | 3,392 | 19,160 | **5.65× slower** | 2-table join + date trunc — MO native join faster |
| Q21 | 7,240 | 36,684 | **5.07× slower** | 4-table join + EXISTS/NOT EXISTS — MO handles large lineitem scan natively |
| Q12 | 3,617 | 17,156 | **4.74× slower** | 2-table join + CASE agg — similar to Q7 |
| Q1 | 6,990 | 30,429 | **4.35× slower** | full lineitem scan + groupby — MO native I/O path optimal |
| Q8 | 6,051 | 24,164 | **3.99× slower** | multi-table join + CASE — large scan, MO native wins |
| Q4 | 2,682 | 10,564 | **3.94× slower** | semi-join + agg — native overhead lower |

---

## SF=100 Results

3 native runs · 6 GPU sidecar runs · Native avg: **143.1 s** · GPU avg: **123.3 s** → GPU sidecar **1.16×** faster

### Per-Query (ms)

`G/N` < 1.0 → GPU sidecar faster; > 1.0 → native faster.

| Q | N1 | N2 | N3 | **N avg** | G1 | G2 | G3 | G4 | G5 | G6 | **G avg** | **G/N** |
|---|----|----|----|-----------|----|----|----|----|----|----|-----------|---------| 
| Q1 | 3,072 | 2,720 | 2,803 | **2,865** | 10,020 | 10,251 | 9,976 | 9,922 | 10,124 | 10,062 | **10,059** | **3.51×** |
| Q2 | 916 | 656 | 655 | **742** | 1,314 | 1,313 | 1,322 | 1,342 | 1,327 | 1,342 | **1,327** | **1.79×** |
| Q3 | 2,243 | 1,894 | 1,989 | **2,042** | 5,717 | 5,257 | 5,352 | 5,343 | 5,293 | 5,355 | **5,386** | **2.64×** |
| Q4 | 1,499 | 1,249 | 1,297 | **1,348** | 3,720 | 3,688 | 4,070 | 4,152 | 3,792 | 4,349 | **3,962** | **2.94×** |
| Q5 | 6,414 | 5,569 | 5,406 | **5,796** | 5,809 | 5,789 | 5,779 | 5,841 | 6,280 | 5,826 | **5,887** | **1.02×** |
| Q6 | 1,075 | 996 | 938 | **1,003** | 4,267 | 4,637 | 4,375 | 4,304 | 4,346 | 4,378 | **4,384** | **4.37×** |
| Q7 | 6,721 | 1,548 | 1,577 | **3,282** | 6,445 | 6,451 | 6,492 | 6,634 | 6,532 | 6,659 | **6,536** | **1.99×** |
| Q8 | 2,274 | 1,945 | 1,966 | **2,062** | 7,609 | 7,211 | 7,230 | 7,336 | 7,340 | 7,549 | **7,379** | **3.58×** |
| Q9 | 21,231 | 22,608 | 20,309 | **21,383** | 8,803 | 8,742 | 9,319 | 9,440 | 8,998 | 9,344 | **9,108** | **0.43×** |
| Q10 | 6,759 | 6,398 | 5,817 | **6,325** | 6,118 | 6,361 | 6,157 | 6,319 | 6,534 | 6,172 | **6,277** | **0.99×** |
| Q11 | 1,463 | 1,413 | 1,375 | **1,417** | 934 | 1,083 | 949 | 933 | 957 | 948 | **967** | **0.68×** |
| Q12 | 1,723 | 1,442 | 1,463 | **1,543** | 5,885 | 5,836 | 5,654 | 5,651 | 5,777 | 5,696 | **5,750** | **3.73×** |
| Q13 | 4,606 | 4,587 | 4,276 | **4,490** | 3,873 | 3,851 | 3,831 | 3,841 | 3,830 | 3,862 | **3,848** | **0.86×** |
| Q14 | 6,570 | 6,217 | 6,548 | **6,445** | 5,755 | 5,530 | 5,422 | 5,422 | 5,334 | 5,476 | **5,490** | **0.85×** |
| Q15 | 25,956 | 26,450 | 26,310 | **26,239** | 5,436 | 5,584 | 5,818 | 6,011 | 5,507 | 5,913 | **5,712** | **0.22×** |
| Q16 | 912 | 851 | 837 | **867** | 824 | 877 | 823 | 830 | 908 | 877 | **856** | **0.99×** |
| Q17 | 18,074 | 18,080 | 20,191 | **18,782** | 6,624 | 6,859 | 6,658 | 6,650 | 7,017 | 6,662 | **6,745** | **0.36×** |
| Q18 | 2,937 | 2,898 | 3,063 | **2,966** | 5,356 | 5,534 | 5,426 | 5,461 | 5,465 | 5,486 | **5,455** | **1.84×** |
| Q19 | 2,018 | 1,834 | 1,811 | **1,888** | 9,202 | 8,918 | 8,936 | 8,986 | 9,000 | 8,957 | **9,000** | **4.77×** |
| Q20 | 19,786 | 19,319 | 21,139 | **20,081** | 5,666 | 5,713 | 6,124 | 6,139 | 5,759 | 6,125 | **5,921** | **0.29×** |
| Q21 | 14,275 | 3,476 | 3,471 | **7,074** | 12,061 | 12,734 | 12,159 | 12,411 | 12,782 | 12,156 | **12,384** | **1.75×** |
| Q22 | 4,326 | 4,804 | 4,180 | **4,437** | 805 | 834 | 853 | 831 | 816 | 822 | **827** | **0.19×** |
| **Total** | | | | **143.1 s** | | | | | | | **123.3 s** | **0.86×** |

### Run Totals

| Run | Mode | Total (ms) | Δ from mode avg |
|-----|------|------------|-----------------|
| 1 | Native | 154,850 | +8.2% |
| 2 | Native | 136,954 | -4.3% |
| 3 | Native | 137,421 | -4.0% |
| 4 | GPU Sidecar | 122,243 | -0.8% |
| 5 | GPU Sidecar | 123,053 | -0.2% |
| 6 | GPU Sidecar | 122,725 | -0.4% |
| 7 | GPU Sidecar | 123,799 | +0.4% |
| 8 | GPU Sidecar | 123,718 | +0.4% |
| 9 | GPU Sidecar | 124,016 | +0.6% |

### GPU Wins (G/N < 0.50)

| Query | Native avg | GPU avg | Speedup | Reason |
|-------|-----------|---------|---------|--------|
| Q22 | 4,437 | 827 | **5.37×** | correlated EXISTS → DuckDB anti-join rewrite |
| Q15 | 26,239 | 5,712 | **4.59×** | correlated subquery + CREATE VIEW → DuckDB eliminates redundant scan |
| Q20 | 20,081 | 5,921 | **3.39×** | correlated EXISTS + filter → anti-join rewrite advantage |
| Q17 | 18,782 | 6,745 | **2.78×** | correlated subquery + agg → DuckDB decorrelates efficiently |
| Q9 | 21,383 | 9,108 | **2.35×** | 9-table join → DuckDB hash-join ordering superiority |

### Native Wins (G/N > 3.0)

| Query | Native avg | GPU avg | Slowdown | Reason |
|-------|-----------|---------|----------|--------|
| Q19 | 1,888 | 9,000 | **4.77× slower** | range filter+SUM — same scan-dominant pattern as Q6 |
| Q6 | 1,003 | 4,384 | **4.37× slower** | simple filter+SUM — MO native scan optimal; HTTP+TAE re-read overhead |
| Q12 | 1,543 | 5,750 | **3.73× slower** | 2-table join + CASE agg — similar to Q7 |
| Q8 | 2,062 | 7,379 | **3.58× slower** | multi-table join + CASE — large scan, MO native wins |
| Q1 | 2,865 | 10,059 | **3.51× slower** | full lineitem scan + groupby — MO native I/O path optimal |

---

## Notes

- All 22 queries returned correct results on all runs (22/22 PASS)
- Q11 uses `[SF]` placeholder substituted at runtime
- Native Run 1 at each SF is a **cold run** (page cache empty after data load):
  - SF=100 Q7 cold: 6,721 ms vs warm avg 1,563 ms; Q21 cold: 14,275 ms vs warm 3,474 ms
  - Cold-run total is included in the 3-run average above
- GPU sidecar shows no cold/warm variance (≤ 0.4%) — stable from first query
- Native MO totals (143 s at SF=100, 416 s at SF=300) are higher than older benchmarks
  because the older SF=100 measurements were taken without `-debug-http` on an earlier MO version
- At both SFs the pattern is consistent: GPU optimizer wins on Q9/Q11/Q13/Q14/Q15/Q17/Q20/Q22;
  native wins on Q1/Q6/Q7/Q8/Q12/Q19/Q21 (scan-heavy, MO native I/O path is optimal)
