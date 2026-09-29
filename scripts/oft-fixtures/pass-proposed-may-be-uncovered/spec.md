# Pass

### Feature
`feat~fixture~1`

Needs: req

### Implemented criterion
`req~fixture.done~1`

Implemented and tested.

Covers:
- feat~fixture~1

Needs: impl, utest

### Criterion of a task not implemented yet
`req~fixture.todo~1`
Status: proposed

Not implemented yet; a design item already covers it.

Covers:
- feat~fixture~1

Depends:
- req~fixture.done~1

Needs: dsn, utest

### Design for the pending criterion
`dsn~fixture.todo-design~1`
Status: proposed

Covers:
- req~fixture.todo~1

Needs: impl
