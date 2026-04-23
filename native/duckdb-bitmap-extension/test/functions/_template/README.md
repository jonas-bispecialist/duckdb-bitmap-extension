# Template Case

Copy this folder to create a new function-level SQL test case.

- Put fixture SQL in `setup.sql`.
- Put the validation query in `query.sql`.
- Capture the expected CSV output in `expected.csv`.

The runner will skip `_template` itself.
