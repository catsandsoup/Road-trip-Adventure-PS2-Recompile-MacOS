#!/bin/bash
# Pre-push safety audit: fail if a repo's tracked files include disc-derived or local-only content.
# Usage: game/tools/push_audit.sh <repo dir>
set -u
cd "$1" || exit 2
bad=$(git ls-files | grep -Eiv '^$' | grep -E '\.(iso|bin|elf|irx|wav|ram|vu1|vu1code|vu1data|png|jpg|jpeg|pyc|csv|gsl|cue)$|SLES_|(^|/)(work|disc|share)/|HANDOFF\.md$|CHANGELOG\.md$|__pycache__|\.DS_Store')
big=$(git ls-files -z | xargs -0 -I{} find {} -size +2000k 2>/dev/null | grep -v sce_symbol_database_data.h)
paths=$(git ls-files -z | xargs -0 grep -IlE '/Users/[a-z]+|ghp_[A-Za-z0-9]{20}|BEGIN (RSA|OPENSSH|PRIVATE)' 2>/dev/null)
if [ -n "$bad$big$paths" ]; then echo "AUDIT FAIL"; echo "$bad"; echo "$big"; echo "$paths"; exit 1; fi
echo "AUDIT OK ($(git ls-files | wc -l) files)"
