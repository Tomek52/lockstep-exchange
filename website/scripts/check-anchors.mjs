#!/usr/bin/env node
// Lints versioned anchors in the Markdown docs (ADR-0017). Runs before the
// Docusaurus build because the build reports broken anchors by route, while
// this reports file:line and which version the anchor moved to. It also
// checks what the build cannot: id format, uniqueness, and that the visible
// [id vN] marker matches the anchor.
//
// Usage: node website/scripts/check-anchors.mjs   (from anywhere; no deps)
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// Keep in sync with `include` in website/docusaurus.config.js.
const rootDocs = ['README.md', 'ROADMAP.md', 'CLAUDE.md'];
const walk = (dir) =>
  fs.readdirSync(path.join(repoRoot, dir), {withFileTypes: true}).flatMap((e) => {
    const rel = `${dir}/${e.name}`;
    if (e.isDirectory()) return walk(rel);
    return e.name.endsWith('.md') ? [rel] : [];
  });
const files = [...rootDocs, ...walk('docs')].sort();

const idPattern = /^(t\d{3}|adr\d{4}|arch)-[a-z0-9]+(?:-[a-z0-9]+)*-v[1-9]\d*$/;
const versioned = /^(.*)-v([1-9]\d*)$/;
const anchorRe = /<a id="([^"]+)"><\/a>/g;
const linkRe = /\]\(([^)\s]*)#([^)\s]+)\)/g;

const errors = [];
const report = (file, line, msg) => errors.push(`${file}:${line}: ${msg}`);

// Lines outside fenced code blocks, with their 1-based numbers. Inline code
// spans are blanked: an `<a id>` or link shown as code is an example, not an
// anchor or a reference.
const proseLines = (text) => {
  let fenced = false;
  return text.split('\n').flatMap((line, i) => {
    if (/^\s*(```|~~~)/.test(line)) {
      fenced = !fenced;
      return [];
    }
    return fenced ? [] : [{line: line.replace(/`[^`]*`/g, '``'), n: i + 1}];
  });
};

// Which prefix a file may define: t004 in docs/tasks/004-*, adr0013 in
// docs/adr/0013-*, arch in docs/architecture/.
const allowedPrefix = (file) => {
  let m = file.match(/^docs\/tasks\/(\d{3})-/);
  if (m) return `t${m[1]}`;
  m = file.match(/^docs\/adr\/(\d{4})-/);
  if (m) return `adr${m[1]}`;
  return file.startsWith('docs/architecture/') ? 'arch' : null;
};

// Pass 1: collect and validate anchors.
const anchors = new Map(); // file -> Map(id -> line)
const byBase = new Map(); // base -> [{file, line, id}]
for (const file of files) {
  const text = fs.readFileSync(path.join(repoRoot, file), 'utf8');
  const ids = new Map();
  for (const {line, n} of proseLines(text)) {
    for (const m of line.matchAll(anchorRe)) {
      const id = m[1];
      if (ids.has(id)) report(file, n, `duplicate anchor "${id}" (first on line ${ids.get(id)})`);
      ids.set(id, n);
      if (!idPattern.test(id)) {
        report(file, n, `anchor "${id}" does not match <tNNN|adrNNNN|arch>-<descriptive-words>-vN`);
        continue;
      }
      const prefix = id.split('-')[0];
      if (prefix !== allowedPrefix(file)) {
        report(file, n, `anchor "${id}" has prefix "${prefix}", expected "${allowedPrefix(file) ?? '(none: this file may not define versioned anchors)'}"`);
      }
      const [, base, version] = id.match(versioned);
      if (prefix.startsWith('t')) {
        const marker = `<a id="${id}"></a>**[${base} v${version}]**`;
        if (!line.includes(marker)) {
          report(file, n, `anchor "${id}" must be followed directly by the marker **[${base} v${version}]**`);
        }
      }
      byBase.set(base, [...(byBase.get(base) ?? []), {file, n, id}]);
    }
  }
  anchors.set(file, ids);

  // Tasks that use anchors must anchor every top-level acceptance criterion.
  if (file.startsWith('docs/tasks/') && [...ids.keys()].some((id) => /^t\d{3}-/.test(id))) {
    const lines = proseLines(text);
    const start = lines.findIndex((l) => l.line === '## Acceptance criteria');
    const end = lines.findIndex((l, i) => i > start && l.line.startsWith('## '));
    for (const {line, n} of lines.slice(start + 1, end)) {
      if (/^\d+\. /.test(line) && !/^\d+\. <a id="t\d{3}-/.test(line)) {
        report(file, n, 'acceptance criterion without a versioned anchor');
      }
    }
  }
}
for (const [base, defs] of byBase) {
  if (defs.length > 1) {
    for (const d of defs) report(d.file, d.n, `"${base}" is defined more than once (${defs.map((x) => x.id).join(', ')}); one version per id`);
  }
}

// Pass 2: every link to a versioned anchor must hit an existing anchor.
for (const file of files) {
  const text = fs.readFileSync(path.join(repoRoot, file), 'utf8');
  for (const {line, n} of proseLines(text)) {
    for (const m of line.matchAll(linkRe)) {
      const [, target, fragment] = m;
      if (/^[a-z]+:/i.test(target) || !idPattern.test(fragment)) continue;
      const targetFile = target === '' ? file : path.posix.normalize(path.posix.join(path.posix.dirname(file), target));
      const ids = anchors.get(targetFile);
      if (!ids) {
        report(file, n, `link to "${target}#${fragment}": ${targetFile} is not a docs page`);
        continue;
      }
      if (ids.has(fragment)) continue;
      const [, base] = fragment.match(versioned);
      const now = [...ids.keys()].find((id) => id.match(versioned)?.[1] === base);
      report(
        file,
        n,
        now
          ? `stale link: ${targetFile}#${fragment} is now #${now}; check this reference still holds, then update it`
          : `broken link: ${targetFile} has no anchor "${fragment}" (removed or renamed)`,
      );
    }
  }
}

if (errors.length) {
  console.error(errors.join('\n'));
  console.error(`\n${errors.length} anchor problem(s). Rules: CLAUDE.md, "Documentation anchors".`);
  process.exit(1);
}
const count = [...anchors.values()].reduce((sum, ids) => sum + ids.size, 0);
console.log(`anchors ok: ${count} anchors in ${files.length} files`);
