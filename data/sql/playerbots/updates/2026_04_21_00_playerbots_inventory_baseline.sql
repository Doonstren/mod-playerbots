-- #########################################################
-- Playerbots - Inventory baseline table for `s save/keep/reset`
-- Stores per-bot snapshot of item entries that must never be
-- auto-sold or auto-destroyed by the `s keep` cleanup command.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_inventory_baseline` (
  `guid` INT(11) NOT NULL,
  `entry` INT(11) NOT NULL,
  PRIMARY KEY (`guid`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
