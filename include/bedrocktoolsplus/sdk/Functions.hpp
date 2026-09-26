#pragma once

#include <bedrocktoolsplus/Api.hpp>
#include <bedrocktoolsplus/memory/Signatures.hpp>

namespace bedrocktoolsplus::sdk {

template <class Function>
Function function(memory::SignatureId id, const api::ApiV1* runtime = nullptr) {
    if (!runtime) runtime = api::find();
    const auto address = api::resolve(id, runtime);
    return address ? reinterpret_cast<Function>(address) : nullptr;
}

}
