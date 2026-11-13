-- Bounded recursion: at most eight generated rows and twelve output rows.
CREATE TABLE samples(id INTEGER PRIMARY KEY, bucket TEXT, value INTEGER);
WITH RECURSIVE seq(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM seq WHERE n<8)
INSERT INTO samples SELECT n,CASE WHEN n%2=0 THEN 'even' ELSE 'odd' END,n*n FROM seq;
SELECT id,bucket,value,
       row_number() OVER (PARTITION BY bucket ORDER BY id),
       sum(value) OVER (PARTITION BY bucket ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW),
       lag(value,1,0) OVER (PARTITION BY bucket ORDER BY id)
FROM samples ORDER BY id;
WITH totals AS (SELECT bucket,sum(value) AS total FROM samples GROUP BY bucket)
SELECT bucket,total,dense_rank() OVER (ORDER BY total DESC) FROM totals ORDER BY bucket;
WITH RECURSIVE factorial(n,v) AS (VALUES(1,1) UNION ALL SELECT n+1,v*(n+1) FROM factorial WHERE n<6)
SELECT max(n),max(v) FROM factorial;
