#!/usr/bin/env bash
# Collect pixi-provided shared libraries needed by the sidecar binary.
# Uses ldd recursively to capture transitive dependencies from CONDA_PREFIX.
set -euo pipefail

binary="$1"
outdir="$2"
mkdir -p "$outdir"

collect() {
    ldd "$1" 2>/dev/null | grep "${CONDA_PREFIX}" | awk '{print $3}' | sort -u || true
}

# Seed with direct deps.
libs=$(collect "$binary")
prev=""

# Iterate until no new transitive deps are found.
while [ "$libs" != "$prev" ]; do
    prev="$libs"
    for f in $libs; do
        cp -aLn "$f" "$outdir/" 2>/dev/null || true
    done
    transitive=""
    for f in "$outdir"/*.so*; do
        transitive="$transitive $(collect "$f")"
    done
    libs=$(echo "$libs $transitive" | tr ' ' '\n' | sort -u | tr '\n' ' ')
done

# Strip debug info from collected libs.
strip --strip-unneeded "$outdir"/*.so* 2>/dev/null || true

echo "Collected $(ls "$outdir" | wc -l) libraries into $outdir"
