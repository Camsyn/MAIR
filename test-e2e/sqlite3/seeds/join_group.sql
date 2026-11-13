-- Empty teams remain visible through the LEFT JOIN.
PRAGMA foreign_keys=ON;
CREATE TABLE teams(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE scores(team_id INTEGER REFERENCES teams(id), player TEXT, points INTEGER);
INSERT INTO teams VALUES(1,'red'),(2,'blue'),(3,'empty');
INSERT INTO scores VALUES(1,'amy',10),(1,'bo',20),(2,'cy',7),(2,'dee',NULL);
CREATE INDEX scores_team ON scores(team_id,points);
SELECT t.name,count(s.player),coalesce(sum(s.points),0),max(s.points)
FROM teams t LEFT JOIN scores s ON s.team_id=t.id
GROUP BY t.id,t.name ORDER BY t.id;
SELECT t.name,sum(s.points) AS total
FROM teams t JOIN scores s ON s.team_id=t.id
GROUP BY t.name HAVING sum(s.points)>=10 ORDER BY total DESC;
SELECT player FROM scores WHERE team_id IN (SELECT id FROM teams WHERE name='red') ORDER BY player;
