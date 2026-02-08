#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <string>
#include <unordered_map>
#include <memory>
#include "table.hpp"

class Database {
    private:
        std::string db_directory;
        std::unordered_map<std::string, std::unique_ptr<Table>> open_tables; 

    public:
        Table* get_table(std::string table_name) {
            if(open_tables.find(table_name) == open_tables.end()) {
                open_tables[table_name] = std::make_unique<Table>(table_name + ".bin");
            }
            return open_tables[table_name].get();
        }
};


#endif