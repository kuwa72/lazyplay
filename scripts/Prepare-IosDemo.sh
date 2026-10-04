#!/usr/bin/env bash
set -euo pipefail
if (( $# < 2 )); then
    printf 'Usage: bash scripts/Prepare-IosDemo.sh SOURCE.mp4 OUTPUT.gif [OUTPUT.gif ...]\n' >&2
    exit 1
fi
source_file=$1
shift
for output_file in "$@"; do
    if [[ ! -d "$(dirname "$output_file")" || "$output_file" != *.gif ]]; then
        printf 'Destination must be a .gif in an existing directory: %s\n' "$output_file" >&2
        exit 1
    fi
    ffmpeg -v error -n -ss 6 -i "$source_file" -t 12 -an -vf 'fps=6,scale=640:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=48:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle' "$output_file"
    size=$(stat -c '%s' "$output_file")
    printf '%s: %s bytes\n' "$output_file" "$size"
    if (( size > 2500000 )); then
        printf 'Asset exceeds the 2,500,000-byte safety limit.\n' >&2
        exit 1
    fi
done
