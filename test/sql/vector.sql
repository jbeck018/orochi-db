-- Vector operations tests
-- Test vector type, distance functions, and similarity search

CREATE EXTENSION orochi;

-- Test: Vector type creation
SELECT '[1.0, 2.0, 3.0]'::vector AS vec;
SELECT '[0.5, 0.5, 0.5, 0.5]'::vector AS vec4d;

-- Test: Vector dimensions
SELECT vector_dims('[1.0, 2.0, 3.0]'::vector) AS dims;

-- Test: Vector norm
SELECT vector_norm('[3.0, 4.0]'::vector) AS norm; -- Should be 5.0

-- Test: L2 distance
SELECT l2_distance(
    '[1.0, 0.0, 0.0]'::vector,
    '[0.0, 1.0, 0.0]'::vector
) AS l2_dist; -- Should be sqrt(2) ≈ 1.414

-- Test: Cosine distance
SELECT cosine_distance(
    '[1.0, 0.0]'::vector,
    '[0.0, 1.0]'::vector
) AS cos_dist; -- Should be 1.0 (orthogonal)

SELECT cosine_distance(
    '[1.0, 1.0]'::vector,
    '[1.0, 1.0]'::vector
) AS cos_dist_same; -- Should be 0.0 (identical)

-- Test: Inner product
SELECT inner_product(
    '[1.0, 2.0, 3.0]'::vector,
    '[4.0, 5.0, 6.0]'::vector
) AS ip; -- Should be 1*4 + 2*5 + 3*6 = 32

-- Test: Vector arithmetic
SELECT '[1.0, 2.0, 3.0]'::vector + '[1.0, 1.0, 1.0]'::vector AS vec_add;
SELECT '[5.0, 4.0, 3.0]'::vector - '[1.0, 1.0, 1.0]'::vector AS vec_sub;

-- Test: Normalize
SELECT normalize('[3.0, 4.0]'::vector) AS normalized; -- Should be [0.6, 0.8]

-- Test: Create embeddings table
CREATE TABLE documents (
    id BIGSERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    embedding vector(4)  -- Small dimension for testing
);

-- Test: Insert vectors
INSERT INTO documents (title, content, embedding) VALUES
    ('Doc 1', 'About cats', '[1.0, 0.0, 0.0, 0.0]'),
    ('Doc 2', 'About dogs', '[0.9, 0.1, 0.0, 0.0]'),
    ('Doc 3', 'About birds', '[0.0, 1.0, 0.0, 0.0]'),
    ('Doc 4', 'About fish', '[0.0, 0.0, 1.0, 0.0]'),
    ('Doc 5', 'About cats and dogs', '[0.7, 0.7, 0.0, 0.0]');

-- Test: Nearest neighbor search (L2)
SELECT
    id,
    title,
    embedding <-> '[1.0, 0.0, 0.0, 0.0]'::vector AS distance
FROM documents
ORDER BY embedding <-> '[1.0, 0.0, 0.0, 0.0]'::vector
LIMIT 3;

-- Test: Nearest neighbor search (Cosine)
SELECT
    id,
    title,
    embedding <=> '[1.0, 0.0, 0.0, 0.0]'::vector AS cos_distance
FROM documents
ORDER BY embedding <=> '[1.0, 0.0, 0.0, 0.0]'::vector
LIMIT 3;

-- Test: Range query
SELECT id, title
FROM documents
WHERE embedding <-> '[1.0, 0.0, 0.0, 0.0]'::vector < 0.5;

-- Test: Aggregate - average embedding
SELECT
    normalize(
        SUM(embedding)::vector
    ) AS centroid
FROM documents;

-- Test: Vector comparison operators
SELECT
    '[1.0, 2.0]'::vector = '[1.0, 2.0]'::vector AS eq,
    '[1.0, 2.0]'::vector <> '[1.0, 3.0]'::vector AS neq;

-- Test: Large dimension vector
SELECT vector_dims('[' || array_to_string(array_agg(n::text), ',') || ']'
FROM generate_series(1, 1536) n)::vector) AS dims;

-- Cleanup
DROP TABLE documents;
DROP EXTENSION orochi CASCADE;
