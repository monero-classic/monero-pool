-- --------------------------------------------------------
-- 主机:                           192.168.0.68
-- 服务器版本:                        5.7.27-0ubuntu0.18.04.1 - (Ubuntu)
-- 服务器操作系统:                      Linux
-- HeidiSQL 版本:                  9.4.0.5125
-- --------------------------------------------------------

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET NAMES utf8 */;
/*!50503 SET NAMES utf8mb4 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;


-- 导出 xmcpool 的数据库结构
CREATE DATABASE IF NOT EXISTS `xmcpool` /*!40100 DEFAULT CHARACTER SET latin1 */;
USE `xmcpool`;

-- 导出  表 xmcpool.block 结构
CREATE TABLE IF NOT EXISTS `block` (
  `id` bigint(20) NOT NULL AUTO_INCREMENT,
  `height` bigint(20) DEFAULT NULL,
  `hash` varchar(64) DEFAULT NULL,
  `prevhash` varchar(64) DEFAULT NULL,
  `difficulty` bigint(20) DEFAULT NULL,
  `status` int(11) DEFAULT NULL,
  `reward` int(11) DEFAULT NULL ,
  `timestamp` bigint(20) DEFAULT NULL,
	`name` varchar(50) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- 数据导出被取消选择。
-- 导出  表 xmcpool.share 结构
CREATE TABLE IF NOT EXISTS `share` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `height` bigint(20) DEFAULT NULL,
  `difficulty` bigint(20) DEFAULT NULL,
  `address` varchar(100) DEFAULT NULL,
  `timestamp` bigint(20) DEFAULT NULL,
	`name` varchar(50) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- 数据导出被取消选择。
/*!40101 SET SQL_MODE=IFNULL(@OLD_SQL_MODE, '') */;
/*!40014 SET FOREIGN_KEY_CHECKS=IF(@OLD_FOREIGN_KEY_CHECKS IS NULL, 1, @OLD_FOREIGN_KEY_CHECKS) */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
