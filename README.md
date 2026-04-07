# kiso-parser-macro

A suite of Clang-based static analysis tools for extracting and analyzing C preprocessor macro information. The tool consists of two complementary tools:

| Tool | Purpose | Output |
|---|---|---|
| **macro_finder** | Tracks preprocessor conditional directives (`#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`) with evaluation results | Line-based text |
| **macro_analyzer** | Extracts macro definitions and resolves symbols referenced in macro bodies | JSON |


Together, they provide a complete picture of a project's preprocessor landscape: what macros are defined, what they reference, and how conditional compilation branches are evaluated.

**Note**: This tool is under active development. Commented-out code is intentionally retained as part of ongoing experimentation across diverse C codebases. It will be cleaned up as the tool matures.


## Directory Structure

```
kiso-parser-macro/
├── macro_finder/
│   ├── CMakeLists.txt
│   ├── build.sh
│   └── macro_finder.cpp
├── macro_analyzer/
│   ├── CMakeLists.txt
│   ├── build.sh
│   └── macro_analyzer.cpp
├── llvm-project/
├── llvm-custom/
├── clang-modifications.patch
├── download_clang.sh
└── README.md
```

## Requirements

- Ubuntu (tested on Ubuntu 22.04+)
- LLVM/Clang 19 development libraries (`libclang-19-dev` or equivalent)
- CMake 3.16+
- C++17 compiler
- Python 3.10+ (for API server usage)


## Setup: Build LLVM/Clang

```bash
# Download LLVM source
./download_clang.sh

# Apply custom patch
cd llvm-project
git apply ../clang-modifications.patch

# Build
cmake -S llvm -B build -G Ninja \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../llvm-custom
cmake --build build
cmake --install build
```

## Building

Each tool is built independently:

```bash
# macro_finder
cd kiso-parser-macro/macro_finder
./build.sh

# macro_analyzer
cd kiso-parser-macro/macro_analyzer
./build.sh

```

---


## macro_finder

### What it does

`macro_finder` tracks all preprocessor conditional directives and macro definition/undefinition events during compilation, outputting a line-by-line log. Key features:

- **Conditional evaluation tracking** — Each `#if`, `#ifdef`, `#ifndef`, and `#elif` is annotated with whether it evaluated to true or false (e.g., `IF_TRUE`, `IFDEF_FALSE`).
- **Skipped block detection** — Directives inside skipped conditional blocks are recorded with a `(skipped)` suffix.
- **`#endif` closure mapping** — Every `#endif` output includes `=> Closes` lines showing which `#if`/`#elif`/`#else` directives it closes, enabling reconstruction of the full conditional block structure.
- **Macro definition bodies** — `#define` output includes the reconstructed replacement text (e.g., `MAX(a, b) -> ((a) > (b) ? (a) : (b))`).
- **Definition location cross-referencing** — `#ifdef`/`#ifndef` and `#if defined(...)` entries include the definition location of each referenced macro.

### Usage

**Compilation database mode:**

```bash
macro_finder -p <compile_commands_dir>
```

**Examples:**

```bash
# Entire project
macro_finder -p /path/to/project/build
```

### Output Format

Line-based text is written to stdout. Each line starts with a directive type tag in brackets.

#### Macro definitions

```
[DEFINED] /abs/path/file.h:10:9:10:9 - BUFFER_SIZE -> 4096
[DEFINED_FUNC] /abs/path/file.h:12:9:12:12 - MAX(a, b) -> ((a) > (b) ? (a) : (b))
[DEFINED (skipped)] /abs/path/file.h:20:9:20:9 - OLD_API
[UNDEFINED] /abs/path/file.h:30:9:30:9 - TEMP_FLAG
```

Format: `[TYPE] <path>:<start_line>:<start_col>:<end_line>:<end_col> - <info>`

#### Conditional directives

```
[IFDEF_TRUE] /abs/path/file.h:5:2:5:8 - HAVE_FEATURE (defined at: /abs/path/config.h:3:9)
[IFDEF_FALSE] /abs/path/file.h:5:2:5:8 - MISSING_FEATURE (defined at: undefined)
[IFNDEF_TRUE] /abs/path/file.h:1:2:1:18 - HEADER_GUARD_H (defined at: undefined)
[IF_TRUE] /abs/path/file.c:10:2:10:20 - defined(FOO) && BAR > 1 [FOO defined at: ...; BAR defined at: ...]
[IF_FALSE] /abs/path/file.c:15:2:15:10 - DEBUG [DEBUG defined at: undefined]
[ELIF_TRUE] /abs/path/file.c:17:2:17:15 - VERSION >= 2 [VERSION defined at: ...]
[ELIF_NOT_EVALUATED] ...
[ELSE_TRUE] /abs/path/file.c:20:2:20:2
[ELSE_FALSE] /abs/path/file.c:20:2:20:2
```

