#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "cpu_context.hpp"
#include "emulator_utils.hpp"
#include "syscall_utils.hpp"

#include <numeric>
#include <cwctype>
#include <algorithm>
#include <utils/string.hpp>
#include <utils/time.hpp>
#include <utils/finally.hpp>

namespace syscalls
{
    // syscalls/event.cpp:
    NTSTATUS handle_NtSetEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtTraceEvent();
    NTSTATUS handle_NtQueryEvent();
    NTSTATUS handle_NtClearEvent(const syscall_context& c, handle event_handle);
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtOpenEvent(const syscall_context& c, emulator_object<uint64_t> event_handle, ACCESS_MASK desired_access,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);

    // syscalls/exception.cpp
    NTSTATUS handle_NtRaiseHardError(const syscall_context& c, NTSTATUS error_status, ULONG number_of_parameters,
                                     emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> unicode_string_parameter_mask,
                                     emulator_object<DWORD> parameters, HARDERROR_RESPONSE_OPTION valid_response_option,
                                     emulator_object<HARDERROR_RESPONSE> response);
    NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                     emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> exception_record,
                                     emulator_object<CONTEXT64> thread_context, BOOLEAN handle_exception);

    // syscalls/file.cpp
    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         ULONG length, FILE_INFORMATION_CLASS info_class);
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
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         uint32_t length, uint32_t info_class, BOOLEAN return_single_entry,
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
    NTSTATUS handle_NtOpenSymbolicLinkObject(const syscall_context& c, emulator_object<handle> link_handle, ACCESS_MASK /*desired_access*/,
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
    NTSTATUS handle_NtFsControlFile(const syscall_context& c, handle event_handle, uint64_t apc_routine, uint64_t app_context,
                                    emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG fs_control_code,
                                    uint64_t input_buffer, ULONG input_buffer_length, uint64_t output_buffer, ULONG output_buffer_length);
    NTSTATUS handle_NtFlushBuffersFile(const syscall_context& c, handle file_handle,
                                       emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/);

    // syscalls/locale.cpp:
    NTSTATUS handle_NtInitializeNlsFiles(const syscall_context& c, emulator_object<uint64_t> base_address,
                                         emulator_object<LCID> default_locale_id,
                                         emulator_object<LARGE_INTEGER> /*default_casing_table_size*/);
    NTSTATUS handle_NtQueryDefaultLocale(const syscall_context&, BOOLEAN /*user_profile*/, emulator_object<LCID> default_locale_id);
    NTSTATUS handle_NtGetNlsSectionPtr(const syscall_context& c, ULONG section_type, ULONG section_data, emulator_pointer /*context_data*/,
                                       emulator_object<uint64_t> section_pointer, emulator_object<ULONG> section_size);
    NTSTATUS handle_NtGetMUIRegistryInfo();
    NTSTATUS handle_NtIsUILanguageComitted();
    NTSTATUS handle_NtUserGetKeyboardLayout();
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
                                              uint32_t page_protection);
    NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                            uint64_t zero_bits, emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                            uint32_t page_protection);
    NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                        emulator_object<uint64_t> bytes_to_allocate, uint32_t free_type);
    NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, handle process_handle, emulator_pointer base_address,
                                        emulator_pointer buffer, ULONG number_of_bytes_to_read,
                                        emulator_object<ULONG> number_of_bytes_read);
    NTSTATUS handle_NtSetInformationVirtualMemory();

    // syscalls/mutant.cpp:
    NTSTATUS handle_NtReleaseMutant(const syscall_context& c, handle mutant_handle, emulator_object<LONG> previous_count);
    NTSTATUS handle_NtOpenMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
    NTSTATUS handle_NtCreateMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                   emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, BOOLEAN initial_owner);

    // syscalls/object.cpp:
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtDuplicateObject(const syscall_context& c, handle source_process_handle, handle source_handle,
                                      handle target_process_handle, emulator_object<handle> target_handle, ACCESS_MASK desired_access,
                                      ULONG handle_attributes, ULONG options);
    NTSTATUS handle_NtQueryObject(const syscall_context& c, handle handle, OBJECT_INFORMATION_CLASS object_information_class,
                                  emulator_pointer object_information, ULONG object_information_length,
                                  emulator_object<ULONG> return_length);
    NTSTATUS handle_NtWaitForMultipleObjects(const syscall_context& c, ULONG count, emulator_object<handle> handles, WAIT_TYPE wait_type,
                                             BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, handle h, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtSetInformationObject();
    NTSTATUS handle_NtQuerySecurityObject(const syscall_context& c, handle /*h*/, SECURITY_INFORMATION /*security_information*/,
                                          emulator_pointer security_descriptor, ULONG length, emulator_object<ULONG> length_needed);

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
    NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, handle port_handle, ULONG /*flags*/,
                                              emulator_object<PORT_MESSAGE64> /*send_message*/,
                                              emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*send_message_attributes*/,
                                              emulator_object<PORT_MESSAGE64> receive_message,
                                              emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                              emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*receive_message_attributes*/,
                                              emulator_object<LARGE_INTEGER> /*timeout*/);
    NTSTATUS handle_NtAlpcConnectPort();
    NTSTATUS handle_NtAlpcConnectPortEx();

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

    // syscalls/registry.cpp:
    NTSTATUS handle_NtOpenKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK /*desired_access*/,
                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
    NTSTATUS handle_NtOpenKeyEx(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG /*open_options*/);
    NTSTATUS handle_NtQueryKey(const syscall_context& c, handle key_handle, KEY_INFORMATION_CLASS key_information_class,
                               uint64_t key_information, ULONG length, emulator_object<ULONG> result_length);
    NTSTATUS handle_NtQueryValueKey(const syscall_context& c, handle key_handle,
                                    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                    KEY_VALUE_INFORMATION_CLASS key_value_information_class, uint64_t key_value_information, ULONG length,
                                    emulator_object<ULONG> result_length);
    NTSTATUS handle_NtCreateKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG /*title_index*/,
                                emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*class*/, ULONG /*create_options*/,
                                emulator_object<ULONG> /*disposition*/);
    NTSTATUS handle_NtNotifyChangeKey();
    NTSTATUS handle_NtSetInformationKey();
    NTSTATUS handle_NtEnumerateKey(const syscall_context& c, handle key_handle, ULONG index, KEY_INFORMATION_CLASS key_information_class,
                                   uint64_t key_information, ULONG length, emulator_object<ULONG> result_length);
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
    NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, handle section_handle, handle process_handle,
                                       emulator_object<uint64_t> base_address,
                                       EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) /*zero_bits*/,
                                       EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) /*commit_size*/,
                                       emulator_object<LARGE_INTEGER> /*section_offset*/,
                                       emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                       SECTION_INHERIT /*inherit_disposition*/, ULONG /*allocation_type*/, ULONG /*win32_protect*/);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, handle process_handle, uint64_t base_address);
    NTSTATUS handle_NtUnmapViewOfSectionEx(const syscall_context& c, handle process_handle, uint64_t base_address, ULONG /*flags*/);
    NTSTATUS handle_NtAreMappedFilesTheSame();

    // syscalls/semaphore.cpp:
    NTSTATUS handle_NtOpenSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK /*desired_access*/,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
    NTSTATUS handle_NtReleaseSemaphore(const syscall_context& c, handle semaphore_handle, ULONG release_count,
                                       emulator_object<LONG> previous_count);
    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK /*desired_access*/,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG initial_count,
                                      ULONG maximum_count);

    // syscalls/system.cpp:
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, uint32_t info_class, uint64_t input_buffer,
                                               uint32_t input_buffer_length, uint64_t system_information,
                                               uint32_t system_information_length, emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtSetSystemInformation();

    // syscalls/thread.cpp:
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                           uint64_t thread_information, uint32_t thread_information_length);

    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                             uint64_t thread_information, uint32_t thread_information_length,
                                             emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtOpenThread(const syscall_context&, handle thread_handle, ACCESS_MASK /*desired_access*/,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                 emulator_pointer /*client_id*/);
    NTSTATUS handle_NtOpenThreadToken(const syscall_context&, handle thread_handle, ACCESS_MASK /*desired_access*/,
                                      BOOLEAN /*open_as_self*/, emulator_object<handle> token_handle);
    NTSTATUS handle_NtOpenThreadTokenEx(const syscall_context& c, handle thread_handle, ACCESS_MASK desired_access, BOOLEAN open_as_self,
                                        ULONG /*handle_attributes*/, emulator_object<handle> token_handle);
    NTSTATUS handle_NtTerminateThread(const syscall_context& c, handle thread_handle, NTSTATUS exit_status);
    NTSTATUS handle_NtDelayExecution(const syscall_context& c, BOOLEAN alertable, emulator_object<LARGE_INTEGER> delay_interval);
    NTSTATUS handle_NtAlertThreadByThreadId(const syscall_context& c, uint64_t thread_id);
    NTSTATUS handle_NtAlertThreadByThreadIdEx(const syscall_context& c, uint64_t thread_id,
                                              emulator_object<EMU_RTL_SRWLOCK<EmulatorTraits<Emu64>>> lock);
    NTSTATUS handle_NtWaitForAlertByThreadId(const syscall_context& c, uint64_t, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtYieldExecution(const syscall_context& c);
    NTSTATUS handle_NtResumeThread(const syscall_context& c, handle thread_handle, emulator_object<ULONG> previous_suspend_count);
    NTSTATUS handle_NtContinue(const syscall_context& c, emulator_object<CONTEXT64> thread_context, BOOLEAN raise_alert);
    NTSTATUS handle_NtContinueEx(const syscall_context& c, emulator_object<CONTEXT64> thread_context, uint64_t continue_argument);
    NTSTATUS handle_NtGetNextThread(const syscall_context& c, handle process_handle, handle thread_handle, ACCESS_MASK /*desired_access*/,
                                    ULONG /*handle_attributes*/, ULONG flags, emulator_object<handle> new_thread_handle);
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtCreateThreadEx(const syscall_context& c, emulator_object<handle> thread_handle, ACCESS_MASK /*desired_access*/,
                                     emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                     /*object_attributes*/,
                                     handle process_handle, uint64_t start_routine, uint64_t argument, ULONG create_flags,
                                     EmulatorTraits<Emu64>::SIZE_T /*zero_bits*/, EmulatorTraits<Emu64>::SIZE_T stack_size,
                                     EmulatorTraits<Emu64>::SIZE_T /*maximum_stack_size*/,
                                     emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list);
    NTSTATUS handle_NtGetCurrentProcessorNumberEx(const syscall_context&, emulator_object<PROCESSOR_NUMBER> processor_number);
    ULONG handle_NtGetCurrentProcessorNumber();
    NTSTATUS handle_NtQueueApcThreadEx2(const syscall_context& c, handle thread_handle, handle reserve_handle, uint32_t apc_flags,
                                        uint64_t apc_routine, uint64_t apc_argument1, uint64_t apc_argument2, uint64_t apc_argument3);
    NTSTATUS handle_NtQueueApcThreadEx(const syscall_context& c, handle thread_handle, handle reserve_handle, uint64_t apc_routine,
                                       uint64_t apc_argument1, uint64_t apc_argument2, uint64_t apc_argument3);
    NTSTATUS handle_NtQueueApcThread(const syscall_context& c, handle thread_handle, uint64_t apc_routine, uint64_t apc_argument1,
                                     uint64_t apc_argument2, uint64_t apc_argument3);

    // syscalls/timer.cpp:
    NTSTATUS handle_NtQueryTimerResolution(const syscall_context&, emulator_object<ULONG> maximum_time, emulator_object<ULONG> minimum_time,
                                           emulator_object<ULONG> current_time);
    NTSTATUS handle_NtSetTimerResolution(const syscall_context&, ULONG /*desired_resolution*/, BOOLEAN set_resolution,
                                         emulator_object<ULONG> current_resolution);
    NTSTATUS handle_NtCreateTimer2(const syscall_context& c, emulator_object<handle> timer_handle, uint64_t reserved,
                                   emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG attributes,
                                   ACCESS_MASK desired_access);
    NTSTATUS handle_NtCreateTimer(const syscall_context& c, emulator_object<handle> timer_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG timer_type);
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
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle, TOKEN_INFORMATION_CLASS token_information_class,
                                            uint64_t token_information, ULONG token_information_length,
                                            emulator_object<ULONG> return_length);
    NTSTATUS handle_NtQuerySecurityAttributesToken();

    NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context& c, const emulator_object<LARGE_INTEGER> performance_counter,
                                              const emulator_object<LARGE_INTEGER> performance_frequency)
    {
        try
        {
            if (performance_counter)
            {
                performance_counter.access(
                    [&](LARGE_INTEGER& value) { value.QuadPart = c.win_emu.clock().steady_now().time_since_epoch().count(); });
            }

            if (performance_frequency)
            {
                performance_frequency.access([&](LARGE_INTEGER& value) {
                    value.QuadPart = c.proc.kusd.get().QpcFrequency; //
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

    NTSTATUS handle_NtCreateWorkerFactory()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetInformationWorkerFactory()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtShutdownWorkerFactory()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtReleaseWorkerFactoryWorker()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateIoCompletion(const syscall_context& c, const emulator_object<handle> event_handle,
                                         const ACCESS_MASK desired_access,
                                         const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                         const uint32_t /*number_of_concurrent_threads*/)
    {
        return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
    }

    NTSTATUS handle_NtSetIoCompletion()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtRemoveIoCompletion(const syscall_context&, const emulator_object<handle> /*io_completion__handle*/,
                                         const emulator_object<int64_t> key_context, const emulator_pointer /*apc_context*/,
                                         const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/,
                                         const emulator_object<LARGE_INTEGER> timeout)
    {
        if (timeout && timeout.read().QuadPart == 0)
        {
            return STATUS_TIMEOUT;
        }

        key_context.write_if_valid(-1);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtRemoveIoCompletionEx()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtCreateWaitCompletionPacket(const syscall_context& c, const emulator_object<handle> event_handle,
                                                 const ACCESS_MASK desired_access,
                                                 const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
    }

    NTSTATUS handle_NtApphelpCacheControl()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtDeviceIoControlFile(const syscall_context& c, const handle file_handle, const handle event,
                                          const emulator_pointer /*PIO_APC_ROUTINE*/ apc_routine, const emulator_pointer apc_context,
                                          const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                          const ULONG io_control_code, const emulator_pointer input_buffer, const ULONG input_buffer_length,
                                          const emulator_pointer output_buffer, const ULONG output_buffer_length)
    {
        auto* device = c.proc.devices.get(file_handle);
        if (!device)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (auto* e = c.win_emu.process.events.get(event))
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

        return device->execute_ioctl(c.win_emu, context);
    }

    NTSTATUS handle_NtQueryWnfStateData()
    {
        // puts("NtQueryWnfStateData not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryWnfStateNameInformation()
    {
        // puts("NtQueryWnfStateNameInformation not supported");
        // return STATUS_NOT_SUPPORTED;
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryLicenseValue()
    {
        // puts("NtQueryLicenseValue not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtTestAlert(const syscall_context& c)
    {
        c.win_emu.yield_thread(true);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserSystemParametersInfo()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtDxgkIsFeatureEnabled()
    {
        // puts("NtDxgkIsFeatureEnabled not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo()
    {
        // puts("NtUserDisplayConfigGetDeviceInfo not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtGdiInit(const syscall_context& c)
    {
        c.proc.peb.access([&](PEB64& peb) {
            if (!peb.GdiSharedHandleTable)
            {
                const auto shared_memory = c.proc.base_allocator.reserve<GDI_SHARED_MEMORY64>();

                shared_memory.access([](GDI_SHARED_MEMORY64& mem) {
                    mem.Objects[0x12] = 1;
                    mem.Objects[0x13] = 1;
                });

                peb.GdiSharedHandleTable = shared_memory.value();
            }
        });

        return STATUS_WAIT_1;
    }

    NTSTATUS handle_NtGdiInit2(const syscall_context& c)
    {
        handle_NtGdiInit(c);
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserRegisterWindowMessage()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetThreadState()
    {
        return 0;
    }

    NTSTATUS handle_NtUpdateWnfStateData()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryInformationJobObject()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtAccessCheck()
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

    NTSTATUS handle_NtAddAtom(const syscall_context& c, const uint64_t atom_name, const ULONG length, const emulator_object<RTL_ATOM> atom)
    {
        return handle_NtAddAtomEx(c, atom_name, length, atom, 0);
    }

    NTSTATUS handle_NtDeleteAtom(const syscall_context& c, const RTL_ATOM atom)
    {
        c.proc.delete_atom(atom);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtFindAtom(const syscall_context& c, const uint64_t atom_name, const ULONG length, const emulator_object<uint16_t> atom)
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

    NTSTATUS handle_NtUserGetAtomName(const syscall_context& c, const RTL_ATOM atom,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> atom_name)
    {
        const auto* name = c.proc.get_atom_name(atom);
        if (!name)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const size_t name_length = name->size() * 2;
        const size_t max_length = name_length + 2;

        bool too_small = false;
        atom_name.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& str) {
            if (str.MaximumLength < max_length)
            {
                too_small = true;
                return;
            }

            str.Length = static_cast<USHORT>(name_length);
            c.emu.write_memory(str.Buffer, name->data(), max_length);
        });

        return too_small ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryDebugFilterState()
    {
        return FALSE;
    }

    NTSTATUS handle_NtUserGetDpiForCurrentProcess()
    {
        return 96;
    }

    hdc handle_NtUserGetDCEx(const syscall_context& /*c*/, const hwnd window, const uint64_t /*clip_region*/, const ULONG /*flags*/)
    {
        return window;
    }

    hdc handle_NtUserGetDC(const syscall_context& c, const hwnd window)
    {
        return handle_NtUserGetDCEx(c, window, 0, 0);
    }

    NTSTATUS handle_NtUserGetWindowDC()
    {
        return 1;
    }

    NTSTATUS handle_NtUserReleaseDC()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserModifyUserStartupInfoFlags()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserGetCursorPos()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserSetCursor()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserFindExistingCursorIcon()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtSystemDebugControl()
    {
        return STATUS_DEBUGGER_INACTIVE;
    }

    NTSTATUS handle_NtRequestWaitReplyPort()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtTraceControl()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetProcessUIContextInformation()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserFindWindowEx()
    {
        return 0;
    }

    NTSTATUS handle_NtUserMoveWindow()
    {
        return 0;
    }

    NTSTATUS handle_NtUserGetProcessWindowStation()
    {
        return 0;
    }

    template <typename Traits>
    struct CLSMENUNAME
    {
        EMULATOR_CAST(typename Traits::PVOID, char*) pszClientAnsiMenuName;
        EMULATOR_CAST(typename Traits::PVOID, char16_t*) pwszClientUnicodeMenuName;
        EMULATOR_CAST(typename Traits::PVOID, UNICODE_STRING*) pusMenuName;
    };

    NTSTATUS handle_NtUserRegisterClassExWOW(const syscall_context& c, const emulator_pointer /*wnd_class_ex*/,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*class_version*/,
                                             const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> /*class_menu_name*/,
                                             const DWORD /*function_id*/, const DWORD /*flags*/, const emulator_pointer /*wow*/)
    {
        uint16_t index = c.proc.add_or_find_atom(read_unicode_string(c.emu, class_name));
        return index;
    }

    NTSTATUS handle_NtUserUnregisterClass(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                          const emulator_pointer /*instance*/,
                                          const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> /*class_menu_name*/)
    {
        return c.proc.delete_atom(read_unicode_string(c.emu, class_name));
    }

    NTSTATUS handle_NtUserSetWindowsHookEx()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserUnhookWindowsHookEx()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserMapVirtualKeyEx()
    {
        return 0;
    }

    NTSTATUS handle_NtUserToUnicodeEx()
    {
        return 0;
    }

    NTSTATUS handle_NtUserSetProcessDpiAwarenessContext()
    {
        return 0;
    }

    std::u16string read_large_string(const emulator_object<LARGE_STRING> str_obj)
    {
        if (!str_obj)
        {
            return {};
        }

        const auto str = str_obj.read();
        if (!str.bAnsi)
        {
            return read_string<char16_t>(*str_obj.get_memory_interface(), str.Buffer, str.Length / 2);
        }

        const auto ansi_string = read_string<char>(*str_obj.get_memory_interface(), str.Buffer, str.Length);
        return u8_to_u16(ansi_string);
    }

    hwnd handle_NtUserCreateWindowEx(const syscall_context& c, const DWORD /*ex_style*/, const emulator_object<LARGE_STRING> class_name,
                                     const emulator_object<LARGE_STRING> /*cls_version*/, const emulator_object<LARGE_STRING> window_name,
                                     const DWORD /*style*/, const int x, const int y, const int width, const int height,
                                     const hwnd /*parent*/, const hmenu /*menu*/, const hinstance /*instance*/, const pointer /*l_param*/,
                                     const DWORD /*flags*/, const pointer /*acbi_buffer*/)
    {
        window win{};
        win.x = x;
        win.y = y;
        win.width = width;
        win.height = height;
        win.thread_id = c.win_emu.current_thread().id;
        win.class_name = read_large_string(class_name);
        win.name = read_large_string(window_name);

        return c.proc.windows.store(std::move(win)).bits;
    }

    BOOL handle_NtUserDestroyWindow(const syscall_context& c, const hwnd window)
    {
        return c.proc.windows.erase(window);
    }

    BOOL handle_NtUserSetProp(const syscall_context& c, const hwnd window, const uint16_t atom, const uint64_t data)
    {
        auto* win = c.proc.windows.get(window);
        const auto* prop = c.proc.get_atom_name(atom);

        if (!win || !prop)
        {
            return FALSE;
        }

        win->props[*prop] = data;

        return TRUE;
    }

    BOOL handle_NtUserSetProp2(const syscall_context& c, const hwnd window,
                               const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str, const uint64_t data)
    {
        auto* win = c.proc.windows.get(window);
        if (!win || !str)
        {
            return FALSE;
        }

        auto prop = read_unicode_string(c.emu, str);
        win->props[std::move(prop)] = data;

        return TRUE;
    }

    ULONG handle_NtUserGetRawInputDeviceList()
    {
        return 0;
    }

    ULONG handle_NtUserGetKeyboardType()
    {
        return 0;
    }

    uint64_t handle_NtUserChangeWindowMessageFilterEx()
    {
        return 0;
    }

    BOOL handle_NtUserShowWindow(const syscall_context& c, const hwnd hwnd, const LONG cmd_show)
    {
        (void)c;
        (void)hwnd;
        (void)cmd_show;
        return TRUE;
    }

    BOOL handle_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd, const UINT msg_filter_min,
                                 const UINT msg_filter_max)
    {
        (void)c;
        (void)message;
        (void)hwnd;
        (void)msg_filter_min;
        (void)msg_filter_max;

        return TRUE;
    }

    BOOL handle_NtUserPeekMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd, const UINT msg_filter_min,
                                  const UINT msg_filter_max, const UINT remove_message)
    {
        (void)c;
        (void)message;
        (void)hwnd;
        (void)msg_filter_min;
        (void)msg_filter_max;
        (void)remove_message;

        return FALSE;
    }

    NTSTATUS handle_NtUserEnumDisplayDevices(const syscall_context& /*c*/,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str_device, const DWORD dev_num,
                                             const emulator_object<EMU_DISPLAY_DEVICEW> display_device, const DWORD /*flags*/)
    {
        if (str_device && dev_num != 0)
        {
            return STATUS_UNSUCCESSFUL;
        }

        if (dev_num > 0)
        {
            return STATUS_UNSUCCESSFUL;
        }

        display_device.access([&](EMU_DISPLAY_DEVICEW& dev) {
            dev.StateFlags = 0;
            utils::string::copy(dev.DeviceName, u"\\\\.\\DISPLAY1");
            utils::string::copy(dev.DeviceID, u"PCI\\VEN_10DE&DEV_0000&SUBSYS_00000000&REV_A1");
            utils::string::copy(dev.DeviceString, u"Emulator Display");
            utils::string::copy(dev.DeviceKey, u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\{00000001-"
                                               u"0002-0003-0004-000000000005}\\0001");
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAssociateWaitCompletionPacket()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCancelWaitCompletionPacket()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetWnfProcessNotificationEvent()
    {
        return STATUS_NOT_SUPPORTED;
    }
}

void syscall_dispatcher::add_handlers(std::map<std::string, syscall_handler>& handler_mapping)
{
#define add_handler(syscall)                                                            \
    do                                                                                  \
    {                                                                                   \
        handler_mapping[#syscall] = make_syscall_handler<syscalls::handle_##syscall>(); \
    } while (0)

    add_handler(NtSetInformationThread);
    add_handler(NtSetEvent);
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
    add_handler(NtCreateEvent);
    add_handler(NtProtectVirtualMemory);
    add_handler(NtOpenDirectoryObject);
    add_handler(NtTraceEvent);
    add_handler(NtAllocateVirtualMemoryEx);
    add_handler(NtCreateIoCompletion);
    add_handler(NtSetIoCompletion);
    add_handler(NtRemoveIoCompletion);
    add_handler(NtCreateWaitCompletionPacket);
    add_handler(NtCreateWorkerFactory);
    add_handler(NtSetInformationWorkerFactory);
    add_handler(NtShutdownWorkerFactory);
    add_handler(NtManageHotPatch);
    add_handler(NtOpenSection);
    add_handler(NtMapViewOfSection);
    add_handler(NtOpenSymbolicLinkObject);
    add_handler(NtQuerySymbolicLinkObject);
    add_handler(NtQuerySystemInformationEx);
    add_handler(NtOpenFile);
    add_handler(NtQueryVolumeInformationFile);
    add_handler(NtApphelpCacheControl);
    add_handler(NtCreateSection);
    add_handler(NtConnectPort);
    add_handler(NtSecureConnectPort);
    add_handler(NtCreateFile);
    add_handler(NtDeviceIoControlFile);
    add_handler(NtQueryWnfStateData);
    add_handler(NtOpenProcess);
    add_handler(NtOpenProcessToken);
    add_handler(NtOpenProcessTokenEx);
    add_handler(NtQuerySecurityAttributesToken);
    add_handler(NtQueryLicenseValue);
    add_handler(NtTestAlert);
    add_handler(NtContinue);
    add_handler(NtContinueEx);
    add_handler(NtTerminateProcess);
    add_handler(NtWriteFile);
    add_handler(NtRaiseHardError);
    add_handler(NtCreateSemaphore);
    add_handler(NtOpenSemaphore);
    add_handler(NtReadVirtualMemory);
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
    add_handler(NtGdiInit2);
    add_handler(NtUserGetThreadState);
    add_handler(NtOpenKeyEx);
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
    add_handler(NtQueryKey);
    add_handler(NtGetNlsSectionPtr);
    add_handler(NtAccessCheck);
    add_handler(NtCreateKey);
    add_handler(NtNotifyChangeKey);
    add_handler(NtGetCurrentProcessorNumberEx);
    add_handler(NtGetCurrentProcessorNumber);
    add_handler(NtQueryObject);
    add_handler(NtQueryAttributesFile);
    add_handler(NtWaitForMultipleObjects);
    add_handler(NtCreateMutant);
    add_handler(NtReleaseMutant);
    add_handler(NtDuplicateToken);
    add_handler(NtQueryTimerResolution);
    add_handler(NtSetInformationKey);
    add_handler(NtUserGetKeyboardLayout);
    add_handler(NtQueryDirectoryFileEx);
    add_handler(NtQueryDirectoryFile);
    add_handler(NtUserSystemParametersInfo);
    add_handler(NtGetContextThread);
    add_handler(NtYieldExecution);
    add_handler(NtUserModifyUserStartupInfoFlags);
    add_handler(NtUserGetDCEx);
    add_handler(NtUserGetDC);
    add_handler(NtUserGetWindowDC);
    add_handler(NtUserGetDpiForCurrentProcess);
    add_handler(NtReleaseSemaphore);
    add_handler(NtEnumerateKey);
    add_handler(NtEnumerateValueKey);
    add_handler(NtAlpcConnectPort);
    add_handler(NtAlpcConnectPortEx);
    add_handler(NtGetNextThread);
    add_handler(NtSetInformationObject);
    add_handler(NtUserGetCursorPos);
    add_handler(NtUserReleaseDC);
    add_handler(NtUserFindExistingCursorIcon);
    add_handler(NtSetContextThread);
    add_handler(NtUserFindWindowEx);
    add_handler(NtUserMoveWindow);
    add_handler(NtSystemDebugControl);
    add_handler(NtRequestWaitReplyPort);
    add_handler(NtQueryDefaultLocale);
    add_handler(NtSetTimerResolution);
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
    add_handler(NtUserRegisterClassExWOW);
    add_handler(NtUserUnregisterClass);
    add_handler(NtUserSetWindowsHookEx);
    add_handler(NtUserUnhookWindowsHookEx);
    add_handler(NtUserCreateWindowEx);
    add_handler(NtUserShowWindow);
    add_handler(NtUserGetMessage);
    add_handler(NtUserPeekMessage);
    add_handler(NtUserMapVirtualKeyEx);
    add_handler(NtUserToUnicodeEx);
    add_handler(NtUserSetProcessDpiAwarenessContext);
    add_handler(NtUserGetRawInputDeviceList);
    add_handler(NtUserGetKeyboardType);
    add_handler(NtUserEnumDisplayDevices);
    add_handler(NtUserSetProp);
    add_handler(NtUserSetProp2);
    add_handler(NtUserChangeWindowMessageFilterEx);
    add_handler(NtUserDestroyWindow);
    add_handler(NtQueryInformationByName);
    add_handler(NtUserSetCursor);
    add_handler(NtOpenMutant);
    add_handler(NtCreateTimer);
    add_handler(NtCreateTimer2);
    add_handler(NtSetTimer);
    add_handler(NtSetTimer2);
    add_handler(NtSetTimerEx);
    add_handler(NtCancelTimer);
    add_handler(NtAssociateWaitCompletionPacket);
    add_handler(NtCancelWaitCompletionPacket);
    add_handler(NtSetWnfProcessNotificationEvent);
    add_handler(NtQuerySecurityObject);
    add_handler(NtQueryEvent);
    add_handler(NtRemoveIoCompletionEx);
    add_handler(NtCreateDebugObject);
    add_handler(NtReleaseWorkerFactoryWorker);

#undef add_handler
}
