#!/usr/bin/env bash
# Runs the paper's grid (distributions x sizes) and appends CSV rows to results/<name>.csv.
# Resume completed algorithm/size combinations. Cache external sorts separately.
#
#   scripts/sweep.sh NAME [synthetic|sosd|robust|all] [max_exponent=8] [runs=tiered]
set -euo pipefail
cd "$(dirname "$0")/.."

name=${1:?usage: sweep.sh NAME [synthetic|sosd|robust|all] [max_exponent] [runs]}
set_=${2:-all}
max_exp=${3:-8}
runs=${4:-tiered}
algos=${ALGOS:-nfss,nfss_base}
binary=${NFSBENCH:-build/nfsbench}

synthetic=(uniform sorted reverse organpipe normal nearlysorted fewunique exponential clustered
           sosd_lognormal sosd_normal sosd_dense sosd_sparse)
sosd=(books fb osm wiki)
robust=(dup300 dup1000 dup10000 organpipe_overlap organpipe_mirror runs2 runs16 runs1000keys
        tail1 tail10 tail40 nearlysorted5 heavytail sentinel span1e19 span1p3e12 signednormal)
case $set_ in
  synthetic) dists=("${synthetic[@]}") ;;
  sosd) dists=("${sosd[@]}") ;;
  robust) dists=("${robust[@]}") ;;
  all) dists=("${synthetic[@]}" "${sosd[@]}" "${robust[@]}") ;;
  *) echo "unknown set $set_" >&2; exit 2 ;;
esac

if [[ $name == check ]]; then
  printf '%s\0' "${dists[@]}" | xargs -0 -P 12 -n 1 "$binary" --check --algos "$algos" \
    --type "${TYPE:-u64,i64,u32,f64}" --n "${CHECK_SIZES:-1,2,17,1000}" --runs 1 --dist
  edge_algos=()
  for candidate in lowmem2 classic5; do
    if [[ ,$algos, == *,"$candidate",* ]]; then edge_algos+=("$candidate"); fi
  done
  if (( ${#edge_algos[@]} )); then
    selected=$(IFS=,; echo "${edge_algos[*]}")
    for dist in edge_equal edge_two edge_top edge_bottom; do
      "$binary" --check --algos "$selected" --type "${TYPE:-u64,i64,u32,f64}" --n "${CHECK_SIZES:-1,2,17,1000}" --dist "$dist"
    done
    if [[ ,${TYPE:-u64,i64,u32,f64}, == *,f64,* ]]; then
      for dist in edge_wide edge_denorm edge_ieee edge_nan; do
        "$binary" --check --algos "$selected" --type f64 --n "${CHECK_SIZES:-1,2,17,1000}" --dist "$dist"
      done
    fi
  fi
  exit
fi

mkdir -p results build
python3 scripts/sweep.py "$name" "$binary" "$algos" "${TIER:-full}" "$max_exp" "$runs" "${TYPE:-}" "${dists[@]}"
