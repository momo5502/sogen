// Exercises the IOKit client surface sogen has to answer, one routine per phase, with a marker
// syscall between phases so an lldb trace can attribute each mach_msg2 to the call that made it.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <unistd.h>

static void phase(const char* name)
{
    fprintf(stderr, "@@PHASE %s\n", name);
    fflush(stderr);
}

// The legacy and _bin MIG stubs have no exported symbol; a modern IOKit picks _bin_buf and never
// calls them, so the only way to see their wire shape is to reach them by address. The deltas are
// from IOServiceGetMatchingService in the same image, read out of the shared cache with
// `image lookup -rn '^io_registry_entry_get_property$' IOKit` (25G76, arm64).
static void probe_private_stubs(io_service_t entry)
{
    const char* base = (const char*)(void*)&IOServiceGetMatchingService;
    int (*get_property)(unsigned, const char*, char**, unsigned*) = (void*)(base + 657316 - 864);
    int (*get_property_bin)(unsigned, const char*, const char*, unsigned, char**, unsigned*) = (void*)(base + 5828 - 864);
    int (*get_properties_bin)(unsigned, char**, unsigned*) = (void*)(base + 8616 - 864);

    int (*get_properties_bin_buf)(unsigned, char*, unsigned*, char**, unsigned*) = (void*)(base + 673196 - 864);
    int (*get_property_bin_buf)(unsigned, const char*, const char*, unsigned, char*, unsigned*, char**, unsigned*) =
        (void*)(base + 673660 - 864);

    char* buf = NULL;
    unsigned len = 0;

    phase("io_registry_entry_get_property_2805");
    const int kr2805 = get_property(entry, "model", &buf, &len);
    fprintf(stderr, "   kr=%d len=%u payload=[%.*s]\n", kr2805, len, (int)len, buf ? buf : "");

    buf = NULL;
    len = 0;
    phase("io_registry_entry_get_property_bin_2879");
    fprintf(stderr, "   kr=%d len=%u\n", get_property_bin(entry, "", "model", 0, &buf, &len), len);

    buf = NULL;
    len = 0;
    phase("io_registry_entry_get_properties_bin_2878");
    fprintf(stderr, "   kr=%d len=%u\n", get_properties_bin(entry, &buf, &len), len);

    char small[16];
    unsigned small_len = sizeof(small);
    buf = NULL;
    len = 0;
    phase("io_registry_entry_get_properties_bin_buf_2888_ool");
    fprintf(stderr, "   kr=%d inband=%u ool=%u\n", get_properties_bin_buf(entry, small, &small_len, &buf, &len), small_len, len);

    small_len = sizeof(small);
    buf = NULL;
    len = 0;
    phase("io_registry_entry_get_property_bin_buf_2889_ool");
    fprintf(stderr, "   kr=%d inband=%u ool=%u\n",
            get_property_bin_buf(entry, "", "model", 0, small, &small_len, &buf, &len), small_len, len);

    small_len = sizeof(small);
    buf = NULL;
    len = 0;
    phase("io_registry_entry_get_property_bin_buf_2889_plane_bad_entry");
    fprintf(stderr, "   kr=%d\n", get_property_bin_buf(entry, "IOService", "model", 0, small, &small_len, &buf, &len));
}

int main(void)
{
    mach_port_t master = MACH_PORT_NULL;

    phase("IOMainPort");
    IOMainPort(MACH_PORT_NULL, &master);

    phase("io_registry_entry_from_path");
    io_registry_entry_t byPath = IORegistryEntryFromPath(master, "IOService:/");
    fprintf(stderr, "   byPath=0x%x\n", byPath);

    phase("io_registry_get_root_entry");
    io_registry_entry_t root = IORegistryGetRootEntry(master);
    fprintf(stderr, "   root=0x%x\n", root);

    phase("io_object_get_class");
    io_name_t cls;
    kern_return_t kr = IOObjectGetClass(root, cls);
    fprintf(stderr, "   kr=%d class=%s\n", kr, kr == 0 ? cls : "?");

    phase("io_registry_entry_get_name");
    io_name_t nm;
    kr = IORegistryEntryGetName(root, nm);
    fprintf(stderr, "   kr=%d name=%s\n", kr, kr == 0 ? nm : "?");

    phase("io_registry_entry_get_path");
    io_string_t path;
    kr = IORegistryEntryGetPath(root, "IOService", path);
    fprintf(stderr, "   kr=%d path=%s\n", kr, kr == 0 ? path : "?");

    phase("io_service_get_matching_service");
    io_service_t expert = IOServiceGetMatchingService(master, IOServiceMatching("IOPlatformExpertDevice"));
    fprintf(stderr, "   expert=0x%x\n", expert);

    phase("io_object_conforms_to");
    fprintf(stderr, "   conforms=%d\n", IOObjectConformsTo(expert, "IOService"));

    phase("io_object_get_retain_count");
    fprintf(stderr, "   retain=%d\n", IOObjectGetRetainCount(expert));

    phase("io_registry_entry_get_property_CFString");
    CFTypeRef serial = IORegistryEntryCreateCFProperty(expert, CFSTR("IOPlatformSerialNumber"), kCFAllocatorDefault, 0);
    fprintf(stderr, "   serial typeid=%lu\n", serial ? CFGetTypeID(serial) : 0);

    phase("io_registry_entry_get_property_CFData");
    CFTypeRef model = IORegistryEntryCreateCFProperty(expert, CFSTR("model"), kCFAllocatorDefault, 0);
    fprintf(stderr, "   model typeid=%lu\n", model ? CFGetTypeID(model) : 0);

    phase("io_registry_entry_get_property_missing");
    CFTypeRef missing = IORegistryEntryCreateCFProperty(expert, CFSTR("sogen-no-such-property"), kCFAllocatorDefault, 0);
    fprintf(stderr, "   missing=%p\n", (void*)missing);

    phase("io_registry_entry_get_property_bytes");
    char buf[512];
    uint32_t len = sizeof(buf);
    kr = IORegistryEntryGetProperty(expert, "model", buf, &len);
    fprintf(stderr, "   kr=%d len=%u\n", kr, len);

    phase("io_registry_entry_get_properties");
    CFMutableDictionaryRef props = NULL;
    kr = IORegistryEntryCreateCFProperties(expert, &props, kCFAllocatorDefault, 0);
    fprintf(stderr, "   kr=%d count=%ld\n", kr, props ? CFDictionaryGetCount(props) : -1);

    phase("io_service_get_matching_services");
    io_iterator_t it = MACH_PORT_NULL;
    kr = IOServiceGetMatchingServices(master, IOServiceMatching("IOPlatformExpertDevice"), &it);
    fprintf(stderr, "   kr=%d it=0x%x\n", kr, it);

    phase("io_iterator_next");
    io_object_t next = IOIteratorNext(it);
    fprintf(stderr, "   next=0x%x\n", next);

    phase("io_iterator_next_end");
    io_object_t none = IOIteratorNext(it);
    fprintf(stderr, "   none=0x%x\n", none);

    phase("io_registry_entry_from_path_missing");
    io_registry_entry_t bad = IORegistryEntryFromPath(master, "IOService:/sogen-no-such-entry");
    fprintf(stderr, "   bad=0x%x\n", bad);

    probe_private_stubs(expert);

    phase("done");
    return 0;
}
