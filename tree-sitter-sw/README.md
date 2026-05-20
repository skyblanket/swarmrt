# tree-sitter-sw

Tree-sitter grammar + highlight queries for the **sw** language (the surface language for the [SwarmRT](https://github.com/skyblanket/swarmrt) runtime).

## What you get

- `grammar.js` — full grammar covering modules, functions, `case`, `receive`, `try/catch`, f-strings, pattern matching, pipes, maps, lists, atoms, tuples, `send`/`spawn`.
- `queries/highlights.scm` — Helix / Neovim / GitHub-style highlight names. Identifies keywords, builtins, parameters, atoms, strings, comments distinctly.

## Build

```bash
cd tree-sitter-sw
npm install
npx tree-sitter generate
npx tree-sitter test          # if you add tests under corpus/
```

The generator produces a `tree-sitter-sw.so` (or `.dylib`/`.dll`) that's loadable by any tree-sitter host.

## Editor wiring

### Neovim (with nvim-treesitter)

```lua
local parser_config = require'nvim-treesitter.parsers'.get_parser_configs()
parser_config.sw = {
  install_info = {
    url = "https://github.com/skyblanket/swarmrt",
    files = { "src/parser.c" },
    location = "tree-sitter-sw",
    branch = "main",
  },
  filetype = "sw",
}
vim.filetype.add({ extension = { sw = "sw" } })
```

### Helix

Add to `~/.config/helix/languages.toml`:

```toml
[[language]]
name = "sw"
scope = "source.sw"
file-types = ["sw"]
roots = []
comment-token = "#"
indent = { tab-width = 4, unit = "    " }

[[grammar]]
name = "sw"
source = { git = "https://github.com/skyblanket/swarmrt", subpath = "tree-sitter-sw" }
```

Then `hx --grammar fetch sw && hx --grammar build sw`.

### VS Code

Use the [vscode-tree-sitter](https://github.com/georgewfraser/vscode-tree-sitter) extension, point it at the generated `.so`.

## Status

Grammar covers everything the SwarmRT lexer accepts as of `swarmrt` commit `36a4c79` (Tier 1: Std + MCP + SQLite/vec). Will drift if new keywords land — pull requests welcome.
