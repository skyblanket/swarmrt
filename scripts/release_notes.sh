#!/usr/bin/env bash
# Print the GitHub release notes for a tag: the docs/CHANGELOG.md entry headed
# "## <date> — <tag>" (if there is one), then the install instructions.
# Used by .github/workflows/release.yml; run it locally to preview.
#
#   bash scripts/release_notes.sh v2.0.0
set -euo pipefail

tag="${1:?usage: release_notes.sh <tag>}"
root="$(cd "$(dirname "$0")/.." && pwd)"
repo="https://github.com/skyblanket/swarmrt"

# The entry runs from its header to the next "---" rule or "## " header.
# Its own links are relative to docs/, so point them at the tagged tree.
notes="$(awk -v tag="$tag" '
    /^## / { if (found) exit; if ($NF == tag) { found = 1; next } }
    found && /^---$/ { exit }
    found { print }
' "$root/docs/CHANGELOG.md" |
    sed -E "s#\]\(([A-Za-z0-9_./-]+\.md)\)#](${repo}/blob/${tag}/docs/\1)#g")"

echo "## SwarmRT ${tag}"
echo
if [ -n "$notes" ]; then
    printf '%s\n' "$notes" | sed -e '/./,$!d'
    echo
fi

linux="swarmrt-${tag}-linux-x86_64"
cat <<NOTES
### Install

Download the archive for your platform, extract it, and put its \`bin/\` on PATH
(shown for Linux x86_64; use \`darwin-arm64\` or \`linux-arm64\` elsewhere):

\`\`\`bash
curl -LO ${repo}/releases/download/${tag}/${linux}.tar.gz
tar xzf ${linux}.tar.gz
export PATH="\$PWD/${linux}/bin:\$PATH"
swc --version
swc new myagent && cd myagent && make test
\`\`\`

\`swc build\` needs a C compiler and the system libraries the runtime links
against: \`sudo apt-get install -y build-essential libsqlite3-dev libssl-dev
zlib1g-dev\` on Ubuntu/Debian, \`brew install sqlite openssl@3 zlib\` on macOS.

| Archive | Platform |
|---------|----------|
| \`swarmrt-${tag}-darwin-arm64.tar.gz\` | macOS Apple Silicon |
| \`swarmrt-${tag}-linux-x86_64.tar.gz\` | Linux x86_64 |
| \`swarmrt-${tag}-linux-arm64.tar.gz\` | Linux ARM64 |

Each archive contains \`bin/swc\`, \`bin/libswarmrt.a\`, the runtime headers
(\`src/\`), the stdlib (\`lib/\`), the project templates (\`templates/\`), the
docs (\`docs/\`), \`README.md\` and \`LICENSE\`. Verify downloads against
\`checksums.txt\`.
NOTES
