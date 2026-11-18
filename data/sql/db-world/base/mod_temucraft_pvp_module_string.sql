-- mod-temucraft-pvp: player-facing texts. Translations go in module_string_locale (same module and id, plus the locale).
DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-pvp';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-pvp', 1, '|cffff8000Honorable kill:|r {} experience.'),
('mod-temucraft-pvp', 2, '|cffff8000Battleground:|r {} experience.'),
('mod-temucraft-pvp', 3, '|cffff8000Close game!|r An extra mark for a hard fight.');
