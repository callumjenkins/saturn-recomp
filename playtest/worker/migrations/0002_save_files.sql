-- What a session's launcher says of it beside the zip, for the save files: its record (the save file
-- it belongs to, the point it branched from, which dump it has) and its progress lines.
ALTER TABLE sessions ADD COLUMN meta TEXT;
ALTER TABLE sessions ADD COLUMN progress TEXT;
