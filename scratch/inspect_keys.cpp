#include <iostream>
#include "asos/Blackboard.hpp"
#include "engine/store.hpp"

int main() {
    asos::Blackboard bb("asos_db", 1);
    auto* store = bb.get_engine()->get_store();
    
    std::cout << "[DEBUG] Listing ALL keys in substrate:" << std::endl;
    
    for (size_t i = 0; i < 8; ++i) { // Assuming 8 shards
        // We can't iterate directly, but we can try common prefixes
        std::string prefixes[] = {"n:", "e:", "meta:", "wbs:", "eu:", "i:", "p:"};
        for (const auto& p : prefixes) {
            auto keys = store->get_prefix_keys_all_shards(p, p, 1000);
            for (const auto& k : keys) {
                std::cout << "  " << k << std::endl;
            }
        }
    }
    return 0;
}
