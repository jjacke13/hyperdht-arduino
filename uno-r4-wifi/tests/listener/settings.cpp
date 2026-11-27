// SPDX-License-Identifier: LGPL-3.0-only
#include "../../examples/uno-r4-listener/HdtListen/listener_settings.h"

#include <cassert>

class Flash {
public:
    uint8_t data[DataFlashSize];
    size_t programmed = 0, erased = 0;
    size_t writeLimit = sizeof(ListenerSettings);
    bool readFails = false;

    Flash() { memset(data, 0xff, sizeof(data)); }
    size_t size() const { return DataFlashSize; }
    size_t get_erase_size() const { return SettingsBlock; }
    int read(void *out, size_t offset, size_t count) {
        assert(count && offset + count <= size());
        if (readFails) return 1;
        memcpy(out, data + offset, count);
        return 0;
    }
    int erase(size_t offset, size_t count) {
        assert(offset >= SettingsOffset && offset % SettingsBlock == 0 && count == SettingsBlock);
        memset(data + offset, 0xff, count);
        ++erased;
        return 0;
    }
    int program(const void *in, size_t offset, size_t count) {
        assert(offset >= SettingsOffset && offset % SettingsBlock == 0 && offset + count <= size());
        const auto *source = static_cast<const uint8_t *>(in);
        ++programmed;
        for (size_t i = 0; i < count && i < writeLimit; ++i) {
            assert(data[offset + i] == 0xff);
            data[offset + i] = source[i];
        }
        return count > writeLimit;
    }
};

int main() {
    assert(sodium_init() >= 0);
    Flash flash;
    ListenerSettings settings, boot;
    assert(loadSettings(flash, settings) == SettingsEmpty);
    randombytes_buf(settings.seed, sizeof(settings.seed));
    strcpy(settings.ssid, "example-wifi");
    strcpy(settings.password, "example-password");
    assert(saveSettings(flash, SettingsEmpty, settings));
    assert(loadSettings(flash, boot) == 0);
    assert(!sodium_memcmp(&settings, &boot, sizeof(settings)));
    assert(flash.programmed == 1 && !flash.erased);

    // An interrupted credentials update must leave the original slot and identity usable.
    strcpy(boot.password, "replacement-example");
    flash.writeLimit = 91;
    assert(!saveSettings(flash, 0, boot));
    assert(loadSettings(flash, boot) == 0);
    assert(!sodium_memcmp(&settings, &boot, sizeof(settings)));

    strcpy(boot.password, "replacement-example");
    flash.writeLimit = sizeof(ListenerSettings);
    assert(saveSettings(flash, 0, boot));
    assert(loadSettings(flash, boot) == 1);
    assert(!strcmp(boot.password, "replacement-example"));
    assert(!sodium_memcmp(boot.seed, settings.seed, sizeof(settings.seed)));
    assert(boot.generation == 2);

    flash.readFails = true;
    const auto programmed = flash.programmed, erased = flash.erased;
    assert(!saveSettings(flash, 1, boot));
    assert(flash.programmed == programmed && flash.erased == erased);
    assert(loadSettings(flash, boot) == SettingsFailed);

    Flash occupied;
    occupied.data[SettingsOffset + SettingsBlock - 1] = 0;
    assert(loadSettings(occupied, boot) == SettingsFailed);
    assert(!occupied.programmed && !occupied.erased);
    sodium_memzero(&settings, sizeof(settings));
    sodium_memzero(&boot, sizeof(boot));
}
