# Doc Search
Mini C++ CLI app for searching keywords across local PDF research documents.

## Install dependency

### macOS

```bash
brew install poppler cmake
```

## Build

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Run

From `build` directory:

```bash
./pdfsearch ~/Documents/research_papers
```

## CLI Queries
```text
search> stochastic block model
```

Use `or:` to match on any of the keywords

```text
search> or: grid regular complete
```

## Remark
Rebuilds the index each time you run it, suitable for small amount of .pdf files only.
