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

```bash
make run SIM=examples/hello.sim
```

Or manually:

```bash
./build/esimc input.sim -o output.ll        # compile to LLVM IR
llc -filetype=obj output.ll -o output.o     # assemble
clang output.o build/simula_rt.o -o program # link with runtime
./program                                    # run
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
Source (.sim) → Flex (tokens) → Bison (AST) → Codegen (LLVM IR) → LLC (object) → Clang (executable)
```

The compiler is single-pass: declarations are processed in order. The runtime library provides memory allocation (`simula_alloc`) and coroutine primitives (`simula_coro_*`) using POSIX `ucontext`.

## Limitations / Not Yet Implemented

- **Virtual dispatch**: method calls are statically dispatched based on the declared REF type, not the runtime type
- **VALUE/NAME parameter modes**: all parameters are passed by value
- **ARRAY type**: not implemented
- **Text operations**: TEXT is a simple char pointer, no Simula text operations (Sub, Length, etc.)
- **Garbage collection**: objects are allocated but never freed
- **Standard environment**: only basic I/O (OutText, OutInt, OutReal, OutImage)
- **Separate compilation**: everything must be in one source file
- **Error recovery**: parser stops at first error
