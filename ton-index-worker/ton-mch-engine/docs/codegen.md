# Code generation contracts

`mch-codegen` turns the version-controlled IR artifacts into deterministic C++
tables and functions. Generated files live in the build tree. The production
classifier uses generated matcher, `where`, and build-program tables, while the
expression and build-vector tables belong to the fixture harness.

Expression code uses preorder-numbered SSA temporaries and the same runtime
operations as the interpreter. Faults propagate as `EvalResult` except at the
outer level of a build function, where they become clean build rejections.
Comprehension bodies remain `EvalResult` lambdas so their faults can pass through
the shared map and quantifier operations.

Each generated table is paired with its input document by a source hash and a
symbol suffix. Several documents can therefore link into the same binary without
manual table selection. Unordered containers are sorted before emission so a
regeneration is byte-identical for the same input.

The string-literal emitter uses named escapes where available and fixed-width
octal escapes for other control bytes. Fixed width prevents a following digit or
hexadecimal character from extending the escape.
