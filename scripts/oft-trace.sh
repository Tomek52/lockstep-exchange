#!/usr/bin/env bash
# Requirement tracing gate: OpenFastTrace over specs, code, tests and CI.
# Decision record: docs/adr/0015-requirement-tracing-with-openfasttrace.md
# Conventions:     CLAUDE.md, section 7.
#
#   scripts/oft-trace.sh               # trace the repository, gate, write build/oft/
#   scripts/oft-trace.sh PATH...       # trace only PATHs (same gate)
#   scripts/oft-trace.sh --self-test   # prove the gate on scripts/oft-fixtures/
#
# Reports: build/oft/report.html (browse), report.txt (plain), report.xml (aspec).
#
# The gate fails when:
#   1. a link is broken or stale: orphaned, outdated, predated, ambiguous,
#      unwanted or duplicate, in either direction;
#   2. a `Depends:` entry names an item that does not exist in that revision
#      (OpenFastTrace itself does not check dependencies);
#   3. an item is "not ok" (in the report: "-type" = needed coverage missing,
#      "+type" = coverage by a type it does not need), unless it is marked
#      `Status: proposed`, which is how acceptance criteria of
#      not-yet-implemented tasks are excluded (CLAUDE.md section 7).
# A `proposed` item that is already fully covered only gets a note.
# Items that are only "not ok (transitive)" are not failures in themselves:
# the item that causes them is always reported by one of the rules above.
#
# Needs Java 17+. Set OFT_JAR to use an existing openfasttrace jar; otherwise
# the pinned release is downloaded once and its checksum verified.
set -euo pipefail

OFT_VERSION="4.10.0"
OFT_SHA256="8449a1652f140841a89fb053b71130b9c880fe2b7dd06f3490ae9c740a0c5e08"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
out_dir="build/oft"
default_inputs=(docs ROADMAP.md exchange-core rust/crates .github/workflows)

require_java() {
  if ! command -v java >/dev/null; then
    echo "error: java not found; OpenFastTrace needs Java 17+" \
      "(Ubuntu: sudo apt-get install default-jre-headless)" >&2
    exit 2
  fi
  local major
  major="$(java -XshowSettings:properties -version 2>&1 \
    | sed -n 's/^ *java\.specification\.version = //p')"
  if [[ -z "${major}" || "${major%%.*}" -lt 17 ]]; then
    echo "error: OpenFastTrace needs Java 17+, found '${major:-unknown}'" >&2
    exit 2
  fi
}

oft_jar() {
  if [[ -n "${OFT_JAR:-}" ]]; then
    echo "${OFT_JAR}"
    return
  fi
  local jar="${out_dir}/tools/openfasttrace-${OFT_VERSION}.jar"
  if [[ ! -f "${jar}" ]]; then
    mkdir -p "${out_dir}/tools"
    local url="https://github.com/itsallcode/openfasttrace/releases/download/${OFT_VERSION}/openfasttrace-${OFT_VERSION}.jar"
    curl -fsSL -o "${jar}.part" "${url}"
    if ! echo "${OFT_SHA256}  ${jar}.part" | sha256sum -c --quiet -; then
      rm -f "${jar}.part"
      echo "error: checksum mismatch for ${url}" >&2
      exit 2
    fi
    mv "${jar}.part" "${jar}"
  fi
  echo "${jar}"
}

# OpenFastTrace exits 0 (all ok) or 1 (defects found); the gate below decides
# which defects matter. Anything else is a usage or tool error.
run_oft() {
  local status=0
  java -jar "${jar}" trace "$@" || status=$?
  if [[ ${status} -gt 1 ]]; then
    echo "error: openfasttrace failed (exit ${status})" >&2
    exit 2
  fi
}

