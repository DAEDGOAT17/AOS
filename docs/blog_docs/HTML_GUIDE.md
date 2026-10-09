# Maintain the AOS Documentation Portal

The documentation portal is a static site in this directory. GitHub Actions validates its generated glossary/search data and deploys the repository to GitHub Pages after checks pass.

## Preview locally

From the repository root, generate the current indexes and serve the site:

```sh
python3 tools/generate_docs_index.py
python3 -m http.server 8000
```

Open `http://localhost:8000/docs/blog_docs/`. A local HTTP server is recommended so browser search can load the adjacent generated `search-index.js` reliably.

## Edit documentation

- Put first-time setup and contribution instructions in `START_HERE.md`.
- Put subsystem explanations in the existing architecture, memory, interrupt, driver, filesystem, core, and networking directories.
- Use `index.html` for navigation and search, not as the only copy of technical material.
- Keep claims tied to implementation and tests. Distinguish host-provided model inference from kernel-owned policy/execution; do not call the bounded Lisp runtime a security sandbox.
- Link to source files and lines when describing APIs. The generated glossary is the authoritative inventory of declarations and definitions.

## Regenerate the glossary and search index

```sh
python3 tools/generate_docs_index.py
python3 tools/check_docs.py
```

The generator indexes public AOS C declarations under `include/` and AOS-owned C function definitions under `src/`. Vendored `src/net/lwip/` implementation is intentionally excluded. Commit the generated `FUNCTION_GLOSSARY.md`, `FUNCTION_GLOSSARY.html`, and `search-index.js` whenever public interfaces, functions, or indexed docs change.

GitHub Actions repeats generation and fails if generated output is stale, required docs are missing, the primary navigation contains broken relative links, or representative API entries are absent. Pull requests run validation; pushes to `pankaj` deploy the Pages site only after validation succeeds.
