// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <sodium.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct ListenerSettings {
    uint32_t magic, generation;
    uint8_t seed[32];
    char ssid[33], password[64];
    uint8_t reserved[3], checksum[16];
};

static_assert(sizeof(ListenerSettings) == 156, "Unexpected settings record layout");

constexpr uint32_t SettingsMagic = 0x31544448; // HDT1
constexpr size_t DataFlashSize = 0x2000;
constexpr size_t SettingsBlock = 0x400;
constexpr size_t SettingsOffset = DataFlashSize - 2 * SettingsBlock;
constexpr int SettingsEmpty = -1, SettingsFailed = -2;

inline bool settingsValid(const ListenerSettings &record) {
    if (record.magic != SettingsMagic || !record.generation || !record.ssid[0] ||
        !memchr(record.ssid, 0, sizeof(record.ssid)) ||
        !memchr(record.password, 0, sizeof(record.password))) return false;
    uint8_t checksum[16];
    crypto_generichash(checksum, sizeof(checksum), reinterpret_cast<const uint8_t *>(&record),
                       offsetof(ListenerSettings, checksum), nullptr, 0);
    return sodium_memcmp(checksum, record.checksum, sizeof(checksum)) == 0;
}

enum class SettingsPage { Blank, Occupied, ReadError };

template<typename Flash>
SettingsPage settingsPage(Flash &flash, size_t offset) {
    uint8_t data[64];
    auto page = SettingsPage::Blank;
    for (size_t i = 0; page == SettingsPage::Blank && i < SettingsBlock; i += sizeof(data)) {
        if (flash.read(data, offset + i, sizeof(data))) {
            page = SettingsPage::ReadError;
        } else {
            for (uint8_t byte : data)
                if (byte != 0xff) { page = SettingsPage::Occupied; break; }
        }
    }
    sodium_memzero(data, sizeof(data));
    return page;
}

template<typename Flash>
int loadSettings(Flash &flash, ListenerSettings &record) {
    sodium_memzero(&record, sizeof(record));
    if (flash.size() != DataFlashSize || flash.get_erase_size() != SettingsBlock)
        return SettingsFailed;
    ListenerSettings candidate;
    int latest = SettingsEmpty;
    for (int slot = 0; slot < 2; ++slot) {
        if (flash.read(&candidate, SettingsOffset + slot * SettingsBlock, sizeof(candidate))) {
            latest = SettingsFailed;
            break;
        }
        if (settingsValid(candidate)) {
            if (latest >= 0 && sodium_memcmp(candidate.seed, record.seed, sizeof(record.seed))) {
                latest = SettingsFailed;
                break;
            }
            if (latest < 0 || candidate.generation > record.generation) {
                record = candidate;
                latest = slot;
            }
        }
    }
    sodium_memzero(&candidate, sizeof(candidate));
    if (latest < 0) sodium_memzero(&record, sizeof(record));
    if (latest == SettingsEmpty &&
        (settingsPage(flash, SettingsOffset) != SettingsPage::Blank ||
         settingsPage(flash, SettingsOffset + SettingsBlock) != SettingsPage::Blank))
        return SettingsFailed;
    return latest;
}

template<typename Flash>
bool saveSettings(Flash &flash, int slot, ListenerSettings &record) {
    if (slot < SettingsEmpty || slot > 1 || record.generation == UINT32_MAX) return false;
    const size_t offset = SettingsOffset + (slot == 0 ? SettingsBlock : 0);
    const auto page = settingsPage(flash, offset);
    if (page == SettingsPage::ReadError) return false;
    if (page == SettingsPage::Occupied) {
        // The first provisioning must never erase data from a previous sketch.
        if (slot == SettingsEmpty || flash.erase(offset, SettingsBlock) ||
            settingsPage(flash, offset) != SettingsPage::Blank) return false;
    }
    record.magic = SettingsMagic;
    ++record.generation;
    crypto_generichash(record.checksum, sizeof(record.checksum), reinterpret_cast<const uint8_t *>(&record),
                       offsetof(ListenerSettings, checksum), nullptr, 0);
    if (flash.program(&record, offset, sizeof(record))) return false;
    ListenerSettings verified;
    const bool ok = !flash.read(&verified, offset, sizeof(verified)) &&
                    !sodium_memcmp(&record, &verified, sizeof(record));
    sodium_memzero(&verified, sizeof(verified));
    return ok;
}
