CREATE TABLE posting_input(name VARCHAR, id UBIGINT);
INSERT INTO posting_input VALUES
    ('a', 1),
    ('a', 3),
    ('b', 2),
    ('b', 3),
    ('b', 4),
    ('c', 6);

CREATE TABLE postings AS
SELECT name, bm_build_agg(id) AS bm
FROM posting_input
GROUP BY name;
