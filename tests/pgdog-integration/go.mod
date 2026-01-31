module github.com/orochi-db/tests/pgdog-integration

go 1.22

require (
	github.com/google/uuid v1.6.0
	github.com/lib/pq v1.10.9
	github.com/orochi-db/pgdog-router/pkg/hash v0.0.0
)

replace github.com/orochi-db/pgdog-router/pkg/hash => ../../services/pgdog-router/pkg/hash