# Traces "$@" into $1 (a report directory) and applies the gate.
# Returns 0 if the gate passes, 1 if it fails.
trace_and_gate() {
  local reports="$1"
  shift
  mkdir -p "${reports}"
  run_oft -o plain -v failure_details -c BLACK_AND_WHITE -f "${reports}/report.txt" "$@"
  run_oft -o aspec -f "${reports}/report.xml" "$@"
  run_oft -o html -f "${reports}/report.html" "$@"
  python3 - "${reports}/report.txt" "${reports}/report.xml" <<'PY'
import re
import sys
import xml.etree.ElementTree as ET

plain_path, aspec_path = sys.argv[1], sys.argv[2]
BROKEN = ("orphaned", "outdated", "predated", "ambiguous", "unwanted", "duplicate",
          "unwanted coverage", "predated coverage", "outdated coverage")
header = re.compile(r"^(ok|not ok)( \(transitive\))? \[[^\]]*\] (\S+)(?: \[(\w+)\])?(?: \((.*)\))?")
link = re.compile(r"^\s+\[(?P<status>[a-z ]+?)\s*\] (?P<dir>[←→]) (?P<target>\S+)")

errors, allowed, listed = [], [], set()
item = None
with open(plain_path, encoding="utf-8") as plain:
    for line in plain:
        if m := header.match(line):
            verdict, transitive, item, status, needs = m.groups()
            listed.add(item)
            if verdict == "not ok" and not transitive:
                if status == "proposed":
                    allowed.append(f"{item} ({needs or ''})")
                else:
                    errors.append(f"not ok: {item} ({needs or ''})")
        elif (m := link.match(line)) and m["status"] in BROKEN:
            errors.append(f"{m['status']} link: {item} {m['dir']} {m['target']}")

root = ET.parse(aspec_path).getroot()
items = {}
for group in root.iter("specobjects"):
    doctype = group.get("doctype")
    for obj in group.findall("specobject"):
        items[f"{doctype}~{obj.findtext('id')}~{obj.findtext('version')}"] = obj
names = {i.rsplit("~", 1)[0]: i for i in items}
# The aspec report calls every non-approved item UNCOVERED, so "fully covered"
# is taken from the plain report instead: it lists every item that is not ok.
stale_proposed = [i for i, obj in items.items()
                  if obj.findtext("status") == "proposed" and i not in listed]
for item_id, obj in items.items():
    for dep in obj.iter("dependsOnSpecObject"):
        target = f"{dep.findtext('doctype')}~{dep.findtext('id')}~{dep.findtext('version')}"
        if target not in items:
            other = names.get(target.rsplit("~", 1)[0])
            why = f"outdated, current is {other}" if other else "no such item"
            where = f"{obj.findtext('sourcefile')}:{obj.findtext('sourceline')}"
            errors.append(f"broken Depends: {item_id} -> {target} ({why}) at {where}")

summary = f"{len(items)} items"
if allowed:
    summary += f", {len(allowed)} proposed and not yet covered (allowed)"
    for entry in allowed:
        print(f"  allowed: {entry}")
for item_id in stale_proposed:
    print(f"  note: {item_id} is fully covered; drop its 'Status: proposed'")
for error in errors:
    print(f"  FAIL: {error}")
print(f"oft gate: {summary}, {len(errors)} failure(s)")
sys.exit(1 if errors else 0)
PY
}

self_test() {
  local fixtures="scripts/oft-fixtures" failed=0 dir name
  for dir in "${fixtures}"/*/; do
    name="$(basename "${dir}")"
    local expect_pass=0
    [[ "${name}" == pass* ]] && expect_pass=1
    local result=0
    trace_and_gate "${out_dir}/self-test/${name}" "${dir}" >"${out_dir}/self-test-${name}.log" || result=$?
    if [[ ${expect_pass} -eq 1 && ${result} -eq 0 ]] || [[ ${expect_pass} -eq 0 && ${result} -eq 1 ]]; then
      echo "ok   ${name}"
    else
      echo "FAIL ${name}: gate exit ${result}, expected $([[ ${expect_pass} -eq 1 ]] && echo 0 || echo 1)"
      sed 's/^/     /' "${out_dir}/self-test-${name}.log"
      failed=1
    fi
  done
  return ${failed}
}

require_java
mkdir -p "${out_dir}"
jar="$(oft_jar)"

if [[ "${1:-}" == "--self-test" ]]; then
  self_test && echo "oft gate self-test passed"
  exit
fi

inputs=("$@")
[[ ${#inputs[@]} -eq 0 ]] && inputs=("${default_inputs[@]}")
if trace_and_gate "${out_dir}" "${inputs[@]}"; then
  echo "trace report: ${out_dir}/report.html"
else
  echo "trace report: ${out_dir}/report.html (details: ${out_dir}/report.txt)" >&2
  exit 1
fi
