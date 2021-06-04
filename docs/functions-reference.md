
<!-- This file is auto-generated. DO NOT EDIT.                               -->

# Function reference

This page explains ZetaSQL expressions, including functions and
operators.

# Function call rules

The following rules apply to all functions unless explicitly indicated otherwise
in the function description:

+ Integer types coerce to INT64.
+ For functions that accept numeric types, if one operand is a floating point
  operand and the other operand is another numeric type, both operands are
  converted to DOUBLE before the function is
  evaluated.
+ If an operand is `NULL`, the result is `NULL`, with the exception of the
  IS operator.
+ For functions that are time zone sensitive (as indicated in the function
  description), the default time zone, which is implementation defined, is used if a time
  zone is not specified.

## Lambdas 
<a id="lambdas"></a>

**Syntax:**

```sql
(arg[, ...]) -> body_expression
```

```sql
arg -> body_expression
```

**Description**

For some functions, ZetaSQL supports lambdas as builtin function
arguments. A lambda takes a list of arguments and an expression as the lambda
body.

+   `arg`:
    +   Name of the lambda argument is defined by the user.
    +   No type is specified for the lambda argument. The type is inferred from
        the context.
+   `body_expression`:
    +   The lambda body can be any valid scalar expression.

## SAFE. prefix

**Syntax:**

```
SAFE.function_name()
```

**Description**

If you begin a function with the `SAFE.` prefix, it will return `NULL` instead
of an error. The `SAFE.` prefix only prevents errors from the prefixed function
itself: it does not prevent errors that occur while evaluating argument
expressions. The `SAFE.` prefix only prevents errors that occur because of the
value of the function inputs, such as "value out of range" errors; other
errors, such as internal or system errors, may still occur. If the function
does not return an error, `SAFE.` has no effect on the output.

[Operators][link-to-operators], such as `+` and `=`, do not support the `SAFE.`
prefix. To prevent errors from a division
operation, use [SAFE_DIVIDE][link-to-SAFE_DIVIDE]. Some operators,
such as `IN`, `ARRAY`, and `UNNEST`, resemble functions, but do not support the
`SAFE.` prefix. The `CAST` and `EXTRACT` functions also do not support the
`SAFE.` prefix. To prevent errors from casting, use
[SAFE_CAST][link-to-SAFE_CAST].

**Example**

In the following example, the first use of the `SUBSTR` function would normally
return an error, because the function does not support length arguments with
negative values. However, the `SAFE.` prefix causes the function to return
`NULL` instead. The second use of the `SUBSTR` function provides the expected
output: the `SAFE.` prefix has no effect.

```sql
SELECT SAFE.SUBSTR('foo', 0, -2) AS safe_output UNION ALL
SELECT SAFE.SUBSTR('bar', 0, 2) AS safe_output;

+-------------+
| safe_output |
+-------------+
| NULL        |
| ba          |
+-------------+
```

[link-to-SAFE_DIVIDE]: #safe_divide
[link-to-SAFE_CAST]: #safe_casting
[link-to-operators]: #operators

