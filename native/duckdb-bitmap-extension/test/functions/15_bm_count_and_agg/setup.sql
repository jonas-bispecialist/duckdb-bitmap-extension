CREATE TABLE count_and_agg_input(name VARCHAR, id UBIGINT);
INSERT INTO count_and_agg_input VALUES
    ('a', 1),
    ('a', 5),
    ('a', 9),
    ('a', 11),
    ('b', 5),
    ('b', 7),
    ('b', 9),
    ('b', 11),
    ('c', 5),
    ('c', 9),
    ('c', 13);

CREATE TABLE count_and_agg_postings AS
SELECT name, bm_build_agg(id) AS bm
FROM count_and_agg_input
GROUP BY name;
