CREATE TABLE IF NOT EXISTS `abyssal_storage` (
  `account_id` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL,
  `count` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`account_id`, `item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
