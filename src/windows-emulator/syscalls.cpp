#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "cpu_context.hpp"
#include "emulator_utils.hpp"
#include "syscall_utils.hpp"
#include "devices/console.hpp"

#include <numeric>
#include <cwctype>
#include <algorithm>
#include <utils/time.hpp>
#include <utils/finally.hpp>

namespace sogen
{

    namespace syscalls
    {
        // syscalls/event.cpp:
        NTSTATUS handle_NtSetEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
        NTSTATUS handle_NtPulseEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
        NTSTATUS handle_NtTraceEvent();
        NTSTATUS handle_NtQueryEvent(const syscall_context& c, handle event_handle, uint32_t event_information_class,
                                     emulator_object<EVENT_BASIC_INFORMATION> event_information, uint32_t event_information_length,
                                     emulator_object<uint32_t> return_length);
        NTSTATUS handle_NtClearEvent(const syscall_context& c, handle event_handle);
        NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                      BOOLEAN initial_state);
        NTSTATUS handle_NtOpenEvent(const syscall_context& c, emulator_object<uint64_t> event_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);

        // syscalls/exception.cpp
        NTSTATUS handle_NtRaiseHardError(const syscall_context& c, NTSTATUS error_status, ULONG number_of_parameters,
                                         emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> unicode_string_parameter_mask,
                                         uint64_t parameters, HARDERROR_RESPONSE_OPTION valid_response_option,
                                         emulator_object<HARDERROR_RESPONSE> response);
        NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                         emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> exception_record,
                                         emulator_object<CONTEXT64> thread_context, BOOLEAN handle_exception);

        // syscalls/file.cpp
        NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                             emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                             uint64_t file_information, ULONG length, FILE_INFORMATION_CLASS info_class);
        NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, handle file_handle,
                                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                                     uint64_t fs_information, ULONG length, FS_INFORMATION_CLASS fs_information_class);
        NTSTATUS handle_NtQueryDirectoryFileEx(const syscall_context& c, handle file_handle, handle event_handle,
                                               EMULATOR_CAST(emulator_pointer, PIO_APC_ROUTINE) apc_routine, emulator_pointer apc_context,
                                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                               uint64_t file_information, uint32_t length, uint32_t info_class, ULONG query_flags,
                                               emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> file_name);
        NTSTATUS handle_NtQueryDirectoryFile(const syscall_context& c, handle file_handle, handle event_handle,
                                             EMULATOR_CAST(emulator_pointer, PIO_APC_ROUTINE) apc_routine, emulator_pointer apc_context,
                                             emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                             uint64_t file_information, uint32_t length, uint32_t info_class, BOOLEAN return_single_entry,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> file_name, BOOLEAN restart_scan);
        NTSTATUS handle_NtQueryInformationFile(const syscall_context& c, handle file_handle,
                                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                               uint64_t file_information, uint32_t length, uint32_t info_class);
        NTSTATUS handle_NtQueryInformationByName(const syscall_context& c,
                                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                                 uint64_t file_information, uint32_t length, uint32_t info_class);
        NTSTATUS handle_NtReadFile(const syscall_context& c, handle file_handle, uint64_t /*event*/, uint64_t /*apc_routine*/,
                                   uint64_t /*apc_context*/, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                   uint64_t buffer, ULONG length, emulator_object<LARGE_INTEGER> /*byte_offset*/,
                                   emulator_object<ULONG> /*key*/);
        NTSTATUS handle_NtWriteFile(const syscall_context& c, handle file_handle, uint64_t /*event*/, uint64_t /*apc_routine*/,
                                    uint64_t /*apc_context*/, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                    uint64_t buffer, ULONG length, emulator_object<LARGE_INTEGER> /*byte_offset*/,
                                    emulator_object<ULONG> /*key*/);
        NTSTATUS handle_NtCopyFileChunk(const syscall_context& c, handle source_handle, handle destination_handle, handle event_handle,
                                        emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG length,
                                        emulator_object<LARGE_INTEGER> source_offset, emulator_object<LARGE_INTEGER> destination_offset,
                                        emulator_object<ULONG> source_key, emulator_object<ULONG> destination_key, ULONG flags);
        NTSTATUS handle_NtLockFile(const syscall_context& c, handle file_handle, handle event_handle, uint64_t apc_routine,
                                   uint64_t apc_context, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                   emulator_object<LARGE_INTEGER> byte_offset, emulator_object<LARGE_INTEGER> length, ULONG key,
                                   BOOLEAN fail_immediately, BOOLEAN exclusive_lock);
        NTSTATUS handle_NtUnlockFile(const syscall_context& c, handle file_handle,
                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                     emulator_object<LARGE_INTEGER> byte_offset, emulator_object<LARGE_INTEGER> length, ULONG key);
        NTSTATUS handle_NtCreateFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                     emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/,
                                     emulator_object<LARGE_INTEGER> /*allocation_size*/, ULONG /*file_attributes*/, ULONG /*share_access*/,
                                     ULONG create_disposition, ULONG create_options, uint64_t ea_buffer, ULONG ea_length);
        NTSTATUS handle_NtQueryAttributesFile(const syscall_context& c,
                                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                              emulator_object<FILE_BASIC_INFORMATION> file_information);
        NTSTATUS handle_NtQueryFullAttributesFile(const syscall_context& c,
                                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                                  emulator_object<FILE_NETWORK_OPEN_INFORMATION> file_information);
        NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                   emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                   emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                                   ULONG open_options);
        NTSTATUS handle_NtOpenDirectoryObject(const syscall_context& c, emulator_object<handle> directory_handle,
                                              ACCESS_MASK /*desired_access*/,
                                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtCreateDirectoryObject(const syscall_context& /*c*/, emulator_object<handle> /*directory_handle*/,
                                                ACCESS_MASK /*desired_access*/,
                                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtOpenSymbolicLinkObject(const syscall_context& c, emulator_object<handle> link_handle,
                                                 ACCESS_MASK /*desired_access*/,
                                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtQuerySymbolicLinkObject(const syscall_context& c, handle link_handle,
                                                  emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> link_target,
                                                  emulator_object<ULONG> returned_length);
        NTSTATUS handle_NtCreateNamedPipeFile(const syscall_context& c, emulator_object<handle> file_handle, ULONG desired_access,
                                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                              emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                                              ULONG create_disposition, ULONG create_options, ULONG named_pipe_type, ULONG read_mode,
                                              ULONG completion_mode, ULONG maximum_instances, ULONG inbound_quota, ULONG outbound_quota,
                                              emulator_object<LARGE_INTEGER> default_timeout);
        NTSTATUS handle_NtFsControlFile(const syscall_context& c, handle file_handle, handle event, emulator_pointer apc_routine,
                                        emulator_pointer apc_context,
                                        emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG fs_control_code,
                                        emulator_pointer input_buffer, ULONG input_buffer_length, emulator_pointer output_buffer,
                                        ULONG output_buffer_length);
        NTSTATUS handle_NtFlushBuffersFile(const syscall_context& c, handle file_handle,
                                           emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/);

        // syscalls/locale.cpp:
        NTSTATUS handle_NtInitializeNlsFiles(const syscall_context& c, emulator_object<uint64_t> base_address,
                                             emulator_object<LCID> default_locale_id,
                                             emulator_object<LARGE_INTEGER> /*default_casing_table_size*/);
        NTSTATUS handle_NtQueryDefaultLocale(const syscall_context&, BOOLEAN /*user_profile*/, emulator_object<LCID> default_locale_id);
        NTSTATUS handle_NtGetNlsSectionPtr(const syscall_context& c, ULONG section_type, ULONG section_data,
                                           emulator_pointer /*context_data*/, emulator_object<uint64_t> section_pointer,
                                           emulator_object<ULONG> section_size);
        NTSTATUS handle_NtGetMUIRegistryInfo();
        NTSTATUS handle_NtIsUILanguageComitted();
        uint64_t handle_NtUserActivateKeyboardLayout(const syscall_context& c, uint64_t keyboard_layout, uint32_t flags);
        uint64_t handle_NtUserGetKeyboardLayout(const syscall_context& c, uint32_t thread_id);
        uint32_t handle_NtUserGetKeyboardLayoutList(const syscall_context& c, uint32_t buffer_count, emulator_pointer keyboard_layouts);
        BOOL handle_NtUserGetKeyboardLayoutName(const syscall_context& c, emulator_pointer name);
        NTSTATUS handle_NtQueryDefaultUILanguage(const syscall_context&, emulator_object<LANGID> language_id);
        NTSTATUS handle_NtQueryInstallUILanguage(const syscall_context&, emulator_object<LANGID> language_id);

        // syscalls/memory.cpp:
        NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, handle process_handle, uint64_t base_address, uint32_t info_class,
                                             uint64_t memory_information, uint64_t memory_information_length,
                                             emulator_object<uint64_t> return_length);
        NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                               emulator_object<uint32_t> bytes_to_protect, uint32_t protection,
                                               emulator_object<uint32_t> old_protection);
        NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                                  emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                                  uint32_t page_protection, emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters,
                                                  ULONG extended_parameter_count);
        NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                                uint64_t zero_bits, emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                                uint32_t page_protection);
        NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                            emulator_object<uint64_t> bytes_to_allocate, uint32_t free_type);
        NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, handle process_handle, emulator_pointer base_address,
                                            emulator_pointer buffer, ULONG number_of_bytes_to_read,
                                            emulator_object<ULONG> number_of_bytes_read);
        NTSTATUS handle_NtWriteVirtualMemory(const syscall_context& c, handle process_handle, emulator_pointer base_address,
                                             emulator_pointer buffer, ULONG number_of_bytes_to_write,
                                             emulator_object<ULONG> number_of_bytes_write);
        NTSTATUS handle_NtSetInformationVirtualMemory();
        BOOL handle_NtLockVirtualMemory();
        NTSTATUS handle_NtUnlockVirtualMemory();
        NTSTATUS handle_NtFlushVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                             emulator_object<uint64_t> region_size,
                                             emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);

        // syscalls/mutant.cpp:
        NTSTATUS handle_NtReleaseMutant(const syscall_context& c, handle mutant_handle, emulator_object<LONG> previous_count);
        NTSTATUS handle_NtOpenMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                     emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtCreateMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                       emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, BOOLEAN initial_owner);

        // syscalls/namespace.cpp:
        NTSTATUS handle_NtCreatePrivateNamespace(const syscall_context& c, emulator_object<handle> namespace_handle,
                                                 ACCESS_MASK desired_access,
                                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                                 emulator_object<OBJECT_BOUNDARY_DESCRIPTOR> boundary_descriptor);

        NTSTATUS handle_NtOpenPrivateNamespace(const syscall_context& c, emulator_object<handle> namespace_handle,
                                               ACCESS_MASK desired_access,
                                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                               emulator_object<OBJECT_BOUNDARY_DESCRIPTOR> boundary_descriptor);
        NTSTATUS handle_NtDeletePrivateNamespace(const syscall_context& c, handle namespace_handle);

        // syscalls/object.cpp:
        NTSTATUS handle_NtClose(const syscall_context& c, handle h);
        NTSTATUS handle_NtDuplicateObject(const syscall_context& c, handle source_process_handle, handle source_handle,
                                          handle target_process_handle, emulator_object<handle> target_handle, ACCESS_MASK desired_access,
                                          ULONG handle_attributes, ULONG options);
        NTSTATUS handle_NtQueryObject(const syscall_context& c, handle handle, OBJECT_INFORMATION_CLASS object_information_class,
                                      emulator_pointer object_information, ULONG object_information_length,
                                      emulator_object<ULONG> return_length);
        NTSTATUS handle_NtCompareObjects(const syscall_context& c, handle first, handle second);
        DWORD handle_NtUserMsgWaitForMultipleObjectsEx(const syscall_context& c, ULONG count, emulator_object<handle> handles,
                                                       DWORD timeout, DWORD wake_mask, DWORD flags);
        NTSTATUS handle_NtWaitForMultipleObjects(const syscall_context& c, ULONG count, emulator_object<handle> handles,
                                                 WAIT_TYPE wait_type, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
        NTSTATUS handle_NtWaitForMultipleObjects32(const syscall_context& c, ULONG count, emulator_object<uint32_t> handles,
                                                   WAIT_TYPE wait_type, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
        NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, handle h, BOOLEAN alertable,
                                              emulator_object<LARGE_INTEGER> timeout);
        NTSTATUS handle_NtSetInformationObject();
        NTSTATUS handle_NtQuerySecurityObject(const syscall_context& c, handle /*h*/, SECURITY_INFORMATION /*security_information*/,
                                              emulator_pointer security_descriptor, ULONG length, emulator_object<ULONG> length_needed);
        NTSTATUS handle_NtSetSecurityObject();

        // syscalls/port.cpp:
        NTSTATUS handle_NtConnectPort(const syscall_context& c, emulator_object<handle> client_port_handle,
                                      emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                      emulator_object<SECURITY_QUALITY_OF_SERVICE> /*security_qos*/,
                                      emulator_object<PORT_VIEW64> client_shared_memory,
                                      emulator_object<REMOTE_PORT_VIEW64> /*server_shared_memory*/,
                                      emulator_object<ULONG> /*maximum_message_length*/, emulator_pointer connection_info,
                                      emulator_object<ULONG> connection_info_length);
        NTSTATUS handle_NtSecureConnectPort(const syscall_context& c, emulator_object<handle> client_port_handle,
                                            emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                            emulator_object<SECURITY_QUALITY_OF_SERVICE> security_qos,
                                            emulator_object<PORT_VIEW64> client_shared_memory, emulator_pointer /*server_sid*/,
                                            emulator_object<REMOTE_PORT_VIEW64> server_shared_memory,
                                            emulator_object<ULONG> maximum_message_length, emulator_pointer connection_info,
                                            emulator_object<ULONG> connection_info_length);
        NTSTATUS handle_NtAlpcCreatePort(const syscall_context& c, emulator_object<handle> port_handle,
                                         emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                         emulator_pointer port_attributes);
        NTSTATUS handle_NtAlpcConnectPort(const syscall_context& c, emulator_object<handle> port_handle,
                                          emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                          emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                          emulator_pointer /*port_attributes*/, ULONG /*flags*/, emulator_pointer /*required_server_sid*/,
                                          emulator_pointer /*connection_message*/,
                                          emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                          emulator_pointer /*out_message_attributes*/, emulator_pointer /*in_message_attributes*/,
                                          emulator_object<LARGE_INTEGER> /*timeout*/);
        NTSTATUS handle_NtAlpcConnectPortEx(const syscall_context& c, emulator_object<handle> port_handle,
                                            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> connection_port_object_attributes,
                                            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*client_port_object_attributes*/,
                                            emulator_pointer port_attributes, ULONG flags,
                                            emulator_pointer /*server_security_requirements*/, emulator_pointer connection_message,
                                            emulator_object<EmulatorTraits<Emu64>::SIZE_T> buffer_length,
                                            emulator_pointer out_message_attributes, emulator_pointer in_message_attributes,
                                            emulator_object<LARGE_INTEGER> timeout);
        NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, handle port_handle, ULONG /*flags*/,
                                                  emulator_object<PORT_MESSAGE64> send_message,
                                                  emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                                  /*send_message_attributes*/,
                                                  emulator_object<PORT_MESSAGE64> receive_message,
                                                  emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                                  emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                                  /*receive_message_attributes*/,
                                                  emulator_object<LARGE_INTEGER> /*timeout*/);
        NTSTATUS handle_NtAlpcQueryInformation();
        NTSTATUS handle_NtAlpcSetInformation();
        NTSTATUS handle_NtAlpcCreateSecurityContext();
        NTSTATUS handle_NtAlpcDeleteSecurityContext();

        // syscalls/process.cpp:
        NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class,
                                                  uint64_t process_information, uint32_t process_information_length,
                                                  emulator_object<uint32_t> return_length);
        NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class,
                                                uint64_t process_information, uint32_t process_information_length);
        NTSTATUS handle_NtOpenProcess();
        NTSTATUS handle_NtOpenProcessToken(const syscall_context&, handle process_handle, ACCESS_MASK /*desired_access*/,
                                           emulator_object<handle> token_handle);
        NTSTATUS handle_NtOpenProcessTokenEx(const syscall_context& c, handle process_handle, ACCESS_MASK desired_access,
                                             ULONG /*handle_attributes*/, emulator_object<handle> token_handle);
        NTSTATUS handle_NtTerminateProcess(const syscall_context& c, handle process_handle, NTSTATUS exit_status);
        NTSTATUS handle_NtFlushProcessWriteBuffers(const syscall_context& c);

        // syscalls/registry.cpp:
        NTSTATUS handle_NtOpenKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK /*desired_access*/,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtOpenKeyEx(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG /*open_options*/);
        NTSTATUS handle_NtQueryKey(const syscall_context& c, handle key_handle, KEY_INFORMATION_CLASS key_information_class,
                                   uint64_t key_information, ULONG length, emulator_object<ULONG> result_length);
        NTSTATUS handle_NtQueryValueKey(const syscall_context& c, handle key_handle,
                                        emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                        KEY_VALUE_INFORMATION_CLASS key_value_information_class, uint64_t key_value_information,
                                        ULONG length, emulator_object<ULONG> result_length);
        NTSTATUS handle_NtQueryMultipleValueKey(const syscall_context& c, handle key_handle, emulator_object<KEY_VALUE_ENTRY> value_entries,
                                                ULONG entry_count, uint64_t value_buffer, emulator_object<ULONG> buffer_length,
                                                emulator_object<ULONG> required_buffer_length);
        NTSTATUS handle_NtCreateKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG /*title_index*/,
                                    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*class*/, ULONG /*create_options*/,
                                    emulator_object<ULONG> /*disposition*/);
        NTSTATUS handle_NtSetValueKey(const syscall_context& c, handle key_handle,
                                      emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name, ULONG /*title_index*/, ULONG type,
                                      uint64_t data, ULONG data_size);
        NTSTATUS handle_NtDeleteValueKey(const syscall_context& c, handle key_handle,
                                         emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name);
        NTSTATUS handle_NtNotifyChangeKey();
        NTSTATUS handle_NtSetInformationKey(const syscall_context& c, handle key_handle, KEY_SET_INFORMATION_CLASS key_information_class,
                                            uint64_t key_information, ULONG length);
        NTSTATUS handle_NtEnumerateKey(const syscall_context& c, handle key_handle, ULONG index,
                                       KEY_INFORMATION_CLASS key_information_class, uint64_t key_information, ULONG length,
                                       emulator_object<ULONG> result_length);
        NTSTATUS handle_NtEnumerateValueKey(const syscall_context& c, handle key_handle, ULONG index,
                                            KEY_VALUE_INFORMATION_CLASS key_value_information_class, uint64_t key_value_information,
                                            ULONG length, emulator_object<ULONG> result_length);

        // syscalls/section.cpp:
        NTSTATUS handle_NtCreateSection(const syscall_context& c, emulator_object<handle> section_handle, ACCESS_MASK /*desired_access*/,
                                        emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                        emulator_object<ULARGE_INTEGER> maximum_size, ULONG section_page_protection,
                                        ULONG allocation_attributes, handle file_handle);
        NTSTATUS handle_NtOpenSection(const syscall_context& c, emulator_object<handle> section_handle, ACCESS_MASK /*desired_access*/,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtQuerySection(const syscall_context& c, handle section_handle, SECTION_INFORMATION_CLASS section_information_class,
                                       uint64_t section_information, EmulatorTraits<Emu64>::SIZE_T section_information_length,
                                       emulator_object<EmulatorTraits<Emu64>::SIZE_T> result_length);
        NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, handle section_handle, handle process_handle,
                                           emulator_object<uint64_t> base_address,
                                           EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) /*zero_bits*/,
                                           EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) /*commit_size*/,
                                           emulator_object<LARGE_INTEGER> /*section_offset*/,
                                           emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                           SECTION_INHERIT /*inherit_disposition*/, ULONG /*allocation_type*/, ULONG /*win32_protect*/);
        NTSTATUS handle_NtMapViewOfSectionEx(const syscall_context& c, handle section_handle, handle process_handle,
                                             emulator_object<uint64_t> base_address, emulator_object<LARGE_INTEGER> section_offset,
                                             emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                             ULONG allocation_type, ULONG page_protection,
                                             uint64_t extended_parameters, // PMEM_EXTENDED_PARAMETER
                                             ULONG extended_parameter_count);
        NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, handle process_handle, uint64_t base_address);
        NTSTATUS handle_NtUnmapViewOfSectionEx(const syscall_context& c, handle process_handle, uint64_t base_address, ULONG /*flags*/);
        NTSTATUS handle_NtAreMappedFilesTheSame();

        // syscalls/semaphore.cpp:
        NTSTATUS handle_NtOpenSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK /*desired_access*/,
                                        emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtReleaseSemaphore(const syscall_context& c, handle semaphore_handle, ULONG release_count,
                                           emulator_object<LONG> previous_count);
        NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle,
                                          ACCESS_MASK /*desired_access*/,
                                          emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG initial_count,
                                          ULONG maximum_count);

        // syscalls/system.cpp:
        NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                                 uint32_t system_information_length, emulator_object<uint32_t> return_length);
        NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, uint32_t info_class, uint64_t input_buffer,
                                                   uint32_t input_buffer_length, uint64_t system_information,
                                                   uint32_t system_information_length, emulator_object<uint32_t> return_length);
        NTSTATUS handle_NtSetSystemInformation();
        NTSTATUS handle_NtPowerInformation(const syscall_context& c, uint32_t information_level, uint64_t input_buffer,
                                           uint32_t input_buffer_length, uint64_t output_buffer, uint32_t output_buffer_length);

        // syscalls/thread.cpp:
        NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                               uint64_t thread_information, uint32_t thread_information_length);

        NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                                 uint64_t thread_information, uint32_t thread_information_length,
                                                 emulator_object<uint32_t> return_length);
        NTSTATUS handle_NtOpenThread(const syscall_context&, emulator_object<handle> thread_handle, ACCESS_MASK desired_access,
                                     emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                     emulator_object<CLIENT_ID64> client_id);
        NTSTATUS handle_NtOpenThreadToken(const syscall_context&, handle thread_handle, ACCESS_MASK /*desired_access*/,
                                          BOOLEAN /*open_as_self*/, emulator_object<handle> token_handle);
        NTSTATUS handle_NtOpenThreadTokenEx(const syscall_context& c, handle thread_handle, ACCESS_MASK desired_access,
                                            BOOLEAN open_as_self, ULONG /*handle_attributes*/, emulator_object<handle> token_handle);
        NTSTATUS handle_NtTerminateThread(const syscall_context& c, handle thread_handle, NTSTATUS exit_status);
        NTSTATUS handle_NtDelayExecution(const syscall_context& c, BOOLEAN alertable, emulator_object<LARGE_INTEGER> delay_interval);
        NTSTATUS handle_NtAlertThreadByThreadId(const syscall_context& c, uint64_t thread_id);
        NTSTATUS handle_NtAlertThreadByThreadIdEx(const syscall_context& c, uint64_t thread_id,
                                                  emulator_object<EMU_RTL_SRWLOCK<EmulatorTraits<Emu64>>> lock);
        NTSTATUS handle_NtWaitForAlertByThreadId(const syscall_context& c, uint64_t, emulator_object<LARGE_INTEGER> timeout);
        NTSTATUS handle_NtYieldExecution(const syscall_context& c);
        NTSTATUS handle_NtSetThreadExecutionState(const syscall_context& c, ULONG new_flags, emulator_object<ULONG> previous_flags);
        NTSTATUS handle_NtSuspendThread(const syscall_context& c, handle thread_handle, emulator_object<ULONG> previous_suspend_count);
        NTSTATUS handle_NtResumeThread(const syscall_context& c, handle thread_handle, emulator_object<ULONG> previous_suspend_count);
        NTSTATUS handle_NtContinue(const syscall_context& c, emulator_object<CONTEXT64> thread_context, BOOLEAN raise_alert);
        NTSTATUS handle_NtContinueEx(const syscall_context& c, emulator_object<CONTEXT64> thread_context, uint64_t continue_argument);
        NTSTATUS handle_NtGetNextThread(const syscall_context& c, handle process_handle, handle thread_handle,
                                        ACCESS_MASK /*desired_access*/, ULONG /*handle_attributes*/, ULONG flags,
                                        emulator_object<handle> new_thread_handle);
        NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
        NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
        NTSTATUS handle_NtCreateThreadEx(const syscall_context& c, emulator_object<handle> thread_handle, ACCESS_MASK /*desired_access*/,
                                         emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                         /*object_attributes*/,
                                         handle process_handle, uint64_t start_routine, uint64_t argument, ULONG create_flags,
                                         EmulatorTraits<Emu64>::SIZE_T /*zero_bits*/, EmulatorTraits<Emu64>::SIZE_T stack_size,
                                         EmulatorTraits<Emu64>::SIZE_T maximum_stack_size,
                                         emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list);
        NTSTATUS handle_NtGetCurrentProcessorNumberEx(const syscall_context&, emulator_object<PROCESSOR_NUMBER> processor_number);
        ULONG handle_NtGetCurrentProcessorNumber(const syscall_context& c);
        NTSTATUS handle_NtQueueApcThreadEx2(const syscall_context& c, handle thread_handle, handle reserve_handle, uint32_t apc_flags,
                                            uint64_t apc_routine, uint64_t apc_argument1, uint64_t apc_argument2, uint64_t apc_argument3);
        NTSTATUS handle_NtQueueApcThreadEx(const syscall_context& c, handle thread_handle, handle reserve_handle, uint64_t apc_routine,
                                           uint64_t apc_argument1, uint64_t apc_argument2, uint64_t apc_argument3);
        NTSTATUS handle_NtQueueApcThread(const syscall_context& c, handle thread_handle, uint64_t apc_routine, uint64_t apc_argument1,
                                         uint64_t apc_argument2, uint64_t apc_argument3);
        NTSTATUS handle_NtCallbackReturn(const syscall_context& c, emulator_pointer callback_result, ULONG callback_result_length,
                                         NTSTATUS callback_status);

        // syscalls/timer.cpp:
        NTSTATUS handle_NtQueryTimerResolution(const syscall_context&, emulator_object<ULONG> maximum_time,
                                               emulator_object<ULONG> minimum_time, emulator_object<ULONG> current_time);
        NTSTATUS handle_NtSetTimerResolution(const syscall_context&, ULONG /*desired_resolution*/, BOOLEAN set_resolution,
                                             emulator_object<ULONG> current_resolution);
        NTSTATUS handle_NtCreateTimer2(const syscall_context& c, emulator_object<handle> timer_handle, uint64_t reserved,
                                       emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG attributes,
                                       ACCESS_MASK desired_access);
        NTSTATUS handle_NtCreateTimer(const syscall_context& c, emulator_object<handle> timer_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG timer_type);
        NTSTATUS handle_NtOpenTimer(const syscall_context& c, emulator_object<handle> timer_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtSetTimer();
        NTSTATUS handle_NtSetTimer2();
        NTSTATUS handle_NtSetTimerEx(const syscall_context& c, handle timer_handle, uint32_t timer_set_info_class,
                                     uint64_t timer_set_information, ULONG timer_set_information_length);
        NTSTATUS handle_NtCancelTimer();

        // syscalls/token.cpp:
        NTSTATUS
        handle_NtDuplicateToken(const syscall_context&, handle existing_token_handle, ACCESS_MASK /*desired_access*/,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                /*object_attributes*/,
                                BOOLEAN /*effective_only*/, TOKEN_TYPE type, emulator_object<handle> new_token_handle);
        NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle,
                                                TOKEN_INFORMATION_CLASS token_information_class, uint64_t token_information,
                                                ULONG token_information_length, emulator_object<ULONG> return_length);
        NTSTATUS handle_NtQuerySecurityAttributesToken(const syscall_context& c, handle token_handle,
                                                       emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> attributes,
                                                       ULONG number_of_attributes, uint64_t buffer, ULONG buffer_length,
                                                       emulator_object<ULONG> return_length);
        NTSTATUS handle_NtAccessCheck(const syscall_context& c, uint64_t security_descriptor, handle client_token,
                                      ACCESS_MASK desired_access, emulator_object<EMU_GENERIC_MAPPING> generic_mapping,
                                      uint64_t privilege_set, emulator_object<ULONG> privilege_set_length,
                                      emulator_object<ACCESS_MASK> granted_access, emulator_object<NTSTATUS> access_status);
        NTSTATUS handle_NtAdjustPrivilegesToken();
        NTSTATUS handle_NtQuerySecurityPolicy();
        NTSTATUS handle_NtFlushInstructionCache(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                                uint64_t region_size);

        // syscalls/license.cpp
        NTSTATUS handle_NtQueryLicenseValue(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                            emulator_object<uint32_t> type, uint64_t data, uint64_t data_size,
                                            emulator_object<uint32_t> result_data_size);

        // syscalls/user.cpp:
        NTSTATUS handle_NtUserTraceLoggingSendMixedModeTelemetry();
        uint32_t handle_NtUserRegisterWindowMessage(const syscall_context& c,
                                                    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> message_name);
        uint64_t handle_NtUserGetThreadState(const syscall_context& c, ULONG routine);
        uint64_t handle_NtUserSetThreadState(const syscall_context& c, uint64_t value, uint64_t mask);
        uint64_t completion_NtUserGetThreadState(const syscall_context& c, ULONG routine);
        NTSTATUS handle_NtUserProcessConnect(const syscall_context& c, handle process_handle, ULONG length, emulator_pointer user_connect);
        NTSTATUS handle_NtUserInitializeClientPfnArrays(const syscall_context& c, emulator_pointer apfn_client_a,
                                                        emulator_pointer apfn_client_w, emulator_pointer apfn_client_worker,
                                                        emulator_pointer hmod_user);
        uint64_t handle_NtUserRemoteConnectState(const syscall_context& c);
        hdesk handle_NtUserGetThreadDesktop(const syscall_context& c, ULONG thread_id);
        hdc handle_NtUserGetDCEx(const syscall_context& c, hwnd window, uint64_t clip_region, ULONG flags);
        hdc handle_NtUserGetDC(const syscall_context& c, hwnd window);
        hdc handle_NtUserGetWindowDC(const syscall_context& c, hwnd window);
        hwnd handle_NtUserWindowFromDC(const syscall_context& c, hdc dc);
        uint64_t handle_NtUserGetControlBrush(const syscall_context& c, hwnd window, hdc dc, uint32_t control_type);
        BOOL handle_NtUserReleaseDC();
        hwnd handle_NtUserSetCapture(const syscall_context& c, hwnd window);
        BOOL handle_NtUserReleaseCapture(const syscall_context& c);
        BOOL handle_NtUserRegisterRawInputDevices(const syscall_context& c, emulator_pointer devices, uint32_t device_count, uint32_t size);
        ULONG handle_NtUserGetRawInputDeviceList(const syscall_context& c, emulator_pointer devices, emulator_pointer device_count,
                                                 uint32_t size);
        ULONG handle_NtUserGetRawInputDeviceInfo(const syscall_context& c, handle device, uint32_t command, emulator_pointer data,
                                                 emulator_pointer size);
        uint32_t handle_NtUserGetRawInputData(const syscall_context& c, emulator_pointer raw_input, uint32_t command, emulator_pointer data,
                                              emulator_object<uint32_t> size_ptr, uint32_t header_size);
        BOOL handle_NtUserDefSetText(const syscall_context& c, hwnd window, emulator_object<LARGE_STRING> text);
        BOOL handle_NtUserGetOemBitmapSize(const syscall_context& c, uint32_t bitmap_id, emulator_pointer size_ptr);
        BOOL handle_NtUserSetWindowState(const syscall_context& c, hwnd window, uint32_t flags);
        BOOL handle_NtUserClearWindowState(const syscall_context& c, hwnd window, uint32_t flags);
        BOOL handle_NtUserDisableProcessWindowsGhosting(const syscall_context& c);
        BOOL handle_NtUserBitBltSysBmp(const syscall_context& c, hdc dc, int x, int y, uint32_t bitmap_index);
        BOOL handle_NtUserGetClientRect(const syscall_context& c, hwnd window, emulator_pointer rect_ptr);
        hdc handle_NtUserBeginPaint(const syscall_context& c, hwnd window, emulator_object<EMU_PAINTSTRUCT> paint_struct);
        BOOL handle_NtUserEndPaint(const syscall_context& c, hwnd window, emulator_object<EMU_PAINTSTRUCT> paint_struct);
        BOOL handle_NtUserGetCursorPos(const syscall_context& c, emulator_pointer point_ptr);
        BOOL handle_NtUserGetCursorInfo(const syscall_context& c, emulator_object<EMU_CURSORINFO> cursor_info);
        BOOL handle_NtUserGetClipCursor(const syscall_context& c, emulator_pointer rect_ptr);
        BOOL handle_NtUserTransformPoint(const syscall_context& c, emulator_pointer point, uint32_t from_dpi, uint32_t to_dpi,
                                         uint32_t flags);
        int32_t handle_NtUserShowCursor(const syscall_context& c, BOOL show);
        uint32_t handle_NtUserGetKeyState(const syscall_context& c, int32_t virtual_key);
        uint32_t handle_NtUserGetAsyncKeyState(const syscall_context& c, int32_t virtual_key);
        BOOL handle_NtUserClipCursor(const syscall_context& c, emulator_pointer rect);
        BOOL handle_NtUserSetCursorPos(const syscall_context& c, int32_t x, int32_t y);
        hcursor handle_NtUserSetCursor(const syscall_context& c, hcursor cursor);
        hcursor handle_NtUserGetCursor(const syscall_context& c);
        hicon handle_NtUserCreateEmptyCursorObject();
        BOOL handle_NtUserSetCursorIconData();
        BOOL handle_NtUserSetCursorIconDataEx();
        BOOL handle_NtUserGetRequiredCursorSizes();
        NTSTATUS handle_NtUserFindExistingCursorIcon();
        BOOL handle_NtUserDestroyCursor(const syscall_context& c, hicon icon, DWORD flags);
        hicon handle_NtUserGetCursorFrameInfo(const syscall_context& c, hicon icon, UINT frame, emulator_object<uint32_t> rate_jiffies,
                                              emulator_object<uint32_t> frame_count);
        BOOL handle_NtUserGetIconSize(const syscall_context& c, hicon icon, UINT frame, emulator_object<int> cx, emulator_object<int> cy);
        BOOL handle_NtUserGetIconInfo(const syscall_context& c, hicon icon, emulator_object<EMU_ICONINFO> icon_info,
                                      emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> inst_name,
                                      emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> res_name, emulator_object<uint32_t> bpp,
                                      BOOL internal);
        BOOL handle_NtUserDrawIconEx(const syscall_context& c, hdc dc, int x, int y, hicon icon, int cx, int cy, UINT istep,
                                     uint64_t flicker_brush, UINT di_flags);
        BOOL handle_NtUserMessageBeep();
        uint64_t handle_NtUserFindWindowEx(const syscall_context& c, hwnd parent, hwnd child_after,
                                           emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                           emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> window_name);
        BOOL handle_NtUserMoveWindow(const syscall_context& c, hwnd hwnd, int x, int y, int width, int height, BOOL repaint);
        uint64_t handle_NtUserGetProcessWindowStation();
        uint64_t handle_NtUserCallHwndParam(const syscall_context& c, hwnd hwnd, uint64_t param, uint32_t code);
        uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                                 emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                                 emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                                 emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name, DWORD function_id,
                                                 DWORD flags, emulator_pointer wow);
        BOOL handle_NtUserUnregisterClass(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                          emulator_pointer instance, emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name);
        BOOL handle_NtUserGetClassInfoEx(const syscall_context& c, hinstance /*instance*/,
                                         emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                         emulator_object<EMU_WNDCLASSEX> wnd_class_ex, emulator_pointer menu_name, BOOL /*ansi*/);
        int handle_NtUserGetClassName(const syscall_context& c, hwnd win_hwnd, BOOL real,
                                      emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name);
        NTSTATUS handle_NtUserSetWindowsHookEx();
        NTSTATUS handle_NtUserUnhookWindowsHookEx();
        hwnd handle_NtUserCreateWindowEx(const syscall_context& c, DWORD ex_style, emulator_object<LARGE_STRING> class_name,
                                         emulator_object<LARGE_STRING> cls_version, emulator_object<LARGE_STRING> window_name, DWORD style,
                                         int x, int y, int width, int height, hwnd parent, hmenu menu, hinstance instance, pointer l_param,
                                         DWORD flags, pointer acbi_buffer);
        hwnd completion_NtUserCreateWindowEx(const syscall_context& c, DWORD ex_style, emulator_object<LARGE_STRING> class_name,
                                             emulator_object<LARGE_STRING> cls_version, emulator_object<LARGE_STRING> window_name,
                                             DWORD style, int x, int y, int width, int height, hwnd parent, hmenu menu, hinstance instance,
                                             pointer l_param, DWORD flags, pointer acbi_buffer);
        BOOL handle_NtUserDestroyWindow(const syscall_context& c, hwnd window);
        BOOL completion_NtUserDestroyWindow(const syscall_context& c, hwnd window);
        BOOL handle_NtUserSetProp(const syscall_context& c, hwnd window, uint16_t atom, uint64_t data);
        BOOL handle_NtUserSetProp2(const syscall_context& c, hwnd window, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str,
                                   uint64_t data);
        uint64_t handle_NtUserGetProp(const syscall_context& c, hwnd window, uint16_t atom);
        uint64_t handle_NtUserGetProp2(const syscall_context& c, hwnd window, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str);
        uint64_t handle_NtUserRemoveProp(const syscall_context& c, hwnd window, uint16_t atom);
        BOOL handle_NtUserChangeWindowMessageFilterEx();
        BOOL handle_NtUserChangeWindowMessageFilter();
        BOOL handle_NtUserShowWindow(const syscall_context& c, hwnd hwnd, LONG cmd_show);
        BOOL completion_NtUserShowWindow(const syscall_context& c, hwnd hwnd, LONG cmd_show);
        uint64_t handle_NtUserMessageCall(const syscall_context& c, hwnd hwnd, UINT msg, uint64_t w_param, uint64_t l_param,
                                          uint64_t result_info, DWORD type, BOOL ansi);
        uint64_t completion_NtUserMessageCall(const syscall_context& c, hwnd hwnd, UINT msg, uint64_t w_param, uint64_t l_param,
                                              uint64_t result_info, DWORD type, BOOL ansi);
        uint64_t handle_NtUserDispatchMessage(const syscall_context& c, emulator_object<msg> message);
        BOOL handle_NtUserTranslateMessage(const syscall_context& c, emulator_object<msg> message, UINT flags);
        BOOL handle_NtUserGetMessage(const syscall_context& c, emulator_object<msg> message, hwnd hwnd, UINT msg_filter_min,
                                     UINT msg_filter_max);
        BOOL handle_NtUserPeekMessage(const syscall_context& c, emulator_object<msg> message, hwnd hwnd, UINT msg_filter_min,
                                      UINT msg_filter_max, UINT remove_message);
        BOOL handle_NtUserWaitMessage(const syscall_context& c);
        BOOL handle_NtUserInvalidateRect(const syscall_context& c, hwnd hwnd, emulator_object<RECT> rect, BOOL erase);
        BOOL handle_NtUserValidateRect(const syscall_context& c, hwnd hwnd, emulator_object<RECT> rect);
        BOOL handle_NtUserGetUpdateRect(const syscall_context& c, hwnd hwnd, emulator_object<RECT> rect, BOOL erase);
        BOOL handle_NtUserUpdateWindow(const syscall_context& c, hwnd hwnd);
        BOOL completion_NtUserUpdateWindow(const syscall_context& c, hwnd hwnd);
        int32_t handle_NtUserGetKeyNameText(const syscall_context& c, int32_t l_param, emulator_pointer buffer, int32_t character_count);
        BOOL handle_NtUserPostMessage(const syscall_context& c, hwnd hwnd, UINT msg, uint64_t wParam, uint64_t lParam);
        BOOL handle_NtUserPostThreadMessage(const syscall_context& c, DWORD id_thread, UINT msg, uint64_t wParam, uint64_t lParam);
        BOOL handle_NtUserPostQuitMessage(const syscall_context& c, int exit_code);
        NTSTATUS handle_NtUserEnumDisplayDevices(const syscall_context& c,
                                                 emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str_device, DWORD dev_num,
                                                 emulator_object<EMU_DISPLAY_DEVICEW> display_device, DWORD flags);
        NTSTATUS handle_NtUserEnumDisplaySettings(const syscall_context& c,
                                                  emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> device_name, DWORD mode_num,
                                                  emulator_object<EMU_DEVMODEW> dev_mode, DWORD flags);
        LONG handle_NtUserChangeDisplaySettings(const syscall_context& c,
                                                emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> device_name,
                                                emulator_object<EMU_DEVMODEW> dev_mode, hwnd window, DWORD flags, uint64_t param);
        NTSTATUS handle_NtUserBuildHwndList(const syscall_context& c, hdesk desktop, hwnd hwnd_next, BOOL children, BOOL remove_immersive,
                                            DWORD thread_id, UINT hwnd_max, emulator_pointer hwnd_list, emulator_object<UINT> hwnd_needed);
        BOOL handle_NtUserEnumDisplayMonitors(const syscall_context& c, hdc hdc_in, uint64_t clip_rect_ptr, uint64_t callback,
                                              uint64_t param);
        BOOL handle_NtUserGetDpiForMonitor(const syscall_context& c, handle monitor, uint32_t dpi_type, emulator_object<uint32_t> dpi_x,
                                           emulator_object<uint32_t> dpi_y);
        BOOL completion_NtUserEnumDisplayMonitors(const syscall_context& c, hdc hdc_in, uint64_t clip_rect_ptr, uint64_t callback,
                                                  uint64_t param);
        BOOL handle_NtUserInheritWindowMonitor(const syscall_context& c, hwnd hwnd_tgt, hwnd hwnd_inherit);
        BOOL handle_NtUserGetHDevName(const syscall_context& c, handle hdev, emulator_pointer device_name);
        emulator_pointer handle_NtUserMapDesktopObject(const syscall_context& c, handle handle);
        BOOL handle_NtUserTransformRect(const syscall_context& c, emulator_object<RECT> rect, hwnd hwnd, uint32_t type, uint64_t unknown);
        hwnd handle_NtUserSetParent(const syscall_context& c, hwnd hwnd_child, hwnd hwnd_new_parent);
        BOOL handle_NtUserSetWindowPos(const syscall_context& c, hwnd hWnd, hwnd hwnd_insert_after, int x, int y, int cx, int cy,
                                       UINT flags);
        NTSTATUS handle_NtUserSetForegroundWindow();
        hwnd handle_NtUserGetForegroundWindow(const syscall_context& c);
        hwnd handle_NtUserSetFocus(const syscall_context& c, hwnd hwnd);
        emulator_pointer handle_NtUserSetWindowLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong,
                                                       BOOL Ansi);
        emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong,
                                                      BOOL Ansi);
        uint32_t handle_NtUserSetWindowLong(const syscall_context& c, handle hWnd, int nIndex, uint32_t dwNewLong, BOOL Ansi);
        uint64_t handle_NtUserGetAncestor(const syscall_context& c, hwnd child_hwnd, UINT flags);
        BOOL handle_NtUserRedrawWindow(const syscall_context& c, hwnd hwnd, emulator_object<RECT> update_rect, uint64_t update_rgn,
                                       UINT flags);
        NTSTATUS handle_NtUserGetCPD();
        BOOL handle_NtUserSetWindowFNID(const syscall_context& c, hwnd hwnd, WORD fnid);
        BOOL handle_NtUserSetDialogPointer(const syscall_context& c, hwnd hwnd, emulator_pointer ptr);
        BOOL handle_NtUserSetDialogSystemMenu(const syscall_context& c, hwnd hwnd);
        BOOL handle_NtUserSetMsgBox(const syscall_context& c, hwnd hwnd);
        BOOL handle_NtUserEnableWindow(const syscall_context& c, hwnd hwnd, BOOL enable);
        BOOL handle_NtUserDeleteMenu(const syscall_context& c, uint64_t menu, UINT position, UINT flags);
        uint64_t handle_NtUserGetSystemMenu(const syscall_context& c, hwnd hwnd, BOOL revert);
        BOOL handle_NtUserAllowSetForegroundWindow();
        ULONG handle_NtUserGetAtomName(const syscall_context& c, RTL_ATOM atom,
                                       emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> atom_name);
        NTSTATUS handle_NtUserGetDisplayConfigBufferSizes(const syscall_context& c, UINT32 flags,
                                                          emulator_object<UINT32> num_path_array_elements,
                                                          emulator_object<UINT32> num_mode_info_array_elements);
        NTSTATUS handle_NtUserQueryDisplayConfig(const syscall_context& c, UINT32 flags, emulator_object<UINT32> num_path_array_elements,
                                                 emulator_pointer path_array, emulator_object<UINT32> current_topology_id,
                                                 emulator_pointer reserved);
        NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo(const syscall_context& c, emulator_pointer packet);
        uint64_t handle_NtUserInitThreadCoreMessagingIocp2(const syscall_context& c, handle window_handle,
                                                           emulator_object<uint32_t> completion_queue_index);
        BOOL handle_NtUserDrainThreadCoreMessagingCompletions2();
        uint64_t handle_NtUserScheduleDispatchNotification(const syscall_context& c, hwnd hwnd);
        uint64_t handle_NtUserSetTimer(const syscall_context& c, hwnd hwnd, uint64_t timer_id, uint32_t elapsed_ms, uint64_t timer_proc);
        uint64_t handle_NtUserSetSystemTimer(const syscall_context& c, hwnd hwnd, uint64_t timer_id, uint32_t elapsed_ms);
        BOOL handle_NtUserKillTimer(const syscall_context& c, hwnd hwnd, uint64_t timer_id);
        BOOL handle_NtUserValidateTimerCallback(const syscall_context& c, uint64_t timer_proc);
        uint32_t handle_NtUserGetQueueStatusReadonly(const syscall_context& c, UINT flags);
        uint32_t handle_NtUserGetQueueStatus(const syscall_context& c, UINT flags);
        uint64_t handle_NtUserCreateAcceleratorTable(const syscall_context& c, emulator_pointer entries, int32_t entry_count);
        BOOL handle_NtUserDestroyAcceleratorTable(const syscall_context& c, handle accelerator_table);
        int32_t handle_NtUserCopyAcceleratorTable();
        int32_t handle_NtUserTranslateAccelerator();
        hmenu handle_NtUserCreateMenu(const syscall_context& c);
        BOOL handle_NtUserThunkedMenuItemInfo(const syscall_context& c, hmenu menu, UINT position, BOOL by_position, BOOL insert,
                                              emulator_object<EMU_MENUITEMINFO> item_info,
                                              emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> item_text);
        hmenu handle_NtUserCreatePopupMenu(const syscall_context& c);
        BOOL handle_NtUserSetMenu();
        BOOL handle_NtUserSetMenuDefaultItem(const syscall_context& c, hmenu menu, UINT item, UINT by_position);
        BOOL handle_NtUserEndMenu();
        BOOL handle_NtUserRemoveMenu(const syscall_context& c, hmenu menu, UINT position, UINT flags);
        BOOL handle_NtUserDestroyMenu(const syscall_context& c, hmenu menu);
        BOOL handle_NtUserDrawMenuBar(const syscall_context& c, hwnd hwnd);
        BOOL handle_NtUserSetWindowCompositionAttribute(const syscall_context& c, hwnd hwnd, emulator_pointer data);
        BOOL handle_NtUserCreateCaret();
        BOOL handle_NtUserDestroyCaret();
        BOOL handle_NtUserSetCaretPos();
        BOOL handle_NtUserShowCaret();
        BOOL handle_NtUserHideCaret();
        BOOL handle_NtUserGetObjectInformation();
        uint64_t handle_NtUserQueryWindow(const syscall_context& c, hwnd window_handle, uint32_t query_type);
        int handle_NtUserSetScrollInfo();
        BOOL handle_NtUserIsTouchWindow();
        BOOL handle_NtUserGetWindowPlacement();
        BOOL handle_NtUserTrackMouseEvent();
        BOOL handle_NtUserSetWindowRgn();
        BOOL handle_NtUserAlterWindowStyle();
        BOOL handle_NtUserSetActiveWindow();
        NTSTATUS handle_NtUserSelectPalette();
        BOOL handle_NtUserSwapMouseButton();
        hwnd handle_NtUserWindowFromPoint(const syscall_context& c, int32_t x, int32_t y);
        BOOL handle_NtUserGetKeyboardState(const syscall_context& c, emulator_pointer key_state);
        uint32_t handle_NtUserGetDoubleClickTime();
        BOOL handle_NtUserModifyWindowTouchCapability();
        uint32_t handle_NtUserGetClipboardSequenceNumber();
        BOOL handle_NtUserOpenClipboard();
        BOOL handle_NtUserCloseClipboard();
        BOOL handle_NtUserEmptyClipboard();
        uint64_t handle_NtUserGetClipboardData();
        uint64_t handle_NtUserConvertMemHandle();
        uint64_t handle_NtUserSetClipboardData();
        uint64_t handle_NtUserGetProcessDpiAwarenessContext();
        NTSTATUS handle_NtUserSetProcessDpiAwarenessContext();
        uint32_t handle_NtUserMapVirtualKeyEx(const syscall_context& c, uint32_t code, uint32_t map_type, uint32_t keyboard_id,
                                              uint64_t keyboard_layout);
        NTSTATUS handle_NtUserToUnicodeEx();
        uint64_t handle_NtUserSetKeyboardState();
        uint64_t handle_NtUserAttachThreadInput();
        BOOL handle_NtUserRegisterTouchHitTestingWindow();
        BOOL handle_NtUserGetGUIThreadInfo(const syscall_context& c, uint32_t thread_id, emulator_pointer info);
        BOOL handle_NtUserSetWinEventHook();
        BOOL handle_NtUserUnhookWinEvent();
        BOOL handle_NtUserDisableThreadIme();
        BOOL handle_NtUserGetPointerDevices();
        BOOL handle_NtUserHwndQueryRedirectionInfo();

        // syscalls/gdi.cpp:
        NTSTATUS handle_NtDxgkIsFeatureEnabled();
        NTSTATUS handle_NtGdiInit(const syscall_context& c);
        NTSTATUS handle_NtGdiInit2(const syscall_context& c);
        uint32_t handle_NtGdiGetDeviceCaps(const syscall_context& c, hdc dc, uint32_t index);
        uint32_t handle_NtGdiGetDeviceCapsAll(const syscall_context& c, hdc dc, emulator_pointer caps);
        uint32_t handle_NtGdiComputeXformCoefficients(const syscall_context& c, hdc dc);
        BOOL handle_NtGdiFlush(const syscall_context& c);
        uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
        uint64_t handle_NtGdiCreatePatternBrushInternal(const syscall_context& c, handle bitmap, uint32_t unused);
        uint64_t handle_NtGdiCreatePen(const syscall_context& c, uint32_t style, uint32_t width, uint32_t color);
        uint64_t handle_NtGdiCreatePaletteInternal(const syscall_context& c);
        uint64_t handle_NtGdiCreateHalftonePalette(const syscall_context& c);
        NTSTATUS handle_NtGdiDoPalette();
        uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
        int32_t handle_NtGdiSaveDC(const syscall_context& c, hdc dc);
        BOOL handle_NtGdiRestoreDC(const syscall_context& c, hdc dc, int32_t saved_dc);
        uint64_t handle_NtGdiAddFontMemResourceEx(const syscall_context& c, emulator_pointer buffer, uint32_t buffer_size,
                                                  emulator_pointer design_vector, uint32_t design_vector_size,
                                                  emulator_object<uint32_t> num_fonts);
        BOOL handle_NtGdiRemoveFontMemResourceEx(const syscall_context& c, uint64_t font_handle);
        uint64_t handle_NtGdiCreateCompatibleBitmap(const syscall_context& c, hdc dc, uint32_t width, uint32_t height);
        uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                          emulator_pointer bits);
        uint64_t handle_NtGdiCreateDIBSection(const syscall_context& c, hdc dc, uint64_t section_app, uint32_t offset,
                                              emulator_pointer info, uint32_t usage, uint32_t header_size, uint32_t flags,
                                              uint64_t color_space, emulator_object<emulator_pointer> bits);
        uint64_t handle_NtGdiCreateDIBitmapInternal(const syscall_context& c, hdc dc, uint32_t width, uint32_t height, uint32_t usage,
                                                    emulator_pointer bits, emulator_pointer info, uint32_t info_header_size, uint32_t init,
                                                    uint32_t offset, uint32_t cj, uint32_t i_usage);
        int handle_NtGdiSetDIBitsToDeviceInternal(const syscall_context& c, hdc dc, int x_dest, int y_dest, uint32_t width, uint32_t height,
                                                  int x_src, int y_src, uint32_t start_scan, uint32_t scan_lines, emulator_pointer bits,
                                                  emulator_pointer info, uint32_t color_use, uint32_t max_bits, uint32_t max_info,
                                                  uint32_t transform_coordinates, uint64_t color_transform);
        int handle_NtGdiGetDIBitsInternal(const syscall_context& c, hdc dc, handle bitmap, uint32_t start_scan, uint32_t scan_lines,
                                          emulator_pointer bits, emulator_pointer info, uint32_t usage, uint32_t max_bits,
                                          uint32_t max_info);
        int handle_NtGdiStretchDIBitsInternal(const syscall_context& c, hdc dc, int x_dst, int y_dst, int dst_width, int dst_height,
                                              int x_src, int y_src, int src_width, int src_height, emulator_pointer bits,
                                              emulator_pointer info, uint32_t usage, uint32_t rop, uint32_t max_info, uint32_t max_bits,
                                              uint64_t color_transform);
        uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
        uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
        uint64_t handle_NtGdiSelectFont(const syscall_context& c, hdc dc, uint64_t font);
        hdc handle_NtGdiGetDCforBitmap(const syscall_context& c, handle bitmap);
        BOOL handle_NtGdiGetDCDword(const syscall_context& c, hdc dc, uint32_t index, emulator_pointer result);
        BOOL handle_NtGdiSetBrushOrg(const syscall_context& c, hdc dc, int x, int y, emulator_pointer prev);
        uint64_t handle_NtGdiHfontCreate(const syscall_context& c, emulator_pointer logfont, uint32_t angle);
        uint32_t handle_NtGdiExtGetObjectW(const syscall_context& c, uint32_t handle_value, uint32_t size, emulator_pointer buffer);
        BOOL handle_NtGdiEnumFonts(const syscall_context& c, hdc dc, ULONG type, ULONG win32_compat, ULONG face_name_len,
                                   emulator_pointer face_name, ULONG charset, emulator_pointer count, emulator_pointer buffer);
        uint32_t handle_NtGdiGetTextCharsetInfo(const syscall_context& c, hdc dc, emulator_pointer sig, uint32_t flags);
        uint32_t handle_NtGdiQueryFontAssocInfo(const syscall_context& c, hdc dc);
        uint32_t handle_NtGdiGetPublicFontTableChangeCookie();
        int32_t handle_NtGdiAddFontResourceW(const syscall_context& c, emulator_pointer files, uint32_t character_count,
                                             uint32_t file_count, uint32_t flags, uint32_t thread_id, emulator_pointer design_vector);
        uint32_t handle_NtGdiGetTextMetricsW(const syscall_context& c, hdc dc, emulator_pointer ptm, uint32_t cj);
        int32_t handle_NtGdiGetTextFaceW(const syscall_context& c, hdc dc, int32_t count, emulator_pointer face_name, BOOL alias_name);
        uint32_t handle_NtGdiGetGlyphOutline(const syscall_context& c, hdc dc, UINT character, UINT format, emulator_pointer glyph_metrics,
                                             DWORD buffer_size, emulator_pointer buffer, emulator_pointer mat2);
        uint32_t handle_NtGdiGetOutlineTextMetricsInternalW(const syscall_context& c, hdc dc, uint32_t cj_copy, emulator_pointer metrics,
                                                            emulator_pointer unknown);
        BOOL handle_NtGdiGetTextExtent(const syscall_context& c, hdc dc, emulator_pointer text, int32_t char_count, emulator_pointer size,
                                       ULONG flags);
        BOOL handle_NtGdiGetCharWidthW(const syscall_context& c, hdc dc, UINT first_char, UINT char_count, emulator_pointer chars,
                                       UINT flags, emulator_pointer buffer);
        BOOL handle_NtGdiGetCharABCWidthsW(const syscall_context& c, hdc dc, UINT first_char, UINT char_count, emulator_pointer chars,
                                           UINT flags, emulator_pointer buffer);
        NTSTATUS handle_NtGdiExtCreateRegion();
        BOOL handle_NtGdiTransparentBlt(const syscall_context& c, hdc dst_dc, int x_dst, int y_dst, int dst_width, int dst_height,
                                        hdc src_dc, int x_src, int y_src, int src_width, int src_height, COLORREF transparent_color);
        uint64_t handle_NtGdiCreateRectRgn(const syscall_context& c, LONG x_left, LONG y_top, LONG x_right, LONG y_bottom);
        int32_t handle_NtGdiGetRandomRgn(const syscall_context& c, hdc dc, uint64_t region, LONG index);
        uint32_t handle_NtGdiGetRegionData(const syscall_context& c, handle hrgn, ULONG buffer_size, emulator_pointer region_data);
        int32_t handle_NtGdiGetAppClipBox(const syscall_context& c, hdc dc, emulator_object<RECT> rect);
        int32_t handle_NtGdiExcludeClipRect(const syscall_context& c, hdc dc, LONG x_left, LONG y_top, LONG x_right, LONG y_bottom);
        int32_t handle_NtGdiIntersectClipRect(const syscall_context& c, hdc dc, LONG x_left, LONG y_top, LONG x_right, LONG y_bottom);
        BOOL handle_NtGdiRectVisible(const syscall_context& c, hdc dc, emulator_object<RECT> rect);
        uint32_t handle_NtGdiGetCharSet(const syscall_context& c, hdc dc);
        int32_t handle_NtGdiExtSelectClipRgn(const syscall_context& c, hdc dc, uint64_t region, LONG mode);
        BOOL handle_NtGdiLineTo(const syscall_context& c, hdc dc, LONG x_end, LONG y_end);
        BOOL handle_NtGdiRectangle(const syscall_context& c, hdc dc, LONG left, LONG top, LONG right, LONG bottom);
        BOOL handle_NtGdiPatBlt(const syscall_context& c, hdc dc, LONG x, LONG y, LONG width, LONG height, DWORD rop);
        COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
        COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
        BOOL handle_NtGdiBitBlt(const syscall_context& c, hdc dst_dc, int x_dst, int y_dst, int width, int height, hdc src_dc, int x_src,
                                int y_src, DWORD rop, DWORD cr_back_color, FLONG fl);
        BOOL handle_NtGdiStretchBlt(const syscall_context& c, hdc dst_dc, int x_dst, int y_dst, int dst_width, int dst_height, hdc src_dc,
                                    int x_src, int y_src, int src_width, int src_height, DWORD rop, DWORD cr_back_color);
        BOOL handle_NtGdiPolyPatBlt(const syscall_context& c, hdc dc, DWORD rop, emulator_pointer poly, DWORD count, DWORD mode);
        BOOL handle_NtGdiExtTextOutW(const syscall_context& c, hdc dc, LONG x, LONG y, UINT options, emulator_pointer rect,
                                     emulator_pointer text, UINT count, emulator_pointer dx, DWORD code_page);
        BOOL handle_NtGdiGetRealizationInfo(const syscall_context& c, hdc dc, emulator_pointer realization_info, uint64_t font);
        NTSTATUS handle_NtGdiGetEntry(const syscall_context& c, uint32_t handle_value, emulator_pointer entry_ptr);
        int32_t handle_NtGdiSetIcmMode();
        NTSTATUS handle_NtGdiSetLayout();
        NTSTATUS handle_NtGdiGetDCObject();
        BOOL handle_NtGdiUnrealizeObject(const syscall_context& c, handle h);
        BOOL handle_NtGdiMoveToEx(const syscall_context& c, hdc dc, LONG x, LONG y, emulator_pointer old_point_ptr);
        uint64_t handle_NtGdiSelectBrushLocal(const syscall_context& c, hdc dc, uint32_t brush, emulator_pointer old_brush_ptr);
        uint64_t handle_NtGdiSelectPenLocal(const syscall_context& c, hdc dc, uint32_t pen, emulator_pointer old_pen_ptr);
        hdc handle_NtGdiOpenDCW(const syscall_context& c);
        NTSTATUS handle_NtGdiDdDDIEnumAdapters2(const syscall_context& c, emulator_object<EMU_D3DKMT_ENUMADAPTERS2> enum_adapters);
        NTSTATUS handle_NtDxgkEnumAdapters3(const syscall_context& c, emulator_object<EMU_D3DKMT_ENUMADAPTERS3> enum_adapters);
        NTSTATUS handle_NtDxgkGetProperties(const syscall_context& c, emulator_object<EMU_D3DKMT_GET_PROPERTIES> get_properties);
        NTSTATUS handle_NtGdiDdDDICloseAdapter();
        NTSTATUS handle_NtGdiDdDDIQueryAdapterInfo(const syscall_context& c, emulator_object<EMU_D3DKMT_QUERYADAPTERINFO> query_adapter);
        NTSTATUS handle_NtGdiDdDDICreateDevice(const syscall_context& c, emulator_object<EMU_D3DKMT_CREATEDEVICE> device_desc);
        NTSTATUS handle_NtGdiDdDDIEscape(const syscall_context& c, emulator_object<EMU_D3DKMT_ESCAPE> escape_desc);
        NTSTATUS handle_NtGdiDdDDICreateContext(const syscall_context& c, emulator_object<EMU_D3DKMT_CREATECONTEXT> context_desc);
        NTSTATUS handle_NtGdiDdDDICreateAllocation(const syscall_context& c, emulator_object<EMU_D3DKMT_CREATEALLOCATION> allocation_desc);
        NTSTATUS handle_NtGdiDdDDIQueryResourceInfo(const syscall_context& c, emulator_object<EMU_D3DKMT_QUERYRESOURCEINFO> resource_info);
        NTSTATUS handle_NtGdiDdDDIOpenResource(const syscall_context& c, emulator_object<EMU_D3DKMT_OPENRESOURCE> open_resource);
        NTSTATUS handle_NtGdiDdDDILock(const syscall_context& c, emulator_object<EMU_D3DKMT_LOCK> lock_desc);
        NTSTATUS handle_NtGdiDdDDIUnlock();
        NTSTATUS handle_NtGdiDdDDIGetDisplayModeList(const syscall_context& c,
                                                     emulator_object<EMU_D3DKMT_GETDISPLAYMODELIST> display_mode_list);
        NTSTATUS handle_NtGdiDdDDIGetSharedPrimaryHandle(const syscall_context& c,
                                                         emulator_object<EMU_D3DKMT_GETSHAREDPRIMARYHANDLE> shared_primary);
        NTSTATUS handle_NtGdiDdDDIGetDeviceState(const syscall_context& c, emulator_object<EMU_D3DKMT_GETDEVICESTATE> device_state);
        NTSTATUS handle_NtGdiDdDDIMarkDeviceAsError(const syscall_context& c, emulator_object<EMU_D3DKMT_MARKDEVICEASERROR> mark_error);
        NTSTATUS handle_NtGdiDdDDIGetCachedHybridQueryValue(const syscall_context& c, emulator_object<uint32_t> value);
        NTSTATUS handle_NtGdiDdDDICacheHybridQueryValue();
        NTSTATUS handle_NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport(
            const syscall_context& c, emulator_object<EMU_D3DKMT_MIRACAST_DISPLAY_DEVICE_CAPS> display_caps);
        NTSTATUS handle_NtGdiDdDDIDestroyAllocation2(const syscall_context& c,
                                                     emulator_object<EMU_D3DKMT_DESTROYALLOCATION2> destroy_allocation);
        NTSTATUS handle_NtGdiDdDDIDestroyAllocation(const syscall_context& c,
                                                    emulator_object<EMU_D3DKMT_DESTROYALLOCATION> destroy_allocation);
        NTSTATUS handle_NtGdiDdDDIDestroyContext();
        NTSTATUS handle_NtGdiDdDDIDestroyDevice();
        NTSTATUS handle_NtGdiDdDDICreateDCFromMemory(const syscall_context& c, emulator_object<EMU_D3DKMT_CREATEDCFROMMEMORY> create_dc);
        NTSTATUS handle_NtGdiDdDDIDestroyDCFromMemory(const syscall_context& c, emulator_object<EMU_D3DKMT_DESTROYDCFROMMEMORY> destroy_dc);
        NTSTATUS handle_NtGdiDdDDIOpenAdapterFromLuid(const syscall_context& c,
                                                      emulator_object<EMU_D3DKMT_OPENADAPTERFROMLUID> open_adapter);
        NTSTATUS handle_NtGdiDdDDIOpenAdapterFromHdc(const syscall_context& c, emulator_object<EMU_D3DKMT_OPENADAPTERFROMHDC> open_adapter);

        // syscalls/trace.cpp:
        NTSTATUS handle_NtTraceControl(const syscall_context& c, ULONG function_code, uint64_t input_buffer, ULONG input_buffer_length,
                                       uint64_t output_buffer, ULONG output_buffer_length, emulator_object<ULONG> return_length);

        // syscalls/io_completion.cpp:
        NTSTATUS handle_NtCreateIoCompletion(const syscall_context& c, emulator_object<handle> io_completion_handle,
                                             ACCESS_MASK desired_access,
                                             emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                             ULONG number_of_concurrent_threads);
        NTSTATUS handle_NtSetIoCompletion(const syscall_context& c, handle io_completion_handle, emulator_pointer key_context,
                                          emulator_pointer apc_context, NTSTATUS io_status,
                                          EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) io_status_information);
        NTSTATUS handle_NtSetIoCompletionEx(const syscall_context& c, handle io_completion_handle, handle io_completion_packet_handle,
                                            emulator_pointer key_context, emulator_pointer apc_context, NTSTATUS io_status,
                                            EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) io_status_information);
        NTSTATUS handle_NtRemoveIoCompletion(const syscall_context& c, handle io_completion_handle,
                                             emulator_object<emulator_pointer> key_context, emulator_object<emulator_pointer> apc_context,
                                             emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                             emulator_object<LARGE_INTEGER> timeout);
        NTSTATUS handle_NtRemoveIoCompletionEx(
            const syscall_context& c, handle io_completion_handle,
            emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> io_completion_information, ULONG count,
            emulator_object<ULONG> num_entries_removed, emulator_object<LARGE_INTEGER> timeout, BOOLEAN alertable);
        NTSTATUS handle_NtCreateWaitCompletionPacket(const syscall_context& c, emulator_object<handle> wait_packet_handle,
                                                     ACCESS_MASK desired_access,
                                                     emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
        NTSTATUS handle_NtAssociateWaitCompletionPacket(const syscall_context& c, handle wait_completion_packet_handle,
                                                        handle io_completion_handle, handle target_object_handle,
                                                        emulator_pointer key_context, emulator_pointer apc_context, NTSTATUS io_status,
                                                        EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) io_status_information,
                                                        emulator_object<BOOLEAN> already_signaled);
        NTSTATUS handle_NtCancelWaitCompletionPacket(const syscall_context& c, handle wait_completion_packet_handle,
                                                     BOOLEAN remove_signaled_packet);

        // syscalls/worker_factory.cpp:
        NTSTATUS handle_NtCreateWorkerFactory(const syscall_context& c, emulator_object<handle> worker_factory_handle,
                                              ACCESS_MASK desired_access,
                                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                              handle io_completion_handle, handle worker_process_handle, emulator_pointer start_routine,
                                              emulator_pointer start_parameter, ULONG max_thread_count,
                                              EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) stack_reserve,
                                              EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) stack_commit);
        NTSTATUS handle_NtWorkerFactoryWorkerReady(const syscall_context& c, handle worker_factory_handle);
        NTSTATUS handle_NtSetInformationWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                                      WORKERFACTORYINFOCLASS info_class, emulator_pointer worker_factory_information,
                                                      ULONG worker_factory_information_length);
        NTSTATUS handle_NtShutdownWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                                emulator_object<LONG> pending_worker_count);
        NTSTATUS handle_NtReleaseWorkerFactoryWorker(const syscall_context& c, handle worker_factory_handle);
        NTSTATUS handle_NtWaitForWorkViaWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                                      emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> mini_packets,
                                                      ULONG count, emulator_object<ULONG> packets_returned, emulator_pointer deferred_work);

        NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context& c, const emulator_object<LARGE_INTEGER> performance_counter,
                                                  const emulator_object<LARGE_INTEGER> performance_frequency)
        {
            try
            {
                if (performance_counter)
                {
                    performance_counter.access([&](LARGE_INTEGER& value) {
                        value.QuadPart = c.win_emu.clock().steady_now().time_since_epoch().count(); //
                    });
                }

                if (performance_frequency)
                {
                    performance_frequency.access([&](LARGE_INTEGER& value) {
                        value.QuadPart = c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.QpcFrequency; }); //
                    });
                }

                return STATUS_SUCCESS;
            }
            catch (...)
            {
                return STATUS_ACCESS_VIOLATION;
            }
        }

        NTSTATUS handle_NtManageHotPatch()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtApphelpCacheControl()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtDeviceIoControlFile(const syscall_context& c, const handle file_handle, const handle event,
                                              const emulator_pointer /*PIO_APC_ROUTINE*/ apc_routine, const emulator_pointer apc_context,
                                              const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                              const ULONG io_control_code, const emulator_pointer input_buffer,
                                              const ULONG input_buffer_length, const emulator_pointer output_buffer,
                                              const ULONG output_buffer_length)
        {
            const auto resolved_file_handle = c.proc.resolve_object_pseudo_handle(file_handle, c.vcpu.active_thread);
            auto* device = c.proc.devices.get(resolved_file_handle);
            if (!device)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (auto* e = c.proc.events.get(event))
            {
                e->signaled = false;
            }

            io_device_context context{c.emu};
            context.event = event;
            context.apc_routine = apc_routine;
            context.apc_context = apc_context;
            context.io_status_block = io_status_block;
            context.io_control_code = io_control_code;
            context.input_buffer = input_buffer;
            context.input_buffer_length = input_buffer_length;
            context.output_buffer = output_buffer;
            context.output_buffer_length = output_buffer_length;
            context.vcpu = &c.vcpu;

            return device->execute_ioctl(c.win_emu, context);
        }

        NTSTATUS handle_NtQueryWnfStateData()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryWnfStateNameInformation()
        {
            // puts("NtQueryWnfStateNameInformation not supported");
            // return STATUS_NOT_SUPPORTED;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtTestAlert(const syscall_context& c)
        {
            c.win_emu.yield_thread(c.vcpu, true);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtUserSystemParametersInfo()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtUpdateWnfStateData()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtQueryInformationJobObject()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtCreateUserProcess()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtCreateDebugObject()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtAddAtomEx(const syscall_context& c, const uint64_t atom_name, const ULONG length,
                                    const emulator_object<RTL_ATOM> atom, const ULONG /*flags*/)
        {
            std::u16string name{};
            name.resize(length / 2);

            c.emu.read_memory(atom_name, name.data(), length);

            uint16_t index = c.proc.add_or_find_atom(name);
            atom.write(index);

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAddAtom(const syscall_context& c, const uint64_t atom_name, const ULONG length,
                                  const emulator_object<RTL_ATOM> atom)
        {
            return handle_NtAddAtomEx(c, atom_name, length, atom, 0);
        }

        NTSTATUS handle_NtDeleteAtom(const syscall_context& c, const RTL_ATOM atom)
        {
            c.proc.delete_atom(atom);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtFindAtom(const syscall_context& c, const uint64_t atom_name, const ULONG length,
                                   const emulator_object<uint16_t> atom)
        {
            const auto name = read_string<char16_t>(c.emu, atom_name, length / 2);
            const auto index = c.proc.find_atom(name);
            if (!index)
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            if (atom)
            {
                atom.write(*index);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryDebugFilterState()
        {
            return FALSE;
        }

        NTSTATUS handle_NtUserGetDpiForCurrentProcess()
        {
            return 96;
        }

        NTSTATUS handle_NtUserModifyUserStartupInfoFlags()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSystemDebugControl()
        {
            return STATUS_DEBUGGER_INACTIVE;
        }

        NTSTATUS handle_NtRequestWaitReplyPort()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtUserGetProcessUIContextInformation()
        {
            return STATUS_NOT_SUPPORTED;
        }

        ULONG handle_NtUserGetKeyboardType()
        {
            return 0;
        }

        NTSTATUS handle_NtSubscribeWnfStateChange()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtUnsubscribeWnfStateChange()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetWnfProcessNotificationEvent()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetInformationDebugObject()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtRemoveProcessDebug()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtNotifyChangeDirectoryFileEx()
        {
            return STATUS_NOT_SUPPORTED;
        }

        uint64_t handle_NtUserCallNoParam()
        {
            return 0;
        }

        NTSTATUS handle_NtAllocateLocallyUniqueId(const syscall_context& c, const emulator_object<LUID> luid)
        {
            luid.access([&](LUID& l) {
                const std::uint64_t value = c.proc.next_luid++;
                l.LowPart = static_cast<std::uint32_t>(value);
                l.HighPart = static_cast<std::int32_t>(value >> 32);
            });

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAllocateReserveObject(const syscall_context& c, const emulator_object<handle> memory_reserve_handle,
                                                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                                const DWORD type)
        {
            std::u16string name{};
            if (object_attributes)
            {
                const auto attributes = object_attributes.read();
                if (attributes.ObjectName != 0)
                {
                    name = read_unicode_string(c.emu, attributes.ObjectName);
                }
            }

            switch (type)
            {
            case 1: { // MemoryReserveIoCompletion
                // TODO: This probably isn't 100% correct.
                wait_completion_packet packet{};
                packet.name = std::move(name);
                memory_reserve_handle.write(c.proc.wait_completion_packets.store(std::move(packet)));
                return STATUS_SUCCESS;
            }
            case 0:
            default: { // MemoryReserveUserApc
                return STATUS_NOT_SUPPORTED;
            }
            }
        }

        NTSTATUS handle_NtNotifyChangeDirectoryFile()
        {
            return STATUS_SUCCESS;
        }

    }

    // NOLINTNEXTLINE(readability-function-size,hicpp-function-size)
    void syscall_dispatcher::add_handlers(std::map<std::string, syscall_handler>& handler_mapping)
    {
#define add_handler(syscall)                                                            \
    do                                                                                  \
    {                                                                                   \
        handler_mapping[#syscall] = make_syscall_handler<syscalls::handle_##syscall>(); \
    } while (0)

        add_handler(NtSetInformationThread);
        add_handler(NtSetThreadExecutionState);
        add_handler(NtSetEvent);
        add_handler(NtPulseEvent);
        add_handler(NtClose);
        add_handler(NtOpenKey);
        add_handler(NtAllocateVirtualMemory);
        add_handler(NtQueryInformationProcess);
        add_handler(NtSetInformationProcess);
        add_handler(NtSetInformationVirtualMemory);
        add_handler(NtFreeVirtualMemory);
        add_handler(NtQueryVirtualMemory);
        add_handler(NtOpenThread);
        add_handler(NtOpenThreadToken);
        add_handler(NtOpenThreadTokenEx);
        add_handler(NtQueryPerformanceCounter);
        add_handler(NtQuerySystemInformation);
        add_handler(NtPowerInformation);
        add_handler(NtCreateEvent);
        add_handler(NtProtectVirtualMemory);
        add_handler(NtLockVirtualMemory);
        add_handler(NtUnlockVirtualMemory);
        add_handler(NtFlushVirtualMemory);
        add_handler(NtOpenDirectoryObject);
        add_handler(NtCreateDirectoryObject);
        add_handler(NtTraceEvent);
        add_handler(NtAllocateVirtualMemoryEx);
        add_handler(NtCreateIoCompletion);
        add_handler(NtSetIoCompletion);
        add_handler(NtSetIoCompletionEx);
        add_handler(NtRemoveIoCompletion);
        add_handler(NtCreateWaitCompletionPacket);
        add_handler(NtCreateWorkerFactory);
        add_handler(NtWorkerFactoryWorkerReady);
        add_handler(NtSetInformationWorkerFactory);
        add_handler(NtShutdownWorkerFactory);
        add_handler(NtWaitForWorkViaWorkerFactory);
        add_handler(NtManageHotPatch);
        add_handler(NtOpenSection);
        add_handler(NtMapViewOfSection);
        add_handler(NtMapViewOfSectionEx);
        add_handler(NtOpenSymbolicLinkObject);
        add_handler(NtQuerySymbolicLinkObject);
        add_handler(NtQuerySystemInformationEx);
        add_handler(NtOpenFile);
        add_handler(NtQueryVolumeInformationFile);
        add_handler(NtApphelpCacheControl);
        add_handler(NtCreateSection);
        add_handler(NtQuerySection);
        add_handler(NtConnectPort);
        add_handler(NtSecureConnectPort);
        add_handler(NtCreateFile);
        add_handler(NtDeviceIoControlFile);
        add_handler(NtQueryWnfStateData);
        add_handler(NtSubscribeWnfStateChange);
        add_handler(NtOpenProcess);
        add_handler(NtOpenProcessToken);
        add_handler(NtOpenProcessTokenEx);
        add_handler(NtQuerySecurityAttributesToken);
        add_handler(NtAdjustPrivilegesToken);
        add_handler(NtQuerySecurityPolicy);
        add_handler(NtQueryLicenseValue);
        add_handler(NtTestAlert);
        add_handler(NtContinue);
        add_handler(NtContinueEx);
        add_handler(NtTerminateProcess);
        add_handler(NtFlushProcessWriteBuffers);
        add_handler(NtWriteFile);
        add_handler(NtCopyFileChunk);
        add_handler(NtLockFile);
        add_handler(NtUnlockFile);
        add_handler(NtRaiseHardError);
        add_handler(NtCreateSemaphore);
        add_handler(NtOpenSemaphore);
        add_handler(NtReadVirtualMemory);
        add_handler(NtWriteVirtualMemory);
        add_handler(NtQueryInformationToken);
        add_handler(NtDxgkIsFeatureEnabled);
        add_handler(NtAddAtomEx);
        add_handler(NtAddAtom);
        add_handler(NtFindAtom);
        add_handler(NtDeleteAtom);
        add_handler(NtUserGetAtomName);
        add_handler(NtInitializeNlsFiles);
        add_handler(NtUnmapViewOfSection);
        add_handler(NtUnmapViewOfSectionEx);
        add_handler(NtDuplicateObject);
        add_handler(NtQueryInformationThread);
        add_handler(NtQueryWnfStateNameInformation);
        add_handler(NtAlpcSendWaitReceivePort);
        add_handler(NtGdiInit);
        add_handler(NtGdiGetDeviceCaps);
        add_handler(NtGdiGetDeviceCapsAll);
        add_handler(NtGdiComputeXformCoefficients);
        add_handler(NtGdiFlush);
        add_handler(NtGdiCreateSolidBrush);
        add_handler(NtGdiCreatePatternBrushInternal);
        add_handler(NtGdiCreatePen);
        add_handler(NtGdiCreateCompatibleDC);
        add_handler(NtGdiSaveDC);
        add_handler(NtGdiRestoreDC);
        add_handler(NtGdiAddFontMemResourceEx);
        add_handler(NtGdiRemoveFontMemResourceEx);
        add_handler(NtGdiCreateCompatibleBitmap);
        add_handler(NtGdiCreateBitmap);
        add_handler(NtGdiCreateDIBitmapInternal);
        add_handler(NtGdiSetDIBitsToDeviceInternal);
        add_handler(NtGdiGetDIBitsInternal);
        add_handler(NtGdiStretchDIBitsInternal);
        add_handler(NtGdiDeleteObjectApp);
        add_handler(NtGdiSelectBitmap);
        add_handler(NtGdiGetDCforBitmap);
        add_handler(NtGdiGetDCDword);
        add_handler(NtGdiSetBrushOrg);
        add_handler(NtGdiHfontCreate);
        add_handler(NtGdiExtGetObjectW);
        add_handler(NtGdiEnumFonts);
        add_handler(NtGdiGetTextCharsetInfo);
        add_handler(NtGdiQueryFontAssocInfo);
        add_handler(NtGdiGetPublicFontTableChangeCookie);
        add_handler(NtGdiAddFontResourceW);
        add_handler(NtGdiGetTextMetricsW);
        add_handler(NtGdiGetTextFaceW);
        add_handler(NtGdiGetTextExtent);
        add_handler(NtGdiGetCharWidthW);
        add_handler(NtGdiGetCharABCWidthsW);
        add_handler(NtGdiGetGlyphOutline);
        add_handler(NtGdiCreateRectRgn);
        add_handler(NtGdiGetRandomRgn);
        add_handler(NtGdiGetRegionData);
        add_handler(NtGdiGetAppClipBox);
        add_handler(NtGdiExcludeClipRect);
        add_handler(NtGdiIntersectClipRect);
        add_handler(NtGdiRectVisible);
        add_handler(NtGdiGetCharSet);
        add_handler(NtGdiExtSelectClipRgn);
        add_handler(NtGdiLineTo);
        add_handler(NtGdiRectangle);
        add_handler(NtGdiPatBlt);
        add_handler(NtGdiBitBlt);
        add_handler(NtGdiStretchBlt);
        add_handler(NtGdiTransparentBlt);
        add_handler(NtGdiPolyPatBlt);
        add_handler(NtGdiExtTextOutW);
        add_handler(NtGdiGetRealizationInfo);
        add_handler(NtGdiGetEntry);
        add_handler(NtGdiInit2);
        add_handler(NtGdiMoveToEx);
        add_handler(NtGdiSelectBrushLocal);
        add_handler(NtGdiSelectPenLocal);
        add_handler(NtGdiUnrealizeObject);
        add_handler(NtUserGetThreadState);
        add_handler(NtUserSetThreadState);
        add_handler(NtUserProcessConnect);
        add_handler(NtUserInitializeClientPfnArrays);
        add_handler(NtUserRemoteConnectState);
        add_handler(NtUserGetThreadDesktop);
        add_handler(NtOpenKeyEx);
        add_handler(NtUserTraceLoggingSendMixedModeTelemetry);
        add_handler(NtUserDisplayConfigGetDeviceInfo);
        add_handler(NtOpenEvent);
        add_handler(NtGetMUIRegistryInfo);
        add_handler(NtIsUILanguageComitted);
        add_handler(NtQueryDefaultUILanguage);
        add_handler(NtQueryInstallUILanguage);
        add_handler(NtUpdateWnfStateData);
        add_handler(NtRaiseException);
        add_handler(NtQueryInformationJobObject);
        add_handler(NtSetSystemInformation);
        add_handler(NtQueryInformationFile);
        add_handler(NtCreateThreadEx);
        add_handler(NtQueryDebugFilterState);
        add_handler(NtWaitForSingleObject);
        add_handler(NtTerminateThread);
        add_handler(NtDelayExecution);
        add_handler(NtWaitForAlertByThreadId);
        add_handler(NtAlertThreadByThreadIdEx);
        add_handler(NtAlertThreadByThreadId);
        add_handler(NtReadFile);
        add_handler(NtSetInformationFile);
        add_handler(NtUserRegisterWindowMessage);
        add_handler(NtQueryValueKey);
        add_handler(NtQueryMultipleValueKey);
        add_handler(NtQueryKey);
        add_handler(NtGetNlsSectionPtr);
        add_handler(NtAccessCheck);
        add_handler(NtCreateKey);
        add_handler(NtSetValueKey);
        add_handler(NtDeleteValueKey);
        add_handler(NtNotifyChangeKey);
        add_handler(NtGetCurrentProcessorNumberEx);
        add_handler(NtGetCurrentProcessorNumber);
        add_handler(NtQueryObject);
        add_handler(NtCompareObjects);
        add_handler(NtQueryAttributesFile);
        add_handler(NtWaitForMultipleObjects);
        add_handler(NtWaitForMultipleObjects32);
        add_handler(NtCreateMutant);
        add_handler(NtReleaseMutant);
        add_handler(NtCreatePrivateNamespace);
        add_handler(NtOpenPrivateNamespace);
        add_handler(NtDeletePrivateNamespace);
        add_handler(NtDuplicateToken);
        add_handler(NtQueryTimerResolution);
        add_handler(NtSetInformationKey);
        add_handler(NtUserGetKeyboardLayout);
        add_handler(NtUserGetKeyboardLayoutList);
        add_handler(NtUserGetKeyboardLayoutName);
        add_handler(NtQueryDirectoryFileEx);
        add_handler(NtQueryDirectoryFile);
        add_handler(NtUserSystemParametersInfo);
        add_handler(NtGetContextThread);
        add_handler(NtYieldExecution);
        add_handler(NtUserModifyUserStartupInfoFlags);
        add_handler(NtUserGetDCEx);
        add_handler(NtUserGetDC);
        add_handler(NtUserGetWindowDC);
        add_handler(NtUserWindowFromDC);
        add_handler(NtUserGetControlBrush);
        add_handler(NtUserGetOemBitmapSize);
        add_handler(NtUserSetCapture);
        add_handler(NtUserReleaseCapture);
        add_handler(NtUserRegisterRawInputDevices);
        add_handler(NtUserGetRawInputData);
        add_handler(NtUserDefSetText);
        add_handler(NtUserSetWindowState);
        add_handler(NtUserClearWindowState);
        add_handler(NtUserDisableProcessWindowsGhosting);
        add_handler(NtUserBitBltSysBmp);
        add_handler(NtUserGetClientRect);
        add_handler(NtUserBeginPaint);
        add_handler(NtUserEndPaint);
        add_handler(NtUserGetDpiForCurrentProcess);
        add_handler(NtReleaseSemaphore);
        add_handler(NtEnumerateKey);
        add_handler(NtEnumerateValueKey);
        add_handler(NtAlpcCreatePort);
        add_handler(NtAlpcConnectPortEx);
        add_handler(NtAlpcConnectPort);
        add_handler(NtAlpcQueryInformation);
        add_handler(NtGetNextThread);
        add_handler(NtSetInformationObject);
        add_handler(NtUserGetCursorPos);
        add_handler(NtUserGetClipCursor);
        add_handler(NtUserTransformPoint);
        add_handler(NtUserShowCursor);
        add_handler(NtUserClipCursor);
        add_handler(NtUserSetCursorPos);
        add_handler(NtUserGetKeyState);
        add_handler(NtUserGetAsyncKeyState);
        add_handler(NtUserReleaseDC);
        add_handler(NtUserFindExistingCursorIcon);
        add_handler(NtUserCreateEmptyCursorObject);
        add_handler(NtUserSetCursorIconData);
        add_handler(NtUserSetCursorIconDataEx);
        add_handler(NtUserGetRequiredCursorSizes);
        add_handler(NtUserDestroyCursor);
        add_handler(NtUserGetCursorFrameInfo);
        add_handler(NtUserGetIconInfo);
        add_handler(NtUserGetIconSize);
        add_handler(NtUserDrawIconEx);
        add_handler(NtUserMessageBeep);
        add_handler(NtSetContextThread);
        add_handler(NtUserFindWindowEx);
        add_handler(NtUserMoveWindow);
        add_handler(NtSystemDebugControl);
        add_handler(NtRequestWaitReplyPort);
        add_handler(NtQueryDefaultLocale);
        add_handler(NtSetTimerResolution);
        add_handler(NtSuspendThread);
        add_handler(NtResumeThread);
        add_handler(NtClearEvent);
        add_handler(NtTraceControl);
        add_handler(NtUserGetProcessUIContextInformation);
        add_handler(NtQueueApcThreadEx2);
        add_handler(NtQueueApcThreadEx);
        add_handler(NtQueueApcThread);
        add_handler(NtCreateUserProcess);
        add_handler(NtCreateNamedPipeFile);
        add_handler(NtFsControlFile);
        add_handler(NtQueryFullAttributesFile);
        add_handler(NtFlushBuffersFile);
        add_handler(NtAreMappedFilesTheSame);
        add_handler(NtUserGetProcessWindowStation);
        add_handler(NtUserCallHwndParam);
        add_handler(NtUserRegisterClassExWOW);
        add_handler(NtUserUnregisterClass);
        add_handler(NtUserSetWindowsHookEx);
        add_handler(NtUserUnhookWindowsHookEx);
        add_handler(NtUserCreateWindowEx);
        add_handler(NtUserShowWindow);
        add_handler(NtUserMessageCall);
        add_handler(NtUserDispatchMessage);
        add_handler(NtUserTranslateMessage);
        add_handler(NtUserGetMessage);
        add_handler(NtUserPeekMessage);
        add_handler(NtUserWaitMessage);
        add_handler(NtUserInvalidateRect);
        add_handler(NtUserValidateRect);
        add_handler(NtUserGetUpdateRect);
        add_handler(NtUserUpdateWindow);
        add_handler(NtUserGetCursorInfo);
        add_handler(NtUserMapVirtualKeyEx);
        add_handler(NtUserToUnicodeEx);
        add_handler(NtUserSetProcessDpiAwarenessContext);
        add_handler(NtUserGetRawInputDeviceList);
        add_handler(NtUserGetRawInputDeviceInfo);
        add_handler(NtUserGetKeyboardType);
        add_handler(NtUserEnumDisplayDevices);
        add_handler(NtUserEnumDisplaySettings);
        add_handler(NtUserChangeDisplaySettings);
        add_handler(NtUserBuildHwndList);
        add_handler(NtUserEnumDisplayMonitors);
        add_handler(NtUserGetDpiForMonitor);
        add_handler(NtUserInheritWindowMonitor);
        add_handler(NtUserSetProp);
        add_handler(NtUserSetProp2);
        add_handler(NtUserGetProp);
        add_handler(NtUserGetProp2);
        add_handler(NtUserRemoveProp);
        add_handler(NtUserChangeWindowMessageFilterEx);
        add_handler(NtUserDestroyWindow);
        add_handler(NtQueryInformationByName);
        add_handler(NtUserSetCursor);
        add_handler(NtUserGetCursor);
        add_handler(NtOpenMutant);
        add_handler(NtOpenTimer);
        add_handler(NtCreateTimer);
        add_handler(NtCreateTimer2);
        add_handler(NtSetTimer);
        add_handler(NtSetTimer2);
        add_handler(NtSetTimerEx);
        add_handler(NtCancelTimer);
        add_handler(NtAssociateWaitCompletionPacket);
        add_handler(NtCancelWaitCompletionPacket);
        add_handler(NtSetWnfProcessNotificationEvent);
        add_handler(NtUnsubscribeWnfStateChange);
        add_handler(NtQuerySecurityObject);
        add_handler(NtQueryEvent);
        add_handler(NtRemoveIoCompletionEx);
        add_handler(NtCreateDebugObject);
        add_handler(NtReleaseWorkerFactoryWorker);
        add_handler(NtAlpcCreateSecurityContext);
        add_handler(NtAlpcDeleteSecurityContext);
        add_handler(NtSetSecurityObject);
        add_handler(NtSetInformationDebugObject);
        add_handler(NtRemoveProcessDebug);
        add_handler(NtNotifyChangeDirectoryFileEx);
        add_handler(NtUserGetHDevName);
        add_handler(NtFlushInstructionCache);
        add_handler(NtUserMapDesktopObject);
        add_handler(NtAlpcSetInformation);
        add_handler(NtUserTransformRect);
        add_handler(NtUserSetParent);
        add_handler(NtUserSetWindowPos);
        add_handler(NtUserSetForegroundWindow);
        add_handler(NtUserGetForegroundWindow);
        add_handler(NtUserSetFocus);
        add_handler(NtUserSetWindowLongPtr);
        add_handler(NtUserSetClassLongPtr);
        add_handler(NtUserSetWindowLong);
        add_handler(NtUserGetAncestor);
        add_handler(NtUserPostMessage);
        add_handler(NtUserPostThreadMessage);
        add_handler(NtUserRedrawWindow);
        add_handler(NtUserGetCPD);
        add_handler(NtUserSetWindowFNID);
        add_handler(NtUserSetDialogPointer);
        add_handler(NtUserSetDialogSystemMenu);
        add_handler(NtUserSetMsgBox);
        add_handler(NtUserEnableWindow);
        add_handler(NtUserDeleteMenu);
        add_handler(NtUserGetSystemMenu);
        add_handler(NtCallbackReturn);
        add_handler(NtUserPostQuitMessage);
        add_handler(NtUserGetClassInfoEx);
        add_handler(NtUserGetClassName);
        add_handler(NtUserCallNoParam);
        add_handler(NtUserGetDisplayConfigBufferSizes);
        add_handler(NtUserQueryDisplayConfig);
        add_handler(NtGdiDdDDIEnumAdapters2);
        add_handler(NtDxgkEnumAdapters3);
        add_handler(NtDxgkGetProperties);
        add_handler(NtGdiDdDDICloseAdapter);
        add_handler(NtGdiDdDDIQueryAdapterInfo);
        add_handler(NtGdiDdDDICreateDevice);
        add_handler(NtGdiDdDDIEscape);
        add_handler(NtGdiDdDDICreateContext);
        add_handler(NtGdiDdDDICreateAllocation);
        add_handler(NtGdiDdDDIQueryResourceInfo);
        add_handler(NtGdiDdDDIOpenResource);
        add_handler(NtGdiDdDDILock);
        add_handler(NtGdiDdDDIGetDisplayModeList);
        add_handler(NtGdiDdDDIGetSharedPrimaryHandle);
        add_handler(NtGdiDdDDIGetDeviceState);
        add_handler(NtGdiDdDDIMarkDeviceAsError);
        add_handler(NtGdiDdDDIGetCachedHybridQueryValue);
        add_handler(NtGdiDdDDICacheHybridQueryValue);
        add_handler(NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport);
        add_handler(NtGdiDdDDIUnlock);
        add_handler(NtGdiDdDDIDestroyAllocation2);
        add_handler(NtGdiDdDDIDestroyAllocation);
        add_handler(NtGdiDdDDIDestroyContext);
        add_handler(NtGdiDdDDIDestroyDevice);
        add_handler(NtGdiDdDDICreateDCFromMemory);
        add_handler(NtGdiDdDDIDestroyDCFromMemory);
        add_handler(NtAllocateLocallyUniqueId);
        add_handler(NtUserAllowSetForegroundWindow);
        add_handler(NtGdiOpenDCW);
        add_handler(NtGdiDdDDIOpenAdapterFromLuid);
        add_handler(NtGdiDdDDIOpenAdapterFromHdc);
        add_handler(NtGdiSelectFont);
        add_handler(NtUserInitThreadCoreMessagingIocp2);
        add_handler(NtUserDrainThreadCoreMessagingCompletions2);
        add_handler(NtUserSetTimer);
        add_handler(NtUserSetSystemTimer);
        add_handler(NtUserKillTimer);
        add_handler(NtUserValidateTimerCallback);
        add_handler(NtAllocateReserveObject);
        add_handler(NtUserMsgWaitForMultipleObjectsEx);
        add_handler(NtUserGetQueueStatusReadonly);
        add_handler(NtUserGetQueueStatus);
        add_handler(NtUserScheduleDispatchNotification);
        add_handler(NtGdiExtCreateRegion);
        add_handler(NtUserSetWindowRgn);
        add_handler(NtUserAlterWindowStyle);
        add_handler(NtUserSetActiveWindow);
        add_handler(NtUserCreateAcceleratorTable);
        add_handler(NtUserDestroyAcceleratorTable);
        add_handler(NtUserCopyAcceleratorTable);
        add_handler(NtUserTranslateAccelerator);
        add_handler(NtGdiSetLayout);
        add_handler(NtGdiGetDCObject);
        add_handler(NtUserCreateMenu);
        add_handler(NtUserThunkedMenuItemInfo);
        add_handler(NtUserIsTouchWindow);
        add_handler(NtUserCreatePopupMenu);
        add_handler(NtUserSetMenu);
        add_handler(NtUserSetMenuDefaultItem);
        add_handler(NtUserEndMenu);
        add_handler(NtUserRemoveMenu);
        add_handler(NtUserDestroyMenu);
        add_handler(NtUserDrawMenuBar);
        add_handler(NtUserSetWindowCompositionAttribute);
        add_handler(NtUserGetWindowPlacement);
        add_handler(NtUserCreateCaret);
        add_handler(NtUserDestroyCaret);
        add_handler(NtUserSetCaretPos);
        add_handler(NtUserShowCaret);
        add_handler(NtUserHideCaret);
        add_handler(NtUserGetObjectInformation);
        add_handler(NtUserQueryWindow);
        add_handler(NtUserSetScrollInfo);
        add_handler(NtUserTrackMouseEvent);
        add_handler(NtGdiGetOutlineTextMetricsInternalW);
        add_handler(NtGdiSetPixel);
        add_handler(NtGdiGetPixel);
        add_handler(NtGdiCreatePaletteInternal);
        add_handler(NtGdiCreateHalftonePalette);
        add_handler(NtGdiDoPalette);
        add_handler(NtUserSelectPalette);
        add_handler(NtGdiCreateDIBSection);
        add_handler(NtUserGetKeyNameText);
        add_handler(NtUserWindowFromPoint);
        add_handler(NtUserSwapMouseButton);
        add_handler(NtUserGetDoubleClickTime);
        add_handler(NtGdiSetIcmMode);
        add_handler(NtUserGetKeyboardState);
        add_handler(NtUserSetKeyboardState);
        add_handler(NtUserModifyWindowTouchCapability);
        add_handler(NtUserGetClipboardSequenceNumber);
        add_handler(NtUserOpenClipboard);
        add_handler(NtUserCloseClipboard);
        add_handler(NtUserEmptyClipboard);
        add_handler(NtUserGetClipboardData);
        add_handler(NtUserConvertMemHandle);
        add_handler(NtUserSetClipboardData);
        add_handler(NtUserGetProcessDpiAwarenessContext);
        add_handler(NtUserAttachThreadInput);
        add_handler(NtUserRegisterTouchHitTestingWindow);
        add_handler(NtUserActivateKeyboardLayout);
        add_handler(NtUserGetGUIThreadInfo);
        add_handler(NtNotifyChangeDirectoryFile);
        add_handler(NtUserChangeWindowMessageFilter);
        add_handler(NtUserSetWinEventHook);
        add_handler(NtUserUnhookWinEvent);
        add_handler(NtUserDisableThreadIme);
        add_handler(NtUserGetPointerDevices);
        add_handler(NtUserHwndQueryRedirectionInfo);

#undef add_handler
    }

    void syscall_dispatcher::add_callbacks()
    {
#define add_callback(syscall, completion_state)                                                                                      \
    do                                                                                                                               \
    {                                                                                                                                \
        this->completion_handlers_[callback_id::syscall] = make_syscall_handler<syscalls::completion_##syscall>();                   \
        syscall_dispatcher::completion_state_factories_[callback_id::syscall] = [] { return std::make_unique<completion_state>(); }; \
    } while (0)

#define add_stateless_callback(syscall)                                                                            \
    do                                                                                                             \
    {                                                                                                              \
        this->completion_handlers_[callback_id::syscall] = make_syscall_handler<syscalls::completion_##syscall>(); \
    } while (0)

        add_stateless_callback(NtUserGetThreadState);
        add_callback(NtUserCreateWindowEx, window_create_state);
        add_callback(NtUserDestroyWindow, window_destroy_state);
        add_callback(NtUserShowWindow, window_show_state);
        add_callback(NtUserMessageCall, message_call_state);
        add_callback(NtUserUpdateWindow, window_update_state);
        add_stateless_callback(NtUserEnumDisplayMonitors);

#undef add_callback
#undef add_stateless_callback
    }

} // namespace sogen
