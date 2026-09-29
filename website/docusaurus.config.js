// @ts-check
// Docusaurus is only a viewer: the Markdown in docs/ and the repository root
// stays the source of truth and must read correctly on GitHub (ADR-0017).
const path = require('path');
const remarkRepoLinks = require('./src/remark-repo-links');

const repoRoot = path.resolve(__dirname, '..');
const repoUrl = 'https://github.com/Tomek52/lockstep-exchange';

// One docs instance rooted at the repository, not at ../docs: markdown links
// resolve only within a single plugin instance, and README/ROADMAP/CLAUDE link
// into docs/ (and back). A second instance rooted at '..' would also overlap
// ../docs and grab its files. See ADR-0017, "Alternatives considered".
const include = ['docs/**/*.md', 'README.md', 'ROADMAP.md', 'CLAUDE.md'];
const isSiteDoc = (relPath) =>
  relPath.endsWith('.md') &&
  (relPath.startsWith('docs/') || include.includes(relPath));

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Lockstep',
  tagline: 'Deterministic exchange engine: architecture, decisions, tasks',
  url: 'https://tomek52.github.io',
  baseUrl: '/',
  trailingSlash: false,

  // Broken links, anchors and markdown links fail the build: this is the
  // mechanism that detects stale references to versioned anchors (ADR-0017).
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',

  markdown: {
    // .md is parsed as CommonMark, not MDX, so `<` and `{` in prose do not
    // break the build. Only .mdx files would be MDX, and we have none.
    format: 'detect',
    mermaid: true,
    hooks: {
      onBrokenMarkdownLinks: 'throw',
      onBrokenMarkdownImages: 'throw',
    },
  },

  themes: ['@docusaurus/theme-mermaid'],

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          path: '..',
          include,
          routeBasePath: '/',
          sidebarPath: require.resolve('./sidebars.js'),
          // Keep URLs equal to file names (0013-risk-feedback-loop, not
          // risk-feedback-loop), so a URL maps back to a file at a glance.
          numberPrefixParser: false,
          editUrl: `${repoUrl}/edit/main/`,
          beforeDefaultRemarkPlugins: [
            [remarkRepoLinks, {repoRoot, repoUrl, branch: 'main', isSiteDoc}],
          ],
        },
        blog: false,
        pages: false,
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      navbar: {
        title: 'Lockstep',
        items: [
          {type: 'doc', docId: 'docs/architecture/README', label: 'Architecture'},
          {type: 'doc', docId: 'docs/adr/README', label: 'ADRs'},
          {type: 'doc', docId: 'docs/tasks/README', label: 'Tasks'},
          {type: 'doc', docId: 'ROADMAP', label: 'Roadmap'},
          {href: repoUrl, label: 'GitHub', position: 'right'},
        ],
      },
      mermaid: {
        theme: {light: 'default', dark: 'dark'},
      },
    }),
};

module.exports = config;
