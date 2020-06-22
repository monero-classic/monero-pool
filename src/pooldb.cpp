#include "pooldb.h"
#include <string>
#include <iostream>
#include <mysql/mysql.h>
#include <string.h>
#include <vector>
static MYSQL s_mysql;
static std::string s_url = "127.0.0.1";
static std::string s_db_user = "root";
static std::string s_db_pass = "a";
static std::string s_db_name = "xmcpool";
static std::vector<std::string> s_vect_sql;

static bool open_db()
{

	if (mysql_init(&s_mysql) == NULL)
	{
		return false;
	}

	if (!mysql_real_connect(&s_mysql, s_url.c_str(), s_db_user.c_str(),
				s_db_pass.c_str(),s_db_name.c_str(),
				3306, NULL, 0))
	{
		std::string error= mysql_error(&s_mysql);
		std::cerr << error << std::endl;
		return false;
	}
	return true;
}

static bool close_db()
{
	mysql_close(&s_mysql);
	return true;
}

bool batch_sql()
{
	open_db();
	mysql_query(&s_mysql,"START TRANSACTION");
	std::string sql;
	for(uint32_t i = 0; i < s_vect_sql.size(); i ++)
	{
		sql = s_vect_sql[i];
		mysql_real_query(&s_mysql, sql.c_str(), strlen(sql.c_str()));      	
	}
	s_vect_sql.clear();
	mysql_query(&s_mysql,"COMMIT");
	close_db();

}

static bool batch_flush(const std::vector<std::string>& vect_sql)
{
	open_db();
	mysql_query(&s_mysql,"START TRANSACTION");
	std::string sql;
	for(uint32_t i = 0; i < vect_sql.size(); i ++)
	{
		sql = vect_sql[i];
		mysql_real_query(&s_mysql, sql.c_str(), strlen(sql.c_str()));      	
	}
	mysql_query(&s_mysql,"COMMIT");
	close_db();
}

bool add_share_to_db(uint64_t height, uint64_t difficulty, const char* address, uint64_t timestamp)
{
	std::string sql_prefix = "INSERT INTO `share` (`height`, `difficulty`, `address`, `timestamp`, `name`) VALUES";
	std::string sql_value = "('" + std::to_string(height) + "' , '" +std::to_string(difficulty) + "' , '" + address + "' , '" +std::to_string(timestamp) +"' , 'B2B');";
	std::string sql = sql_prefix + sql_value;
	s_vect_sql.push_back(sql);
	return true;
}

bool add_block_to_db(uint64_t height, const char* hash, const char* prevhash, uint64_t difficulty, uint32_t status, uint64_t reward, uint64_t timestamp)
{
	std::string sql_prefix = "INSERT INTO `block` (`height`, `hash`, `prevhash`, `difficulty`, `status`, `reward`, `timestamp`, `name`) VALUES ";
	std::string str_hash = hash;
	str_hash = str_hash.substr(0,128);
	std::string str_prevhash = prevhash;
	str_prevhash = str_prevhash.substr(0,64);
	std::string sql_value = "('" + std::to_string(height) + "' , '" + str_hash + "' , '" + str_prevhash + "' , '" + std::to_string(difficulty) + "' , '" + std::to_string(status) + "' , '" + std::to_string(reward) + "' , '" + std::to_string(timestamp) +  "', 'B2B');";
	std::string sql = sql_prefix + sql_value;

	s_vect_sql.push_back(sql);
	return true;
}