#### Directive type suffixes

| Suffix | Meaning |
|---|---|
| `_TRUE` | The condition evaluated to true; the enclosed block was compiled. |
| `_FALSE` | The condition evaluated to false; the enclosed block was skipped. |
| `_NOT_EVALUATED` | (`#elif` only) The condition was not evaluated because a preceding branch was already taken. |
| `(skipped)` | The directive itself was inside a skipped block and was not processed by the preprocessor. |

#### `#endif` with closure mapping

```
[ENDIF] /abs/path/file.c:25:2:25:2
  => Closes [IF_TRUE] at /abs/path/file.c:10:2:10:20 (defined(FOO) && BAR > 1)
  => Closes [ELIF_FALSE] at /abs/path/file.c:15:2:15:10 (VERSION < 2)
  => Closes [ELSE_TRUE] at /abs/path/file.c:20:2:20:2
```

Each `#endif` lists every directive in its conditional block (`#if`, all `#elif`s, and `#else` if present), making it straightforward to reconstruct the complete block structure.

---


## macro_analyzer

### What it does

`macro_analyzer` hooks into Clang's preprocessor and AST to:

1. **Collect all macro definitions** (`#define`) — both object-like and function-like — recording their source location and line range.
2. **Identify tokens used inside each macro body**, filtering out the macro's own parameters.
3. **Resolve those tokens** to their definitions through a three-stage strategy (see [Resolution Strategy](#resolution-strategy)).
4. **Output structured JSON** with all collected information.

### Usage

**Single-file mode:**

```bash
macro_analyzer <source_file> [-- <compile_flags>]
```

**Compilation database mode (batch):**

```bash
macro_analyzer -p <compile_commands_dir>
```

Analyzes all files listed in a `compile_commands.json` found in the given directory. Results from all translation units are merged into a single JSON output.

**Examples:**

```bash
# Entire project via compile_commands.json
macro_analyzer -p /path/to/project/build
```

### Output Format

JSON is written to stdout. Diagnostics and progress go to stderr.

```json
{
  "macros": [
    {
      "kind": "macro",
      "name": "BUFFER_SIZE",
      "definition": "/home/user/project/config.h:12:9",
      "start_line": 12,
      "end_line": 12,
      "uses": [
        {
          "kind": "macro",
          "name": "PAGE_SIZE",
          "usage_location": "/home/user/project/config.h:12:22",
          "definition": "/home/user/project/config.h:5:9",
          "start_line": 5,
          "end_line": 5
        }
      ]
    },
    {
      "kind": "macro_function",
      "name": "MAX",
      "parameters": ["a", "b"],
      "definition": "/home/user/project/util.h:3:9",
      "start_line": 3,
      "end_line": 3,
      "uses": []
    }
  ]
}
```

#### Macro entry fields

| Field | Description |
|---|---|
| `kind` | `"macro"` for object-like, `"macro_function"` for function-like. |
| `name` | The macro identifier. |
| `parameters` | *(function-like only)* Parameter names. Includes `"..."` for variadic macros. |
| `definition` | Absolute path with line and column: `<path>:<line>:<col>`. |
| `start_line` / `end_line` | Line range of the macro definition. |
| `uses` | Symbols referenced in the macro body (excluding its own parameters). |

#### Uses entry fields

| Field | Description |
|---|---|
| `kind` | `"macro"`, `"macro_function"`, `"function"`, `"global_var"`, `"typedef"`, `"struct"`, `"union"`, `"enum"`, `"enum_constant"`, or `"unknown"`. |
| `name` | The referenced identifier. |
| `usage_location` | Where the token appears inside the macro body. |
| `definition` | Where the referenced symbol is defined. |
| `start_line` / `end_line` | Line range of the referenced symbol's definition. |

### Resolution Strategy

Tokens inside a macro body are resolved in three passes:

1. **At `MacroDefined` time** — The preprocessor is queried for each identifier token. If it is a known macro at that point, the reference is resolved immediately.

2. **At `MacroExpands` time** — When a macro is expanded during preprocessing, the tool revisits unresolved tokens in that macro's body and attempts resolution again. This catches macros defined after the referencing macro but before first use.

3. **At `EndSourceFileAction` time** — After the full AST is available, remaining unresolved tokens are matched against all collected macro definitions and AST-level symbols (functions, global variables, typedefs, structs/unions, enums, enum constants). Tokens that still cannot be resolved retain `kind: "unknown"`.

---


