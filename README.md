# esimc — A Simula Compiler

A compiler for a subset of the Simula programming language, targeting LLVM IR. Built with flex, bison, and LLVM.

## Building

### Prerequisites

- **macOS** (tested on ARM64) or Linux
- LLVM (tested with 21.x, installed via Homebrew)
- Bison 3.8+ (`brew install bison` — the Apple-shipped bison 2.3 is too old)
- Flex
- Clang/Clang++

### Build

```bash
make        # builds the compiler (build/esimc) and runtime (build/simula_rt.o)
```

### Compile and run a Simula program

`esimc` is a one-step compiler: point it at a `.sim` file and an output name and it
produces a runnable executable directly, linking the runtime for you.

```bash
./build/esimc input.sim -o program   # compile + link -> executable
./program                            # run
```

The output extension selects what is produced:

| `-o` argument | Output                       |
|---------------|------------------------------|
| `program`     | linked executable (default)  |
| `program.o`   | native object file           |
| `program.ll`  | LLVM IR (text)               |

The runtime (`simula_rt.o`) is located automatically next to the `esimc` binary.
The linker is the system `cc` (override with the `CC` environment variable).

Output is optimized at `-O2` by default (an LLVM `mem2reg`/`SROA`/`GVN`/inlining
pipeline runs before lowering); pass `-O0` to disable optimization.

Or via make:

```bash
make run SIM=examples/hello.sim
```

## Supported Language Features

### Types and declarations

```simula
INTEGER x;
REAL y;
BOOLEAN flag;
TEXT greeting;
REF(MyClass) obj;
```

### Arithmetic and logic

```simula
x := 2 + 3 * 4;        ! arithmetic: + - * / //
flag := x > 10 AND y < 5.0;  ! comparisons and logic: = <> < <= > >= AND OR NOT
```

### Control flow

```simula
IF x > 0 THEN
    OutText("positive")
ELSE
    OutText("non-positive");

WHILE x > 0 DO
BEGIN
    x := x - 1
END;

FOR i := 1 STEP 1 UNTIL 10 DO
    OutInt(i, 4);
```

### Procedures and functions

Procedures use inline typed parameters:

```simula
PROCEDURE greet(TEXT name);
BEGIN
    OutText("Hello, ");
    OutText(name);
    OutImage
END;

! Typed procedures return values by assigning to their own name:
INTEGER PROCEDURE factorial(INTEGER n);
BEGIN
    IF n <= 1 THEN factorial := 1
    ELSE factorial := n * factorial(n - 1)
END;
```

### Classes and inheritance

```simula
CLASS Point(REAL x, REAL y);
BEGIN
    PROCEDURE show;
    BEGIN
        OutText("(");
        OutReal(x, 1, 1);
        OutText(", ");
        OutReal(y, 1, 1);
        OutText(")")
    END;
END;

! Inheritance: ParentClass CLASS ChildClass(params)
Point CLASS ColorPoint(INTEGER r, INTEGER g, INTEGER b);
BEGIN
    ! additional methods here
END;
```

### REF variables and object creation

```simula
REF(Point) p;
p :- NEW Point(3.0, 4.0);   ! :- is reference assignment
p.show();                     ! method call
OutReal(p.x, 1, 1);          ! field access
p.x := 5.0;                  ! field assignment
```

### Type checking

```simula
IF p IS ColorPoint THEN ...   ! exact class check
IF p IN Shape THEN ...        ! class or ancestor check

INSPECT obj
    WHEN Circle DO OutText("circle")
    WHEN Square DO OutText("square")
    OTHERWISE OutText("unknown");
```

### Coroutines (DETACH / RESUME)

Simula's signature feature — class bodies are coroutines:

```simula
CLASS Producer(INTEGER count);
BEGIN
    INTEGER i;
    FOR i := 1 STEP 1 UNTIL count DO
    BEGIN
        OutInt(i, 4);
        OutImage;
        DETACH          ! suspend, return to caller
    END
END;

REF(Producer) p;
p :- NEW Producer(5);   ! runs body until first DETACH, prints "1"
RESUME(p);               ! continues from DETACH, prints "2"
RESUME(p);               ! prints "3"
```

### I/O

```simula
OutText("hello");           ! print string
OutInt(42, 4);              ! print integer with width
OutReal(3.14, 1, 2);       ! print real with width and decimals
OutImage;                   ! newline (flush output line)
```

### Comments

```simula
! This is a line comment
COMMENT This is a block comment that ends with a semicolon;
```

## Project Structure

```
src/
  ast.h          — AST node definitions
  lexer.l        — flex lexer (tokenizer)
  parser.y       — bison parser (grammar → AST)
  codegen.h      — LLVM code generation context
  codegen.cpp    — LLVM IR generation for all AST nodes
  main.cpp       — compiler driver
runtime/
  simula_rt.h    — runtime library header
  simula_rt.c    — coroutine runtime (ucontext-based)
examples/
  hello.sim      — hello world
  procedures.sim — procedures, recursion, fibonacci
  classes.sim    — classes, inheritance, REF, IS
  coroutines.sim — DETACH/RESUME coroutines
  inspect.sim    — INSPECT/WHEN type dispatch
```

## Architecture

