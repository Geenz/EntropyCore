#include <stdio.h>
#include <inttypes.h>
#include <Core/entropy_c_api.h>

// Dummy owner and vtable callbacks for demonstration
static int dummy_validate(const void* owner, uint32_t index, uint32_t generation) {
    // No backing store; nothing is valid in this minimal example
    (void)owner; (void)index; (void)generation;
    return 0;
}
static EntropyObjectRef* dummy_resolve(const void* owner, uint32_t index, uint32_t generation) {
    // No backing store; cannot resolve
    (void)owner; (void)index; (void)generation;
    return NULL;
}

int main(void) {
    uint32_t maj=0, min=0, pat=0, abi=0;
    entropy_get_version(&maj, &min, &pat, &abi);
    printf("EntropyCore C API version: %u.%u.%u (ABI %u)\n", maj, min, pat, abi);

    // Register a dummy owner vtable (for demonstration of handle flow)
    const void* dummy_owner = (const void*)0xDEADBEEF;
    entropy_register_owner_vtable(dummy_owner, dummy_resolve, dummy_validate);

    // Construct a handle value (no real object behind it in this example)
    EntropyHandle h = { dummy_owner, 42u, 7u, 0u };

    // Basic handle operations
    printf("Handle valid? %d\n", entropy_handle_is_valid(h));
    EntropyHandle h2 = h;
    printf("Handles equal? %d\n", entropy_handle_equals(h, h2));
    printf("Type matches (expected 0)? %d\n", entropy_handle_type_matches(h, 0));

    // Attempt to resolve (will be NULL in this minimal example)
    EntropyObjectRef* obj = entropy_resolve_handle(h);
    printf("Resolved object: %p (expected NULL)\n", (void*)obj);
    if (obj) {
        // If your application registered a real owner, you would get a retained object here
        entropy_object_release(obj);
    }

    // Allocation helpers
    char* buf = (char*)entropy_alloc(32);
    if (buf) {
        for (int i=0;i<31;i++) buf[i] = (i%10)+'0';
        buf[31] = '\0';
        printf("Allocated buffer: %s\n", buf);
        entropy_free(buf);
    }

    return 0;
}
