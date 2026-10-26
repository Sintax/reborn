#pragma once

// Public half of the update-signing key (ECDSA P-256, X||Y, base64).
// The private half stays on the owner's laptop and is never committed.
namespace UpdateKey {
constexpr const char* PublicKey = "nBQ0IX3S4w+BnlL6F4lKw//L2yWNKRb7b5Kolh7oT+LtL97XptYJvrtMXdNgkbSZN+CqwjxbpHn9X7X33ecx6w==";
}
