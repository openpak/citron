// Attaches files to the release of the tag being built, for actions/github-script:
//   await require(`${process.env.GITHUB_WORKSPACE}/.github/scripts/attach-to-release.js`)({ github, context, glob, core }, 'dist/*.AppImage*\ndist/*.tar.zst');
// Only release.yml creates the release. The build jobs of several workflows start on the same
// tag, and when each of them created it as well, two releases for one tag came out of the race
// (v0.1.0, 2026-09-23). Here a build waits for the release to exist, then uploads by its id,
// replacing a file of the same name (a re-run).
const fs = require('fs');
const path = require('path');

module.exports = async ({ github, context, glob, core }, patterns) => {
  const { owner, repo } = context.repo;
  const tag = context.ref.replace('refs/tags/', '');
  let release;
  for (let attempt = 0; attempt < 60; attempt++) {
    try {
      release = (await github.rest.repos.getReleaseByTag({ owner, repo, tag })).data;
      break;
    } catch (e) {
      if (e.status !== 404) throw e;
      core.info(`No release for ${tag} yet; waiting for release.yml`);
      await new Promise((r) => setTimeout(r, 30000));
    }
  }
  if (!release) throw new Error(`No release for ${tag} after 30 minutes`);

  const files = await (await glob.create(patterns)).glob();
  if (files.length === 0) throw new Error(`Nothing matches ${patterns}`);
  for (const file of files) {
    if (fs.statSync(file).isDirectory()) continue;
    const name = path.basename(file);
    const assets = await github.paginate(github.rest.repos.listReleaseAssets, { owner, repo, release_id: release.id });
    for (const old of assets.filter((a) => a.name === name)) {
      await github.rest.repos.deleteReleaseAsset({ owner, repo, asset_id: old.id });
    }
    core.info(`Uploading ${name} to ${tag}`);
    await github.rest.repos.uploadReleaseAsset({
      owner, repo, release_id: release.id, name,
      data: fs.readFileSync(file),
      headers: { 'content-type': 'application/octet-stream', 'content-length': fs.statSync(file).size },
    });
  }
};
