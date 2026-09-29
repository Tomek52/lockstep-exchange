// Rewrites relative links to repository files that are not pages of the docs
// site (source code, LICENSE, directories) into GitHub URLs.
//
// Why: the Markdown sources keep plain relative links, which work in GitHub's
// file preview and in an editor. Left as-is, Docusaurus would treat them as
// site routes and fail the build with onBrokenLinks. A link to a file that
// does not exist is left untouched, so it still fails the build (ADR-0017).
const fs = require('fs');
const path = require('path');

const hasScheme = /^[a-z][a-z0-9+.-]*:/i;

module.exports = function remarkRepoLinks({repoRoot, repoUrl, branch, isSiteDoc}) {
  const rewrite = (node, filePath) => {
    const url = node.url;
    if (!url || hasScheme.test(url) || url.startsWith('#') || url.startsWith('/')) {
      return;
    }
    const [target, hash] = url.split('#');
    const absolute = path.resolve(path.dirname(filePath), decodeURI(target));
    const relative = path.relative(repoRoot, absolute).split(path.sep).join('/');
    if (relative.startsWith('..') || !fs.existsSync(absolute) || isSiteDoc(relative)) {
      return;
    }
    const kind = fs.statSync(absolute).isDirectory() ? 'tree' : 'blob';
    node.url = `${repoUrl}/${kind}/${branch}/${relative}${hash ? `#${hash}` : ''}`;
  };

  const visit = (node, filePath) => {
    if (node.type === 'link' || node.type === 'definition') {
      rewrite(node, filePath);
    }
    for (const child of node.children ?? []) {
      visit(child, filePath);
    }
  };

  return (tree, file) => visit(tree, file.path);
};
