#!/usr/bin/env bash
# Apply the GitHub repository settings this project relies on (idempotent; needs `gh` authenticated
# as a repo admin). Rulesets on a private repository need a paid plan; on a public one they are free.
#
#   Actions     workflows get a read-only GITHUB_TOKEN and can't approve PRs; workflows from every
#               outside contributor's fork PR wait for an admin's approval; only GitHub-owned,
#               verified-creator and explicitly listed actions may run.
#   merging     squash only; head branches are deleted after merge.
#   main        no deletion or force-push, linear history, changes through a PR with one approving
#               review from a code owner (.github/CODEOWNERS), resolved conversations, and passing
#               "Linux (host)" (ci.yml) and "TruffleHog" (secrets.yml) checks. Admins may bypass.
#   v* tags     only admins can create, move or delete them, so only admins can publish a release
#               (release.yml runs on tag push).
#
#   scripts/repo-settings.sh [owner/repo]
set -euo pipefail
REPO="${1:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"
ADMIN_ROLE=5          # built-in "admin" repository role
ACTIONS_APP=15368     # GitHub Actions, the source of the required checks

gh api -X PATCH "repos/$REPO" -F delete_branch_on_merge=true -F allow_merge_commit=false -F allow_rebase_merge=false >/dev/null
gh api -X PUT "repos/$REPO/actions/permissions/workflow" \
  -f default_workflow_permissions=read -F can_approve_pull_request_reviews=false
gh api -X PUT "repos/$REPO/actions/permissions" -F enabled=true -f allowed_actions=selected
gh api -X PUT "repos/$REPO/actions/permissions/selected-actions" --input - <<'JSON'
{"github_owned_allowed": true, "verified_allowed": true,
 "patterns_allowed": ["softprops/action-gh-release@*"]}
JSON
PRIVATE=$(gh api "repos/$REPO" --jq .private)

# GitHub only accepts a fork-PR approval policy on public repositories.
if [ "$PRIVATE" = false ]; then
  gh api -X PUT "repos/$REPO/actions/permissions/fork-pr-contributor-approval" \
    -f approval_policy=all_external_contributors
else
  echo "fork-PR approval: skipped (private repository; re-run after making it public)"
fi

# Rulesets on a private repository need GitHub Pro; stop here and apply them once it is public.
if ! gh api "repos/$REPO/rulesets" >/dev/null 2>&1; then
  echo "rulesets: skipped (not available on this repository; re-run after making it public)"
  exit 0
fi

# Create or update a ruleset by name from JSON on stdin.
ruleset() {
  local name=$1 body id
  body=$(cat)
  id=$(gh api "repos/$REPO/rulesets" --jq ".[] | select(.name == \"$name\") | .id")
  if [ -n "$id" ]; then
    gh api -X PUT "repos/$REPO/rulesets/$id" --input - <<<"$body" >/dev/null
  else
    gh api -X POST "repos/$REPO/rulesets" --input - <<<"$body" >/dev/null
  fi
  echo "ruleset: $name"
}

ruleset main <<JSON
{"name": "main", "target": "branch", "enforcement": "active",
 "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
 "bypass_actors": [{"actor_id": $ADMIN_ROLE, "actor_type": "RepositoryRole", "bypass_mode": "always"}],
 "rules": [
   {"type": "deletion"},
   {"type": "non_fast_forward"},
   {"type": "required_linear_history"},
   {"type": "pull_request", "parameters": {
     "required_approving_review_count": 1, "require_code_owner_review": true,
     "dismiss_stale_reviews_on_push": true, "require_last_push_approval": false,
     "required_review_thread_resolution": true}},
   {"type": "required_status_checks", "parameters": {
     "strict_required_status_checks_policy": true,
     "required_status_checks": [
       {"context": "Linux (host)", "integration_id": $ACTIONS_APP},
       {"context": "TruffleHog", "integration_id": $ACTIONS_APP}]}}
 ]}
JSON

ruleset "release tags" <<JSON
{"name": "release tags", "target": "tag", "enforcement": "active",
 "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
 "bypass_actors": [{"actor_id": $ADMIN_ROLE, "actor_type": "RepositoryRole", "bypass_mode": "always"}],
 "rules": [{"type": "creation"}, {"type": "update"}, {"type": "deletion"}, {"type": "non_fast_forward"}]}
JSON

echo "settings applied to $REPO"
