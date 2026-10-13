#include <string.h>

#include "diag.h"

/*
 * Long-form explanations for `watrc --explain CODE`.
 *
 * Code ranges:
 *   E00xx  lexing (characters, numbers, strings)
 *   E01xx  syntax
 *   E02xx  names, scopes and assignment
 *   E03xx  types, returns and constants
 *   E09xx  features that aren't implemented yet
 *   W0xxx  warnings
 */

typedef struct {
    const char *code;
    const char *text;
} Explanation;

static const Explanation explanations[] = {
    {"E0001",
     "A block comment was opened with /* but never closed with */.\n"
     "\n"
     "    /* this comment runs to the end of the file\n"
     "    function main(): int { return 0; }\n"
     "\n"
     "Add the closing */. Block comments don't nest, so the first */ ends\n"
     "the comment."},

    {"E0002",
     "An integer literal starts with 0 followed by more digits.\n"
     "\n"
     "    int mode = 0755;   // error\n"
     "\n"
     "In C, a leading zero silently means octal, so 0755 is 493. That trips\n"
     "people up, so watrlang rejects it. Say what you mean:\n"
     "\n"
     "    int mode = 0o755;  // octal: 493\n"
     "    int n = 755;       // decimal"},

    {"E0003",
     "A base prefix (0x, 0b or 0o) isn't followed by any digits of that base.\n"
     "\n"
     "    int x = 0x;    // error: no digits\n"
     "    int y = 0b2;   // error: 2 isn't a binary digit\n"
     "\n"
     "Hex digits are 0-9 and a-f, binary digits are 0 and 1, and octal\n"
     "digits are 0-7."},

    {"E0004",
     "A number is immediately followed by letters or digits that don't\n"
     "belong to it.\n"
     "\n"
     "    int x = 12abc;  // error\n"
     "    int y = 0xFG;   // error: G isn't a hex digit\n"
     "\n"
     "watrlang has no number suffixes like C's 10UL. Use a cast instead:\n"
     "`10 as u64`."},

    {"E0005",
     "An underscore in a number isn't between two digits.\n"
     "\n"
     "    int a = 1__000;   // error: two in a row\n"
     "    int b = 1000_;    // error: at the end\n"
     "    int c = 1_000;    // ok\n"
     "\n"
     "Underscores are only there to make long numbers readable."},

    {"E0006",
     "An integer literal is larger than 2^64 - 1 (18446744073709551615),\n"
     "the largest value any watrlang integer type can hold."},

    {"E0007",
     "A string or char literal contains a backslash escape watrlang doesn't\n"
     "know.\n"
     "\n"
     "    string s = \"C:\\Users\";   // error: \\U isn't an escape\n"
     "    string t = \"C:\\\\Users\";  // ok: \\\\ is a backslash\n"
     "\n"
     "The escapes are \\n \\t \\r \\0 \\\\ \\\" \\' \\{ \\} and \\xHH (one byte\n"
     "in hex)."},

    {"E0008",
     "A \\x escape needs exactly two hex digits.\n"
     "\n"
     "    string s = \"\\x4\";    // error\n"
     "    string t = \"\\x41\";   // ok: the byte 0x41, \"A\""},

    {"E0009",
     "A string contains {} with nothing inside.\n"
     "\n"
     "In a watrlang string, { starts an interpolation: \"Hi {name}\" inserts\n"
     "the value of `name`. For a literal brace, escape the opening one:\n"
     "\n"
     "    string json = \"\\{}\";   // the two characters {}"},

    {"E0010",
     "An interpolation inside a string was opened with { but not closed\n"
     "with } on the same line.\n"
     "\n"
     "    string s = \"Hi {name\";    // error\n"
     "    string t = \"Hi {name}\";   // ok\n"
     "    string u = \"a \\{ brace\";  // ok: a literal {\n"
     "\n"
     "Interpolations must close on the same line so a forgotten } gets\n"
     "reported where it happened, instead of swallowing the rest of the file."},

    {"E0011",
     "A string literal was opened with \" but never closed.\n"
     "\n"
     "Strings can span several lines, so this error points at the opening\n"
     "quote: the string runs from there to the end of the file."},

    {"E0012",
     "A char literal is empty, holds more than one byte, or isn't closed.\n"
     "\n"
     "    u8 a = '';     // error: empty\n"
     "    u8 b = 'ab';   // error: two bytes\n"
     "    u8 c = 'a';    // ok\n"
     "\n"
     "A char literal is a single byte (a u8). For text, use a string with\n"
     "double quotes."},

    {"E0013",
     "A variable name starts with $.\n"
     "\n"
     "watrlang's syntax is inspired by PHP, but variables don't use a sigil:\n"
     "\n"
     "    $count = 5;      // error\n"
     "    count = 5;       // ok\n"
     "\n"
     "Inside strings, interpolation uses braces: \"total: {count}\"."},

    {"E0014",
     "The source contains a character that isn't part of watrlang's syntax.\n"
     "\n"
     "Comments start with //, not #, and there's no @ operator."},

    {"E0015",
     "An identifier contains non-ASCII characters.\n"
     "\n"
     "    int café = 1;     // error\n"
     "    int cafe = 1;     // ok\n"
     "\n"
     "Names must be ASCII letters, digits and underscores. Any UTF-8 text is\n"
     "fine inside strings and comments."},

    {"E0100",
     "The parser found a token it didn't expect at this point.\n"
     "\n"
     "The message says what it expected instead. Often something earlier on\n"
     "the line is missing, like a closing parenthesis."},

    {"E0101",
     "A statement or declaration is missing its terminating semicolon.\n"
     "\n"
     "    int x = 5      // error\n"
     "    int x = 5;     // ok\n"
     "\n"
     "The error points right after the last token of the statement, which\n"
     "is where the ; belongs."},

    {"E0102",
     "A reserved word was used as a name.\n"
     "\n"
     "    int string = 5;          // error: `string` is a type\n"
     "    function new() { }       // error: `new` is a keyword\n"
     "\n"
     "Built-in type names (int, u8, string, ...) are reserved too. Pick a\n"
     "different name, like `text` or `create`."},

    {"E0103",
     "Comparisons were chained, as in `a < b < c`.\n"
     "\n"
     "In C, this compares the true/false result of `a < b` with c, which is\n"
     "almost never what was meant, so watrlang rejects it. Write both\n"
     "comparisons:\n"
     "\n"
     "    if (a < b && b < c) { ... }\n"
     "\n"
     "If you really mean to compare a comparison's result, add parentheses:\n"
     "`(a < b) == c`."},

    {"E0104",
     "A ?: expression is missing its `:` part.\n"
     "\n"
     "    int x = ok ? 1;       // error\n"
     "    int x = ok ? 1 : 0;   // ok\n"
     "\n"
     "A `?` directly after an expression can also mean error propagation\n"
     "(`open(path)?`). That reading applies only when nothing that could\n"
     "start an expression follows, so `f()?[0]` is read as a ternary. Write\n"
     "`(f()?)[0]` instead."},

    {"E0105",
     "A type name was used where a value is expected.\n"
     "\n"
     "    int x = int;          // error\n"
     "    usize n = sizeof(int); // ok: sizeof takes a type\n"
     "\n"
     "To convert a value to another type, use `as`: `x as u8`."},

    {"E0106",
     "A variable declaration is the entire body of an if, loop or defer.\n"
     "\n"
     "    if (ok) int x = 5;        // error: x would be useless\n"
     "    if (ok) { int x = 5; }    // allowed, though x is still unused\n"
     "\n"
     "The variable would go out of scope immediately, so this is almost\n"
     "always a mistake."},

    {"E0107",
     "A function, struct, class, enum, import, export or extern appears\n"
     "inside a function. These are only allowed at the top level of a file.\n"
     "\n"
     "This can also happen when an earlier function is missing its closing\n"
     "brace, which makes the next declaration look like it's inside it."},

    {"E0108",
     "`defer return` and `defer defer` aren't allowed.\n"
     "\n"
     "`defer` schedules a statement to run when the enclosing block exits.\n"
     "Returning from there, or deferring again, would make the order of\n"
     "cleanup impossible to follow."},

    {"E0109",
     "A {, ( or [ was opened but the file ended before it was closed.\n"
     "\n"
     "The error points at the opening delimiter. An editor with bracket\n"
     "matching makes these easy to find."},

    {"E0110",
     "The code nests more than 256 levels deep.\n"
     "\n"
     "This limit keeps the compiler from running out of stack on\n"
     "pathological input. Real code should never get near it; split deeply\n"
     "nested expressions into variables."},

    {"E0111",
     "A function's body doesn't match its kind.\n"
     "\n"
     "    extern function puts(u8* s): i32;      // ok: no body\n"
     "    extern function bad(): int { ... }     // error: extern has no body\n"
     "    function nobody(): int;                // error: needs a body\n"
     "\n"
     "`extern` functions are defined elsewhere (for example in C), so they\n"
     "end with `;`. Every other function needs a body."},

    {"E0112",
     "`export` or `extern` was applied to something they don't support.\n"
     "\n"
     "Both only apply to functions and global variables: `export` makes a\n"
     "symbol visible to the linker under its plain name, `extern` declares\n"
     "one that's defined elsewhere. A declaration can't be both."},

    {"E0113",
     "A struct field has a default value.\n"
     "\n"
     "    struct P { int x = 0; }   // error\n"
     "\n"
     "Structs are plain data. For default values, use a class with a\n"
     "constructor, or a struct literal: `P { x: 0 }`."},

    {"E0114",
     "A string uses {interpolation} where only a plain string is allowed:\n"
     "import paths, asm instructions and asm register names. These must be\n"
     "known at compile time."},

    {"E0115",
     "An asm operand doesn't start with in, out or inout.\n"
     "\n"
     "    asm(\"out dx, al\" : in(\"dx\") port, in(\"al\") value);\n"
     "\n"
     "`in(\"reg\") expr` loads a value into a register before the asm runs,\n"
     "`out(\"reg\") var` stores a register into a variable after it, and\n"
     "`inout` does both."},

    {"E0116",
     "A variadic `...` parameter isn't the last parameter.\n"
     "\n"
     "    extern function printf(u8* fmt, ...): i32;   // ok"},

    {"E0117",
     "Something other than a declaration appears at the top level of a file.\n"
     "\n"
     "Only functions, structs, classes, enums, imports and global variables\n"
     "can appear there. Statements like `return` or `while` have to be\n"
     "inside a function."},

    {"E0118",
     "An expression was expected, but the code has something else, like an\n"
     "operator with nothing after it:\n"
     "\n"
     "    int x = 1 + ;   // error"},

    {"E0200",
     "A variable name was used, but no variable with that name is in scope.\n"
     "\n"
     "    function main(): int {\n"
     "        int count = 1;\n"
     "        return cuont;    // error: typo\n"
     "    }\n"
     "\n"
     "Check the spelling, and check that the variable is declared before\n"
     "this point and in this block or an enclosing one. Variables declared\n"
     "inside { } stop existing at the closing brace."},

    {"E0201",
     "A function was called, but no function with that name exists.\n"
     "\n"
     "Check the spelling. Functions can be called before they're defined in\n"
     "the file; the order doesn't matter."},

    {"E0202",
     "Two top-level declarations have the same name.\n"
     "\n"
     "Functions and global variables share one namespace, so a function and\n"
     "a global can't have the same name either. watrlang has no overloading."},

    {"E0203",
     "A local variable reuses the name of another local or parameter that's\n"
     "still in scope.\n"
     "\n"
     "    function f(int n): int {\n"
     "        int n = 2;            // error: shadows the parameter\n"
     "        if (n > 0) {\n"
     "            int n = 3;        // error: shadows again\n"
     "        }\n"
     "        return n;\n"
     "    }\n"
     "\n"
     "Shadowing makes it easy to modify the wrong variable, so watrlang\n"
     "doesn't allow it inside a function. Separate blocks can reuse a name,\n"
     "and a local may reuse the name of a global."},

    {"E0204",
     "A variable was called like a function.\n"
     "\n"
     "    int total = 5;\n"
     "    int x = total();   // error\n"
     "\n"
     "Remove the parentheses to read the variable."},

    {"E0205",
     "A function name was used as a value without calling it.\n"
     "\n"
     "    int x = compute;     // error\n"
     "    int x = compute();   // ok\n"
     "\n"
     "Function pointers aren't supported yet."},

    {"E0206",
     "A function was called with the wrong number of arguments.\n"
     "\n"
     "    function add(int a, int b): int { return a + b; }\n"
     "    add(1);         // error: needs 2\n"
     "    add(1, 2, 3);   // error: needs 2\n"
     "\n"
     "watrlang has no default arguments or overloading; every parameter must\n"
     "be passed."},

    {"E0207",
     "A constant was assigned to, incremented or decremented.\n"
     "\n"
     "    const int LIMIT = 10;\n"
     "    LIMIT = 20;    // error\n"
     "\n"
     "Remove `const` from the declaration if the value needs to change."},

    {"E0208",
     "A constant was declared without a value.\n"
     "\n"
     "    const int LIMIT;        // error: can never be set\n"
     "    const int LIMIT = 10;   // ok"},

    {"E0209",
     "The left side of an assignment isn't something that can be assigned.\n"
     "\n"
     "    5 = x;          // error\n"
     "    f() = 3;        // error\n"
     "    x = 5;          // ok"},

    {"E0210",
     "An assignment was used as a value.\n"
     "\n"
     "    if (x = 5) { ... }        // error: did you mean == ?\n"
     "    int y = (x += 1);         // error\n"
     "\n"
     "In watrlang an assignment is a statement, not an expression. That\n"
     "turns C's classic `if (x = 5)` bug into a compile error. Do the\n"
     "assignment on its own line:\n"
     "\n"
     "    x += 1;\n"
     "    int y = x;\n"
     "\n"
     "`++` and `--` still produce values, so `arr[i++]` works."},

    {"E0211",
     "Assignments were chained, as in `a = b = 0`.\n"
     "\n"
     "Assignments aren't values in watrlang (see E0210). Assign each\n"
     "variable separately:\n"
     "\n"
     "    a = 0;\n"
     "    b = 0;"},

    {"E0212",
     "`break` or `continue` appears outside any loop.\n"
     "\n"
     "They only work inside while, do-while, for and foreach. To leave a\n"
     "function early, use `return`."},

    {"E0300",
     "The result of a function that returns nothing (void) was used as a\n"
     "value.\n"
     "\n"
     "    function log(): void { ... }\n"
     "    int x = log();   // error: log() has no value\n"
     "\n"
     "Call it as a statement on its own, or give the function a return type."},

    {"E0301",
     "An integer literal doesn't fit in the type it's used as.\n"
     "\n"
     "    u8 x = 256;      // error: u8 holds 0 to 255\n"
     "    i8 y = -129;     // error: i8 holds -128 to 127\n"
     "    int z = 9223372036854775808;  // error: use u64\n"
     "\n"
     "Literals take the type their context expects. To wrap a value on\n"
     "purpose, use a cast: `300 as u8` is 44."},

    {"E0302",
     "A void function returns a value.\n"
     "\n"
     "    function reset(): void {\n"
     "        return 0;   // error\n"
     "    }\n"
     "\n"
     "Either remove the value (`return;`) or give the function a return type:\n"
     "`function reset(): int`."},

    {"E0303",
     "A function with a return type has a `return` without a value.\n"
     "\n"
     "    function f(): int {\n"
     "        return;     // error: which int?\n"
     "    }"},

    {"E0304",
     "A function with a return type can reach its closing brace without\n"
     "returning a value.\n"
     "\n"
     "    function sign(int x): int {\n"
     "        if (x > 0) { return 1; }\n"
     "        if (x < 0) { return -1; }\n"
     "    }   // error: what does sign(0) return?\n"
     "\n"
     "C would return garbage here; watrlang requires every path to return.\n"
     "Loops whose condition is a constant true value (like `while (1)`) and\n"
     "that contain no `break` count as never ending, so no return is needed\n"
     "after them."},

    {"E0305",
     "The program has no main function.\n"
     "\n"
     "A program starts running at main, and main's return value becomes the\n"
     "process exit code:\n"
     "\n"
     "    function main(): int {\n"
     "        return 0;\n"
     "    }"},

    {"E0306",
     "main has the wrong signature. It must take no parameters (command-line\n"
     "arguments are coming later) and return int; the return value becomes\n"
     "the process exit code."},

    {"E0307",
     "A global variable's initializer isn't a compile-time constant.\n"
     "\n"
     "    int counter = 0;\n"
     "    int next = counter + 1;     // error: counter can change\n"
     "    const int BASE = 10;\n"
     "    int limit = BASE * 2;       // ok: BASE is constant\n"
     "\n"
     "Globals get their values before any code runs, so initializers can\n"
     "only use literals, operators and earlier `const` globals. Set other\n"
     "values at the start of main."},

    {"E0308",
     "A constant expression divides by zero or overflows a division\n"
     "(-9223372036854775808 / -1). At run time this would crash, so it's\n"
     "reported at compile time when the values are known."},

    {"E0309",
     "A value of one type was used where another type is expected.\n"
     "\n"
     "    u8 small = 300;          // error: 300 doesn't fit in u8 (see E0301)\n"
     "    int n = 1.5;             // error: expected int, found f64\n"
     "    bool b = count;          // error: expected bool, found int\n"
     "\n"
     "watrlang converts automatically only when nothing can be lost: a\n"
     "smaller integer into a larger one that holds all its values (u8 into\n"
     "int, i16 into i32), f32 into f64, T* into void*, and null into any\n"
     "pointer. Everything else needs `as`:\n"
     "\n"
     "    int n = 1.5 as int;      // 1 (truncates)\n"
     "    bool b = count != 0;     // compare to get a bool"},

    {"E0310",
     "An operator was used with operand types it doesn't support, or with\n"
     "two types that don't mix.\n"
     "\n"
     "    i32 a = 1;\n"
     "    u32 b = 2;\n"
     "    a + b;           // error: signed and unsigned don't mix\n"
     "    a + (b as i32);  // ok\n"
     "\n"
     "Arithmetic needs numbers of compatible types: the smaller side widens\n"
     "if every value fits (u8 + int is int), but signed and unsigned of the\n"
     "same size, or ints and floats, need an explicit `as`. `%`, `&`, `|`\n"
     "and `^` don't work on floats."},

    {"E0311",
     "A unary operator was applied to a type it doesn't support.\n"
     "\n"
     "    !count      // error: `!` needs a bool; write count == 0\n"
     "    ~flag       // error: `~` needs an integer; use `!` for bools\n"
     "    ++ratio     // error if ratio is a float"},

    {"E0312",
     "A condition isn't a bool.\n"
     "\n"
     "    while (1) { ... }        // error\n"
     "    while (true) { ... }     // ok\n"
     "    if (count) { ... }       // error\n"
     "    if (count != 0) { ... }  // ok\n"
     "    if (ptr != null) { ... } // ok\n"
     "\n"
     "Unlike C and PHP, watrlang doesn't treat numbers or pointers as\n"
     "true/false. Saying what you mean prevents mix-ups like testing a\n"
     "pointer when you meant the value it points to."},

    {"E0313",
     "An `as` conversion isn't allowed between these two types.\n"
     "\n"
     "`as` converts between any numeric types, from bool to an integer,\n"
     "between pointer types, and between pointers and 64-bit integers\n"
     "(usize, u64, i64, int). It can't turn numbers into bools (compare\n"
     "instead) or convert structs and arrays."},

    {"E0315",
     "Something that isn't a pointer was dereferenced with `*`, or a void*\n"
     "was dereferenced.\n"
     "\n"
     "    void* raw = get();\n"
     "    u8 b = *raw;              // error: what size is it?\n"
     "    u8 b = *(raw as u8*);     // ok"},

    {"E0316",
     "`&` was applied to something that isn't stored in memory, like a\n"
     "literal or the result of arithmetic.\n"
     "\n"
     "    int* p = &(a + b);   // error\n"
     "    int sum = a + b;\n"
     "    int* p = &sum;       // ok\n"
     "\n"
     "You can take the address of variables, fields, array elements and\n"
     "`*pointer`."},

    {"E0317",
     "A struct doesn't have a field with this name.\n"
     "\n"
     "    struct Vec2 { f64 x; f64 y; }\n"
     "    v->z = 1.0;   // error\n"
     "\n"
     "Check the spelling against the struct's declaration."},

    {"E0318",
     "`->` was used on something that isn't a struct or a pointer to one.\n"
     "\n"
     "`->` works on struct values and on pointers to structs (it follows\n"
     "the pointer for you). For a pointer to a pointer, dereference once\n"
     "first: `(*pp)->field`."},

    {"E0319",
     "`[...]` was used on something that isn't an array or a pointer.\n"
     "\n"
     "Indexing works on fixed arrays (`int[4]`) and on typed pointers\n"
     "(`int*`). A void* has to be cast first, since its element size is\n"
     "unknown."},

    {"E0320",
     "A constant index is outside the array.\n"
     "\n"
     "    int[4] xs;\n"
     "    xs[4] = 1;    // error: valid indexes are 0 to 3\n"
     "\n"
     "Indexes that aren't constant are checked when the program runs; an\n"
     "out-of-bounds index stops the program with a message saying where."},

    {"E0321",
     "An array length isn't a positive compile-time constant.\n"
     "\n"
     "    int[n] xs;          // error if n is a variable\n"
     "    const int N = 16;\n"
     "    int[N] xs;          // ok\n"
     "\n"
     "Fixed arrays have their size decided at compile time. Arrays sized at\n"
     "run time will come with allocation (milestone 6) and slices."},

    {"E0322",
     "A type name isn't known.\n"
     "\n"
     "The built-in types are int, i8, i16, i32, i64, u8, u16, u32, u64,\n"
     "usize, float, f32, f64, bool and void. Struct names can be used\n"
     "anywhere in the file, before or after the struct is declared.\n"
     "watrlang has no `char` (use u8) or `long` (use i64)."},

    {"E0323",
     "A struct contains itself, directly or through other structs, which\n"
     "would make it infinitely large.\n"
     "\n"
     "    struct Node { int value; Node next; }    // error\n"
     "    struct Node { int value; Node* next; }   // ok: a pointer has a fixed size"},

    {"E0324",
     "A struct literal sets the same field twice.\n"
     "\n"
     "    Vec2 { x: 1.0, x: 2.0 }   // error\n"
     "\n"
     "Fields that aren't listed start at zero."},

    {"E0325",
     "An array literal can't be given a type, or has the wrong number of\n"
     "elements.\n"
     "\n"
     "    int[3] xs = [1, 2, 3];   // ok\n"
     "    int[3] ys = [1, 2];      // error: every element must be listed\n"
     "    f([1, 2]);               // error unless f's parameter gives a type\n"
     "\n"
     "To start with all zeros, declare the array without a value:\n"
     "`int[256] buffer;`."},

    {"E0326",
     "A pointer conversion would drop a `const` or `volatile` qualifier.\n"
     "\n"
     "    const u8* text = ...;\n"
     "    u8* p = text;            // error: would allow writing through p\n"
     "\n"
     "Adding a qualifier is always fine (u8* into const u8*). Removing one\n"
     "needs an explicit cast, which says you know it's safe."},

    {"E0327",
     "Something was written through a `const` pointer.\n"
     "\n"
     "    function f(const int* p): void {\n"
     "        *p = 5;   // error\n"
     "    }\n"
     "\n"
     "A `const T*` promises not to modify what it points to. Take a plain\n"
     "`T*` if the function needs to write."},

    {"E0328",
     "A negative value was used with an unsigned type, or an unsigned value\n"
     "was negated.\n"
     "\n"
     "    u8 x = -1;        // error\n"
     "    u8 y = 255;       // ok\n"
     "    u8 z = 0 - x;     // ok: wrapping subtraction\n"
     "    u32 all = -1 as u32;  // ok: casts wrap, giving 4294967295"},

    {"E0330",
     "`void` was used where a real type is needed: as a variable type, or\n"
     "as the target of pointer arithmetic.\n"
     "\n"
     "`void` only appears as a function's return type and in `void*`, a\n"
     "pointer to memory of unknown type. To move a void* by bytes, cast it\n"
     "to `u8*` first."},

    {"E0331",
     "A struct declares two fields with the same name."},

    {"E0332",
     "`len` was called on something that isn't a fixed array.\n"
     "\n"
     "    int[8] xs;\n"
     "    len(xs);    // 8, known at compile time\n"
     "\n"
     "A pointer doesn't know how many elements it points to, so pass the\n"
     "length alongside it."},

    {"E0333",
     "A function returns the address of one of its own local variables or\n"
     "parameters.\n"
     "\n"
     "    function make(): int* {\n"
     "        int x = 5;\n"
     "        return &x;   // error: x is gone once make() returns\n"
     "    }\n"
     "\n"
     "The memory is reused by the next function call, so the pointer would\n"
     "point at garbage. Return the value itself, or have the caller pass in\n"
     "a pointer to fill."},

    {"E0334",
     "A watrlang function was declared with `...`.\n"
     "\n"
     "Variadic parameters exist so watrlang can call C functions like\n"
     "printf, so only `extern` functions can have them:\n"
     "\n"
     "    extern function printf(const u8* fmt, ...): i32;   // ok"},

    {"E0335",
     "An array was passed to or returned from a function by value.\n"
     "\n"
     "As in C, arrays aren't passed by value. Pass a pointer to the first\n"
     "element, plus the length:\n"
     "\n"
     "    function sum(int* xs, usize n): int { ... }\n"
     "    sum(&nums[0], len(nums));\n"
     "\n"
     "Structs (including ones that contain arrays) can be passed by value."},

    {"E0336",
     "An `extern` or `export` symbol is named like an x86 register (rax,\n"
     "eax, rsp, xmm0, ...).\n"
     "\n"
     "In assembly, `call rax` calls the address in the register, not a\n"
     "function called rax. watrlang renames its own functions internally to\n"
     "avoid this, but extern and export names have to stay as written so\n"
     "C code can find them."},

    {"E0337",
     "`main` was marked `extern` or `export`.\n"
     "\n"
     "The compiler wires `main` up as the program's entry point itself:\n"
     "with libc it becomes C's main, and without it, it's called from the\n"
     "program's own startup code."},

    {"E0900",
     "This feature is part of watrlang's design but isn't implemented yet.\n"
     "The message names the milestone that will add it; docs/DESIGN.md has\n"
     "the roadmap."},

    {"W0001",
     "A variable or parameter is never read.\n"
     "\n"
     "    int unused = compute();   // warning\n"
     "\n"
     "Remove it, or if it's intentional (for example a parameter required by\n"
     "a signature), start its name with an underscore: `_unused`. Variables\n"
     "that are only ever assigned to also count as unused."},

    {"W0002",
     "A statement can never run because it comes after a return, break or\n"
     "continue in the same block.\n"
     "\n"
     "    return x;\n"
     "    x++;        // warning: unreachable"},
};

const char *diag_explain(const char *code) {
    for (size_t i = 0; i < sizeof explanations / sizeof explanations[0]; i++)
        if (strcmp(explanations[i].code, code) == 0) return explanations[i].text;
    return NULL;
}
