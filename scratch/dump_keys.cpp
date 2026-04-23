#include <iostream>
#include "asos/Blackboard.hpp"
#include "engine/store.hpp"

int main() {
    try {
        std::cout << "[DEBUG] Dumping all keys in asos_db..." << std::endl;
        asos::Blackboard bb("asos_db", 1);
        auto* store = bb.get_engine()->get_store();
        
        auto keys = store->get_prefix_keys_all_shards("", "", 5000);
        std::cout << "Found " << keys.size() << " keys." << std::endl;
        for (const auto& k : keys) {
            std::cout << "  " << k << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}