```
Source (.sim) → Flex (tokens) → Bison (AST) → Sema (checks) → Codegen (LLVM IR)
              → LLVM TargetMachine (object) → system cc (link runtime → executable)
```

The compiler is single-pass: declarations are processed in order. The runtime library provides memory allocation (`simula_alloc`) and coroutine primitives (`simula_coro_*`) using POSIX `ucontext`.

## What's Implemented

- **Types**: INTEGER, REAL, BOOLEAN, TEXT, CHARACTER, REF(Class); ARRAY (1D/2D dynamic bounds, 3D constant bounds)
- **Procedures**: C-style and Simula-style parameter declarations, recursion, return-by-name, NAME/VALUE/array/LABEL parameters, formal procedures
- **Classes**: inheritance with prefixing, INNER, virtual dispatch via vtables (qualified and unqualified calls), INSPECT/WHEN/OTHERWISE, IS/IN, QUA
- **Coroutines**: DETACH/RESUME (including symmetric RESUME between coroutines) via POSIX ucontext (Windows Fibers on Win32)
- **SIMULATION**: PROCESS classes, ACTIVATE/REACTIVATE (AT/DELAY/BEFORE/AFTER/PRIOR), HOLD, PASSIVATE, WAIT, CANCEL, TIME, CURRENT, EVTIME/IDLE/TERMINATED, ACCUM
- **SIMSET**: LINK/HEAD doubly-linked lists (INTO, OUT, FIRST, LAST, SUC, PRED, CARDINAL, EMPTY)
- **TEXT**: a proper text descriptor (frame/start/length/pos) — SUB/STRIP/MAIN alias the parent frame, `==`/`=/=` are reference identity, `=`/`<>` content; .Length .More .GetChar .PutChar .SetPos .Pos, GetInt/GetReal/GetFrac, PutInt/PutFix/PutReal/PutFrac
- **I/O**: OutText/OutInt/OutReal/OutFix/OutFrac/OutChar/OutImage, ININT/INREAL/INCHAR/INFRAC/LASTITEM; INFILE and OUTFILE classes; EXTERNAL CLASS auto-loading
- **Built-ins**: ABS, MOD, ENTIER, ROUND, SIGN, SQRT/SIN/COS/TAN/EXP/LN/LOG, CHAR, RANK, DIGIT, LETTER, UPCASE, LOWCASE, BLANKS, COPY, LENGTH, MAXINT/PI, LOWERBOUND/UPPERBOUND; drawing library (RANDINT, UNIFORM, NORMAL, NEGEXP, POISSON, ERLANG, DRAW, DISCRETE, LINEAR, HISTD, HISTO) with by-reference seeds
- **Control flow**: IF/THEN/ELSE (statement and expression), WHILE, FOR (range, value-list, and multi-range), GOTO/LABEL (incl. non-local GOTO via LABEL parameters), SWITCH + computed GOTO
- **Operators**: arithmetic (+ - * / // **), comparison `= <> < <= > >=` and letter forms `EQ NE LT LE GT GE`, reference `== =/=`, logical (AND/OR/NOT/EQV/IMP/AND THEN/OR ELSE), text concatenation (&)
- **Semantic analysis**: a pre-codegen pass reports, with source line numbers, unknown classes, undefined variables, misspelled calls, wrong NEW/procedure argument counts, and non-BOOLEAN IF/WHILE conditions
- **Optimization**: an LLVM `-O2` module pipeline (mem2reg/SROA/instcombine/GVN/simplifycfg/inlining) runs before object emission (disable with `-O0`)
- **Checked runtime mode**: array indexing (constant-bound arrays), integer division/MOD by zero, and NONE-reference access (method calls, field reads/writes) are checked and abort with a source-located `Runtime error at line N: ...` diagnostic instead of corrupting memory or silently producing garbage

## Limitations / Not Yet Implemented

- **NAME parameters re-evaluation (Jensen's device)**: *intentionally not implemented.* A NAME parameter captures the actual's address once at the call (so write-back to a plain variable or array element works); it is not re-evaluated on every access, so Jensen's-device-style `Sum(i, 1, n, A(i))` does not recompute `A(i)` per iteration. This is rarely used in practice and the thunk machinery it requires isn't worth the complexity.
- **Assignment type compatibility**: the type checker flags non-BOOLEAN conditions and bad arities but does not fully check `:=` operand compatibility (Simula's implicit INTEGER↔REAL↔CHARACTER coercions make this false-positive-prone).
- **3D arrays**: constant bounds only; not supported as procedure parameters (1D/2D may have dynamic bounds and be passed to procedures).
- **Bounds checking scope**: the checked runtime mode covers constant-bound (stack) arrays; dynamic-bound arrays are not yet bounds-checked (their size isn't threaded to the access site).
- **Garbage collection**: objects and text frames are allocated but never freed (arena-leak model).
- **Parser error recovery**: parsing stops at the first syntax error (semantic and codegen errors are collected and reported together).

## Test Coverage

Two suites, both run by scripts in the repo root:

- `./test_all.sh` — **53 positive examples** in `examples/`, checked by full-output golden diff against `examples/expected/` (`--bless` regenerates). All passing.
- `./test_bad.sh` — **28 negative examples** in `examples/bad/`, each an intentionally-wrong program that must be rejected with the right diagnostic. All correctly rejected.
