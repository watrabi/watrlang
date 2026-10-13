# watrlang

A compiled systems language with PHP-flavored syntax, written in plain C.
It's low-level enough to write an operating system in.

```php
import "std/io";

function main(): int {
    string name = "world";
    echo "Hello, {name}!\n";
    return 0;
}
```

The full plan is in [docs/DESIGN.md](docs/DESIGN.md).

## Status

| Milestone | State |
|-----------|-------|
| 0. Skeleton (build, arena, diagnostics) | done |
| 1. Lexer | done |
| 2. Parser | done |
| 3. First binary: int math, calls, return | done |
| 4. Variables, if/while/for, parameters, globals | done |
| 5. Type system (sized ints, bool, pointers, structs, floats) | next |
| 6+. FFI, strings, modules, ... | planned |

## Build

Needs a C99 compiler, `make`, and GNU binutils (`as`, `ld`) on x86-64 Linux.

```sh
make                          # builds build/watrc
make test                     # runs the test suite
make SAN=1 CC=gcc test        # same, with AddressSanitizer + UBSan
./build/watrc file.watr && ./file  # compile and run
./build/watrc -S file.watr         # just write file.s
./build/watrc --tokens file.watr   # dump tokens
./build/watrc --ast file.watr      # dump the syntax tree
```

## Tests

Each `tests/lexer/*.watr` (token dump), `tests/parser/*.watr` (syntax
tree) and `tests/run/*.watr` (compiled and run) has a `.expected` file
holding the output, error messages and exit code.

Two differential testers compile random code and check every result
against a Python model of watrlang:

- `tests/expr_fuzz.py`: integer expressions
- `tests/prog_fuzz.py`: whole programs with variables, if/else, loops,
  break/continue and calls with up to 8 arguments

`make test` runs a quick pass of each; for a long one add
`--seed 5 --programs 500`. After an intentional output change, regenerate
them with `UPDATE=1 ./tests/run_tests.sh` and review the diff.
