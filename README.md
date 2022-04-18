## ZetaSQL Formatter

[![build](https://github.com/Matts966/zetasql-formatter/workflows/Build/badge.svg?branch=main)](https://github.com/Matts966/zetasql-formatter/actions?query=branch%main+workflow%3ABuild+)

<p align="center">
  <img src="./docs/changes.png">
</p>

This repository is forked from [google/zetasql](https://github.com/google/zetasql) and provides SQL formatter with preserved comments. This formatter can be applied to mainly BigQuery and SpanSQL.

## Quick Start

```bash
# To install for MacOSX
wget https://github.com/Matts966/zetasql-formatter/releases/latest/download/zetasql-formatter_darwin_amd64.zip \
  && sudo unzip zetasql-formatter_darwin_amd64.zip -d /usr/local/bin
```

```bash
# To install for Linux
wget https://github.com/Matts966/zetasql-formatter/releases/latest/download/zetasql-formatter_linux_x86_64.zip \
  && sudo unzip zetasql-formatter_linux_x86_64.zip -d /usr/local/bin
```

```bash
# To apply formatter for files
$ zetasql-formatter [files and directories]

# Format stdin
$ echo "select * from test" | zetasql-formatter
SELECT
  *
FROM
  test;

$ zetasql-formatter
select * from ok;
-- CTRL-D
SELECT
  *
FROM
  ok;
-- CTRL-D
```

## Integration with [efm-langserver](https://github.com/mattn/efm-langserver)

- Install efm-langserver
- Locate [`config.yaml`](https://github.com/mattn/efm-langserver#example-for-configyaml) like below

```yaml
version: 2
tools:
  zetasql-formatter: &zetasql-formatter
    format-command: zetasql-formatter
    format-stdin: true
languages:
  sql:
    - <<: *zetasql-formatter
  sql-bigquery:
    - <<: *zetasql-formatter
```

## License

[Apache License 2.0](LICENSE)

## Sponsors

The development of this formatter is sponsored by the Japan Data Science Consortium.
