#!/usr/bin/env bash
# Run the host build repeatedly, re-lifting whenever the run reaches code the
# lifter didn't know (recorded in extra_funcs.tsv), until a run stops for some
# other reason.
#
#   tools/discover.sh [max-rounds]
#
# Runs with RT_MISSING=zero, so unimplemented imports return 0 and the game
# gets as far as it can. The last run's log is build/run.log. Other RT_*
# settings pass through, so scripted taps (RT_TAPS) can take the runs further
# in; LOG_IMPORTS=0 leaves out the import log (which fills up while a level
# loads) and TIMEOUT sets the seconds per run (300).
set -uo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
rounds="${1:-10}"
extra="$here/extra_funcs.tsv"

lifted="$here/build/extra_funcs.lifted"   # the starts the current build was lifted with
for ((k = 1; k <= rounds; k++)); do
    log_imports=(RT_LOG_IMPORTS=1)
    [ "${LOG_IMPORTS:-1}" = 0 ] && log_imports=()
    (cd build && env RT_LOG_LIMIT="${RT_LOG_LIMIT:-80000}" RT_EXTRA_FUNCS="$extra" RT_MISSING=zero "${log_imports[@]}" \
        timeout "${TIMEOUT:-300}" ./codz-host ../orig/Payload/kiwi.app > run.log 2>&1)
    status=$?
    [ -f "$extra" ] && sort -u "$extra" -o "$extra"
    if cmp -s "$extra" "$lifted" || [ ! -f "$extra" ]; then
        echo "round $k: exit $status, no new function starts"
        break
    fi
    echo "round $k: exit $status, $(comm -13 "$lifted" "$extra" 2>/dev/null | wc -l | tr -d ' ') new function starts; re-lifting"
    cp "$extra" "$lifted"
    ./build.sh host --relift > build/build.log 2>&1 || { echo "build failed; see build/build.log"; exit 1; }
done
grep -v '^\[codz\] import' build/run.log | tail -12
