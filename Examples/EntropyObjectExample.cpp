#define NOMINMAX
#include <EntropyCore.h>

#include <vector>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Logging;

class GameObject : public EntropyObject
{
    ENTROPY_CLASS_BODY(GameObject)

    std::string _name;

public:
    explicit GameObject(std::string name) : _name(std::move(name)) {}

    std::string toString() const override {
        return std::format("GameObject('{}')@{}", _name, static_cast<const void*>(this));
    }

    const std::string& name() const {
        return _name;
    }
};

int main() {
    // Container of owning references
    std::vector<RefObject<GameObject>> objects;
    objects.emplace_back(makeRef<GameObject>("Player"));
    objects.emplace_back(makeRef<GameObject>("Enemy"));

    // Reference counted sharing
    auto shared = makeRef<GameObject>("PowerUp");
    std::vector<RefObject<GameObject>> references;
    references.push_back(shared);
    references.push_back(shared);

    ENTROPY_LOG_INFO_CAT("Example", std::format("Shared object refcount: {}", shared->refCount()));
    ENTROPY_LOG_INFO_CAT("Example", std::format("Object info: {}", shared->debugString()));

    // shared_ptr interop
    auto sp = toSharedPtr(shared);
    ENTROPY_LOG_INFO_CAT("Example", std::format("Can convert to shared_ptr: {}", sp != nullptr));

    return 0;
}
