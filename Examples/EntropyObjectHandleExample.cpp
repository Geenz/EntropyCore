#define NOMINMAX
#include <EntropyCore.h>
#include <vector>
#include <optional>
#include <thread>
#include <chrono>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Logging;
using namespace std::chrono_literals;

// A simple EntropyObject-derived type
class GameObject : public EntropyObject {
    ENTROPY_CLASS_BODY(GameObject)
    std::string _name;
public:
    explicit GameObject(std::string name) : _name(std::move(name)) {}
    std::string toString() const override {
        if (hasHandle()) {
            return std::format("GameObject('{}')#({:p}, idx={}, gen={})", _name, handleOwner(), handleIndex(), handleGeneration());
        } else {
            return std::format("GameObject('{}')@{:p}", _name, static_cast<const void*>(this));
        }
    }
    const std::string& name() const { return _name; }
};

// Tag type for typed handle
struct GameObjectTag {};

// A tiny owner/registry that stamps handle identity onto objects
class GameObjectPool {
public:
    using Handle = TypeSystem::TypedHandle<GameObjectTag, GameObjectPool>;

private:
    struct Slot {
        RefObject<GameObject> obj;     // owning reference
        uint32_t generation = 1;       // start at 1 so {0,0} remains invalid
        bool occupied = false;
    };

    std::vector<Slot> _slots;

public:
    explicit GameObjectPool(size_t capacity = 16) : _slots(capacity) {}

    // Insert returns a typed handle; stamps identity on the object
    Handle insert(RefObject<GameObject> obj) {
        // Find a free slot
        uint32_t index = 0;
        for (; index < _slots.size(); ++index) {
            if (!_slots[index].occupied) break;
        }
        if (index == _slots.size()) {
            _slots.emplace_back();
        }
        Slot& s = _slots[index];
        s.obj = std::move(obj);
        s.occupied = true;
        // Stamp identity (owner, index, generation)
        if (s.obj) {
            HandleAccess::set(*s.obj.get(), this, index, s.generation);
        }
        return Handle(this, index, s.generation);
    }

    // Validate typed handle
    bool validate(const Handle& h) const noexcept {
        if (h.getOwner() != this) return false;
        uint32_t idx = h.getIndex();
        if (idx >= _slots.size()) return false;
        const Slot& s = _slots[idx];
        return s.occupied && s.generation == h.getGeneration();
    }

    // Resolve typed handle to a retained reference (copy of RefObject)
    RefObject<GameObject> resolve(const Handle& h) const noexcept {
        if (!validate(h)) return {};
        return _slots[h.getIndex()].obj; // copy retains
    }

    // Erase object by handle (clears identity, bumps generation)
    void erase(const Handle& h) noexcept {
        if (!validate(h)) return;
        Slot& s = _slots[h.getIndex()];
        if (s.obj) {
            HandleAccess::clear(*s.obj.get());
        }
        s.obj = {};
        s.occupied = false;
        // Bump generation so stale handles become invalid
        s.generation++;
        if (s.generation == 0) s.generation = 1; // avoid 0
    }

    // Convenience: make a handle from a live object that is currently stamped
    std::optional<Handle> toHandle(const RefObject<GameObject>& obj) const noexcept {
        if (!obj || !obj->hasHandle()) return std::nullopt;
        if (obj->handleOwner() != this) return std::nullopt;
        return Handle(const_cast<GameObjectPool*>(this), obj->handleIndex(), obj->handleGeneration());
    }
};

int main() {
    // The global logger is auto-configured with a console sink.
    Logger::global().setMinLevel(Logging::LogLevel::Trace);

    GameObjectPool pool(4);

    // Create a couple of objects
    auto player = makeRef<GameObject>("Player");
    auto enemy  = makeRef<GameObject>("Enemy");

    // Insert into the pool; this stamps handle identity on the objects
    auto hPlayer = pool.insert(player);
    auto hEnemy  = pool.insert(enemy);

    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("Player debug: {}", player->debugString()));
    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("Enemy  debug: {}", enemy->debugString()));

    // Resolve handles back to objects
    auto playerRef = pool.resolve(hPlayer);
    auto enemyRef  = pool.resolve(hEnemy);

    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("Resolved Player valid: {}", static_cast<bool>(playerRef)));
    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("Resolved Enemy  valid: {}", static_cast<bool>(enemyRef)));

    // Demonstrate object -> handle via stamped identity
    if (auto objHandle = pool.toHandle(player)) {
        ENTROPY_LOG_INFO_CAT("HandleExample", std::format("Object->Handle roundtrip: index={} gen={}", objHandle->getIndex(), objHandle->getGeneration()));
    }

    // Erase the enemy; its handle should become invalid after generation bump
    pool.erase(hEnemy);
    auto enemyAfterErase = pool.resolve(hEnemy);
    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("After erase, old enemy handle valid: {}", static_cast<bool>(enemyAfterErase)));

    // Re-insert a new object; likely reuses the same slot but with new generation
    auto newEnemy = makeRef<GameObject>("Enemy#2");
    auto hEnemy2 = pool.insert(newEnemy);

    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("New enemy debug: {}", newEnemy->debugString()));
    ENTROPY_LOG_INFO_CAT("HandleExample", std::format("Old enemy handle index/gen = {}/{}; New handle index/gen = {}/{}",
        hEnemy.getIndex(), hEnemy.getGeneration(), hEnemy2.getIndex(), hEnemy2.getGeneration()));

    // Small pause to make logs readable if needed
    std::this_thread::sleep_for(10ms);

    return 0;
}
