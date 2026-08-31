#include "otaku/module.hpp"

#include <memory>
#include <string>

namespace otaku {

// Module factory. Placeholder in step 0; real modules are registered in
// later steps. `simulate` selects simulated data sources for `preview`.
std::shared_ptr<IModule> create_module(const std::string& name, bool simulate) {
    (void)simulate;
    return nullptr;  // no modules registered yet
}

}  // namespace otaku
