---
name: cyberclip-deploy
description: Ship Cyberclip to production (https://cyberclip.vercel.app) by publishing a GitHub release, roll production back, and move the shipped tasks to "deployed/done" on the ClickUp development board. Use when the user asks to deploy, release, ship to production, roll back, or tidy the ClickUp board after a deploy.
---

# Deploying Cyberclip

## How environments work

- **Staging** is main's Vercel preview. Merging a PR into main deploys it there. A PR that changes firmware gets a "Build firmware X.Y.Z" commit from CI (`.github/workflows/firmware.yml`) before it merges. Versions count releases, not merges: until `vX.Y.Z` is released, every firmware PR rebuilds X.Y.Z, and the first one after the release takes X.Y.(Z+1).
- **Production** is the `production` branch. Vercel deploys it to https://cyberclip.vercel.app. Nobody pushes to it by hand: publishing a GitHub release runs `.github/workflows/release.yml`, which force-pushes the tagged commit to `production`.
- The release workflow rejects a release unless all of these hold:
  - the tag is `v<version>`, where `<version>` is `version` in `public/firmware/manifest.json` at the tagged commit, or `v<version>-web.<n>` for web-only changes, which is allowed only once `v<version>` itself is released,
  - the tag is on main (it has been on staging),
  - the release is not a pre-release (pre-releases stay on staging).

A firmware run that fails right after a passing one usually ran on CI's own "Build firmware" commit. That failure is expected. Look at the run on the PR's real head before calling a build broken.

## Release steps

Publishing changes the live site, so confirm the tag and the changes with the user before step 4.

1. `git fetch --tags` and read the version: `git show origin/main:public/firmware/manifest.json | jq -r .version`.
2. Pick the tag from `gh release list`:
   - `v<version>` doesn't exist yet: the release carries new firmware, so tag it `v<version>`.
   - `v<version>` exists: firmware is unchanged since that release, so this is a web-only release. Tag it `v<version>-web.<n>`, where `<n>` is one more than the highest existing `-web.` tag for this version (start at 1).
3. Collect what ships since the last release: `git log --oneline <last tag>..origin/main`, or all of main if this is the first release. If nothing is listed, there's nothing to ship. Tell the user rather than tagging.
4. Publish. Pass `--latest`, so GitHub marks it latest even though semver sorts `-web.` tags below the plain tag:
   ```bash
   gh release create <tag> --target main --title "<tag>" --latest --notes "<one line per merged PR>"
   ```
5. Watch the Release workflow: `gh run list --workflow release.yml -L 1`, then `gh run watch <id>`. Confirm `production` now points at the tag: `gh api repos/{owner}/{repo}/branches/production --jq .commit.sha`.
6. Then sort ClickUp (below).

**Rollback:** `gh workflow run release.yml -f tag=v<older version>`. This points production at the older tag.

## Sorting ClickUp after a deploy

The board is the list "Development (Firmware/Software)", list id `1100370000184672`, in space `1100370000137479`. The statuses come from the space and run in this order:

to do → in progress → ready for deployment → **deployed/done** (closed)

1. Get the list's tasks in "ready for deployment" with `clickup_filter_tasks` (`list_ids`, `statuses`).
2. Match each task to a commit or PR in the release by its title. Task titles and PR titles are written the same way.
3. Move the matched tasks in one call: `clickup_execute_operator` with `task.update_many`, `status_label: "deployed/done"`.
4. Leave any task that didn't ship where it is, and tell the user which ones.
5. Optional: move a task to "ready for deployment" once its PR merges to main (staging), so the column always shows what the next release will carry.

Don't rename or reorder the space statuses during a deploy. They're shared across the space.
