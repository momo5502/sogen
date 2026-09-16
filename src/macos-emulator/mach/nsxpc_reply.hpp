#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::nsxpc
{
    // An ObjC method signature carries a byte offset after every type; the copy NSXPC embeds in a reply
    // carries none. Measured 2026-08-28: the request's "replysig" is
    // v44@?0@8@"FSNode"16@"NSXPCListenerEndpoint"24B32@"NSError"36 and the reply's own signature string is
    // v@?@@"FSNode"@"NSXPCListenerEndpoint"B@"NSError".
    std::string strip_type_encoding_offsets(std::string_view signature);

    // The reply block's arguments after the block itself, in signature order.
    std::optional<std::vector<std::string>> reply_argument_types(std::string_view stripped_signature);

    // The selector an NSXPC invocation names, read out of the bplist17 payload the request carries in
    // its "root" entry. Element 0 of the top-level array is the selector on a request and nil on a reply.
    std::optional<std::string> invocation_selector(const std::vector<uint8_t>& root);

    // The bplist17 payload of an NSXPC reply that ran the client's reply block with every argument at its
    // zero value: nil for objects, NO for BOOL, 0 for integers. Nullopt when an argument type has no
    // measured encoding, so the caller can report it rather than guess.
    std::optional<std::vector<uint8_t>> empty_reply_root(std::string_view reply_signature, std::string* unsupported_type);
}
