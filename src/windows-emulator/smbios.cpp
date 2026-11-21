#include "std_include.hpp"
#include "smbios.hpp"

namespace sogen::smbios
{
    namespace
    {
        // DSP0134 3.3.0: each structure is a formatted area starting with type, length and handle, followed by its
        // strings, each NUL terminated, and one more NUL. A structure without strings ends with two NULs.
        class structure_table
        {
          public:
            void add(const uint8_t type, const std::vector<uint8_t>& formatted_fields, const std::vector<std::string_view>& strings)
            {
                const auto length = static_cast<uint8_t>(4 + formatted_fields.size());
                this->data_.insert(this->data_.end(), {type, length});
                this->append_u16(this->next_handle_++);
                this->data_.insert(this->data_.end(), formatted_fields.begin(), formatted_fields.end());

                for (const auto string : strings)
                {
                    this->data_.insert(this->data_.end(), string.begin(), string.end());
                    this->data_.push_back(0);
                }
                if (strings.empty())
                {
                    this->data_.push_back(0);
                }
                this->data_.push_back(0);
            }

            std::vector<uint8_t> take()
            {
                return std::move(this->data_);
            }

          private:
            void append_u16(const uint16_t value)
            {
                this->data_.insert(this->data_.end(), {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)});
            }

            std::vector<uint8_t> data_{};
            uint16_t next_handle_{};
        };

        constexpr uint8_t smbios_major_version = 3;
        constexpr uint8_t smbios_minor_version = 3;

        std::vector<uint8_t> build_structure_table()
        {
            structure_table table{};

            // Type 0, BIOS: vendor, version, starting segment, release date, ROM size, characteristics (not
            // supported), extension bytes, BIOS and firmware revisions, extended ROM size.
            table.add(0,
                      {
                          1,    2,    0x00, 0xE8, 3, 0x00,       //
                          0x08, 0,    0,    0,    0, 0,    0, 0, //
                          0x00, 0x00,                            //
                          1,    0,    0xFF, 0xFF,                //
                          0x00, 0x00,                            //
                      },
                      {"Sogen", "1.0", "01/01/2024"});

            // Type 1, System: manufacturer, product, version, serial, UUID, wake-up type (power switch), SKU, family.
            table.add(1,
                      {
                          1,    2,    3,    4,                                                                            //
                          0x53, 0x4F, 0x47, 0x45, 0x4E, 0x00, 0x4D, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, //
                          0x06, 5,    6,                                                                                  //
                      },
                      {"Sogen", "Sogen Virtual Machine", "1.0", "0", "Sogen", "Virtual Machine"});

            // Type 2, Baseboard: manufacturer, product, version, serial, asset tag, feature flags (hosting board),
            // location, chassis handle, board type (motherboard), no contained objects.
            table.add(2,
                      {
                          1,
                          2,
                          3,
                          4,
                          5, //
                          0x01,
                          6, //
                          0x03,
                          0x00, //
                          0x0A,
                          0, //
                      },
                      {"Sogen", "Sogen Virtual Machine", "1.0", "0", "None", "Default"});

            // Type 3, Chassis: manufacturer, type (desktop), version, serial, asset tag, boot-up, power supply,
            // thermal and security states (safe, safe, safe, none).
            table.add(3,
                      {
                          1,
                          0x03,
                          2,
                          3,
                          4, //
                          0x03,
                          0x03,
                          0x03,
                          0x03,
                      },
                      {"Sogen", "1.0", "0", "None"});

            table.add(127, {}, {});
            return table.take();
        }

        std::vector<uint8_t> build_raw_smbios_data()
        {
            const auto table = build_structure_table();
            std::vector<uint8_t> raw{0, smbios_major_version, smbios_minor_version, 0};
            const auto length = static_cast<uint32_t>(table.size());
            raw.insert(raw.end(), {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length >> 16),
                                   static_cast<uint8_t>(length >> 24)});
            raw.insert(raw.end(), table.begin(), table.end());
            return raw;
        }
    }

    const std::vector<uint8_t>& raw_smbios_data()
    {
        static const auto data = build_raw_smbios_data();
        return data;
    }
}
